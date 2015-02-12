/*
 * Copyright (c) 2007 Bobby Bingham
 *
 * This custom video filter implements unwarping of the lens distortion
 * correction and chromatic abberation correction performed by virtual
 * reality head-mounted displays such as the Oculus Rift. This allows
 * recordings of virtual reality software to be viewed normally on the
 * monitor.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * unwarpvr video filter
 */

#include <stdio.h>
#include <string.h>
#include <jansson.h>

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/avassert.h"
#include "libswscale/swscale.h"

#ifdef _WIN32
#include <windows.h>
const char *unexpanded_profile_path = "%USERPROFILE%\\AppData\\Local\\Oculus\\ProfileDB.json";
#else
#include <wordexp.h>
const char *unexpanded_profile_path = "~/Library/Preferences/Oculus/ProfileDB.json";
#endif

static const char *const var_names[] = {
    "in_w",   "iw",
    "in_h",   "ih",
    "out_w",  "ow",
    "out_h",  "oh",
    "a",
    "sar",
    "dar",
    "hsub",
    "vsub",
    "ohsub",
    "ovsub",
    NULL
};

enum var_name {
    VAR_IN_W,   VAR_IW,
    VAR_IN_H,   VAR_IH,
    VAR_OUT_W,  VAR_OW,
    VAR_OUT_H,  VAR_OH,
    VAR_A,
    VAR_SAR,
    VAR_DAR,
    VAR_HSUB,
    VAR_VSUB,
    VAR_OHSUB,
    VAR_OVSUB,
    VARS_NB
};

typedef struct UnwarpVRContext {
    const AVClass *class;
    struct SwsContext *sws;     ///< software scaler context
    struct SwsContext *isws[2]; ///< software scaler context for interlaced material
    AVDictionary *opts;

    /**
     * New dimensions. Special values are:
     *   0 = original width/height
     *  -1 = keep original aspect
     *  -N = try to keep aspect but make sure it is divisible by N
     */
    int w, h;
    char *size_str;
    unsigned int flags;         ///sws flags

    int hsub, vsub;             ///< chroma subsampling
    int slice_y;                ///< top of current output slice
    int input_is_pal;           ///< set to 1 if the input format is paletted
    int output_is_pal;          ///< set to 1 if the output format is paletted
    int interlaced;

    char *w_expr;               ///< width  expression string
    char *h_expr;               ///< height expression string
    char *flags_str;

    char *in_color_matrix;
    char *out_color_matrix;

    int in_range;
    int out_range;

    int out_h_chr_pos;
    int out_v_chr_pos;
    int in_h_chr_pos;
    int in_v_chr_pos;

    int force_original_aspect_ratio;
    int swap_eyes;
    int left_eye_only;
    float scale_width;
    float scale_height;
    float scale_in_width;
    float scale_in_height;

    int eye_relief_dial;
    int forward_warp;
    float ppd;
    char *device;
    char *sdkversion;
    int mono_input;

    int* inv_cache;
} UnwarpVRContext;

static av_cold int ovr_parse_error(AVFilterContext *ctx, json_t *root, const char *reason)
{
    av_log(ctx, AV_LOG_ERROR,
        "Error encountered parsing Oculus SDK profile (%s). Set eye relief manually with eye_relief_dial option.\n", reason);
    json_decref(root);
    return AVERROR(EINVAL);
}

static av_cold int read_ovr_profile(AVFilterContext *ctx)
{
    UnwarpVRContext *unwarpvr = ctx->priv;
    int i, j;
    int found_user_profile = 0;
    json_t *root, *json, *tagged_data;
    json_error_t error;
    const char* default_user = NULL;
    const char* selected_product = unwarpvr->device;

#ifdef _WIN32
    char profile_path[FILENAME_MAX + 1];
    ExpandEnvironmentStrings(unexpanded_profile_path, profile_path, sizeof(profile_path) / sizeof(*profile_path));
#else
    wordexp_t exp_result;
    wordexp(unexpanded_profile_path, &exp_result, 0);
    char* profile_path = exp_result.we_wordv[0]; //""; // No Oculus SDK for Linux for DK2 yet. TODO: Make this work for DK1 at least.
#endif

    // Default settings if not specified in JSON
    unwarpvr->eye_relief_dial = 3;

    root = json_load_file(profile_path, 0, &error);
    if (!root) {
        av_log(ctx, AV_LOG_ERROR,
            "Could not find Oculus SDK profile. Oculus Runtime may not be installed. Set eye relief manually with eye_relief_dial option.\n");
        return AVERROR(EINVAL);
    }
    if (!json_is_object(root))
        return ovr_parse_error(ctx, root, "root is not object");
    tagged_data = json_object_get(root, "TaggedData");
    if (!json_is_array(tagged_data))
        return ovr_parse_error(ctx, root, "TaggedData is not array");
    for (i = 0; i < json_array_size(tagged_data); i++)
    {
        json_t *tags, *vals;
        json = json_array_get(tagged_data, i);
        if (!json_is_object(json))
            return ovr_parse_error(ctx, root, "TaggedData element is not object");
        tags = json_object_get(json, "tags");
        vals = json_object_get(json, "vals");
        if (!json_is_array(tags))
            return ovr_parse_error(ctx, root, "tags is not array");
        if (!json_is_object(vals))
            return ovr_parse_error(ctx, root, "vals is not object");
        for (j = 0; j < json_array_size(tags); j++)
        {
            json_t *product;
            json = json_array_get(tags, j);
            if (!json_is_object(json))
                return ovr_parse_error(ctx, root, "tags element is not object");
            product = json_object_get(json, "Product");
            if (json_is_string(product) && strcmp(json_string_value(product), selected_product) == 0)
            {
                json_t *default_user_json = json_object_get(vals, "DefaultUser");
                if (json_is_string(default_user_json))
                {
                    const char* default_user_here = json_string_value(default_user_json);
                    if (default_user == NULL)
                    {
                        default_user = default_user_here;
                    }
                    else if (strcmp(default_user, default_user_here) != 0)
                    {
                        return ovr_parse_error(ctx, root, "two matching devices with different default users");
                    }
                }
            }
        }
    }
    if (default_user == NULL)
        return ovr_parse_error(ctx, root, "could not find default user for selected device");
    av_log(ctx, AV_LOG_INFO, "Reading profile settings from Oculus SDK user '%s'\n", default_user);

    found_user_profile = 0;
    for (i = 0; i < json_array_size(tagged_data); i++)
    {
        json_t *tags, *vals;
        int matched_user = 0, matched_product = 0;

        json = json_array_get(tagged_data, i);
        if (!json_is_object(json))
            return ovr_parse_error(ctx, root, "TaggedData element is not object");
        tags = json_object_get(json, "tags");
        vals = json_object_get(json, "vals");
        if (!json_is_array(tags))
            return ovr_parse_error(ctx, root, "tags is not array");
        if (!json_is_object(vals))
            return ovr_parse_error(ctx, root, "vals is not object");
        for (j = 0; j < json_array_size(tags); j++)
        {
            json_t *product, *user;
            json = json_array_get(tags, j);
            if (!json_is_object(json))
                return ovr_parse_error(ctx, root, "tags element is not object");
            user = json_object_get(json, "User");
            if (json_is_string(user) && strcmp(json_string_value(user), default_user) == 0)
                matched_user = 1;
            product = json_object_get(json, "Product");
            if (json_is_string(product) && strcmp(json_string_value(product), selected_product) == 0)
                matched_product = 1;
        }
        if (matched_user && matched_product)
        {
            json_t *eye_relief_dial = json_object_get(vals, "EyeReliefDial");
            if (json_is_integer(eye_relief_dial))
            {
                unwarpvr->eye_relief_dial = json_integer_value(eye_relief_dial);
            }
            else if (eye_relief_dial != NULL)
                return ovr_parse_error(ctx, root, "EyeReliefDial is not integer");
            av_log(ctx, AV_LOG_VERBOSE, "Oculus profile settings: eye_relief_dial:%d\n",
                unwarpvr->eye_relief_dial);
            found_user_profile = 1;
        }
    }
    if (!found_user_profile)
        return ovr_parse_error(ctx, root, "could not find user profile for default user for selected device");
    json_decref(root);
    return 0;
}

static av_cold char* join_string_list(char* buffer, const size_t size, const char** list, const char* separator)
{
    if (*list == NULL) {
        strcpy(buffer, "");
        return buffer;
    }
    strncpy(buffer, *list, size);
    for (list++; *list; list++) {
        snprintf(buffer, size, "%s%s%s", buffer, separator, *list);
    }
    buffer[size - 1] = '\0'; // In case strncpy/snprintf didn't write a null terminator
    return buffer;
}

static av_cold int init_dict(AVFilterContext *ctx, AVDictionary **opts)
{
    UnwarpVRContext *unwarpvr = ctx->priv;
    int i, j, ret;

    char buffer[1024];
    const char *valid_devices[] = { "RiftDK1", "RiftDK2", NULL };
    const char *valid_sdk_versions[][128] = {
        { "0.2.5c", "0.4.2", NULL }, // RiftDK1
        { "0.4.2", NULL }, // RiftDK2
    };
    for (i = 0; valid_devices[i] != NULL; i++) {
        if (strcmp(unwarpvr->device, valid_devices[i]) == 0) {
            if (strcmp(unwarpvr->sdkversion, "default") == 0)
                strcpy(unwarpvr->sdkversion, valid_sdk_versions[i][0]);
            for (j = 0; valid_sdk_versions[j] != NULL; j++) {
                if (strcmp(unwarpvr->sdkversion, valid_sdk_versions[i][j]) == 0)
                    break;
            }
            if (valid_sdk_versions[i][j] == NULL) {
                av_log(ctx, AV_LOG_ERROR,
                    "Invalid SDK version specified. Valid options: %s\n",
                    join_string_list(buffer, sizeof(buffer), valid_sdk_versions[i], ", "));
                return AVERROR(EINVAL);
            }
            break;
        }
    }
    if (valid_devices[i] == NULL) {
        av_log(ctx, AV_LOG_ERROR,
            "unwarpvr: Invalid device specified. Valid options: %s\n",
            join_string_list(buffer, sizeof(buffer), valid_devices, ", "));
        return AVERROR(EINVAL);
    }

    if (valid_devices[i] == NULL) {
        av_log(ctx, AV_LOG_ERROR,
            "unwarpvr: Invalid device specified. Valid options: %s\n",
            join_string_list(buffer, sizeof(buffer), valid_devices, ", "));
        return AVERROR(EINVAL);
    }

    if (unwarpvr->eye_relief_dial == -1)
    {
        int ret = read_ovr_profile(ctx);
        if (ret)
            return ret;
    }

    if (unwarpvr->ppd != 0.0f && !unwarpvr->forward_warp) {
        av_log(ctx, AV_LOG_ERROR, "ppd parameter only valid when forward_warp=1\n");
        return AVERROR(EINVAL);
    }

    if (unwarpvr->size_str && (unwarpvr->w_expr || unwarpvr->h_expr)) {
        av_log(ctx, AV_LOG_ERROR,
            "Size and width/height expressions cannot be set at the same time.\n");
        return AVERROR(EINVAL);
    }

    if (unwarpvr->w_expr && !unwarpvr->h_expr)
        FFSWAP(char *, unwarpvr->w_expr, unwarpvr->size_str);

    if (unwarpvr->size_str) {
        char buf[32];
        if ((ret = av_parse_video_size(&unwarpvr->w, &unwarpvr->h, unwarpvr->size_str)) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                  "Invalid size '%s'\n", unwarpvr->size_str);
            return ret;
        }
        snprintf(buf, sizeof(buf)-1, "%d", unwarpvr->w);
        av_opt_set(unwarpvr, "w", buf, 0);
        snprintf(buf, sizeof(buf)-1, "%d", unwarpvr->h);
        av_opt_set(unwarpvr, "h", buf, 0);
    }
    if (!unwarpvr->w_expr)
        av_opt_set(unwarpvr, "w", "iw", 0);
    if (!unwarpvr->h_expr)
        av_opt_set(unwarpvr, "h", "ih", 0);

    av_log(ctx, AV_LOG_VERBOSE, "w:%s h:%s flags:'%s' interl:%d\n",
           unwarpvr->w_expr, unwarpvr->h_expr, (char *)av_x_if_null(unwarpvr->flags_str, ""), unwarpvr->interlaced);

    unwarpvr->flags = 0;

    if (unwarpvr->flags_str) {
        const AVClass *class = sws_get_class();
        const AVOption    *o = av_opt_find(&class, "sws_flags", NULL, 0,
                                           AV_OPT_SEARCH_FAKE_OBJ);
        int ret = av_opt_eval_flags(&class, o, unwarpvr->flags_str, &unwarpvr->flags);
        if (ret < 0)
            return ret;
    }
    unwarpvr->opts = *opts;
    *opts = NULL;

    return 0;
}

const int NumSegments = 11;

// From OVR_Stereo.cpp, evaluates Catmull-Rom spline based on provided K values
static float EvalCatmullRom10Spline(float const *K, float scaledVal)
{
    float t, omt, res;
    float p0, p1;
    float m0, m1;
    int k;

    float scaledValFloor = floorf(scaledVal);
    scaledValFloor = fmaxf(0.0f, fminf((float)(NumSegments - 1), scaledValFloor));
    t = scaledVal - scaledValFloor;
    k = (int)scaledValFloor;

    switch (k)
    {
    case 0:
        // Curve starts at 1.0 with gradient K[1]-K[0]
        p0 = 1.0f;
        m0 = (K[1] - K[0]);    // general case would have been (K[1]-K[-1])/2
        p1 = K[1];
        m1 = 0.5f * (K[2] - K[0]);
        break;
    default:
        // General case
        p0 = K[k];
        m0 = 0.5f * (K[k + 1] - K[k - 1]);
        p1 = K[k + 1];
        m1 = 0.5f * (K[k + 2] - K[k]);
        break;
    case NumSegments - 2:
        // Last tangent is just the slope of the last two points.
        p0 = K[NumSegments - 2];
        m0 = 0.5f * (K[NumSegments - 1] - K[NumSegments - 2]);
        p1 = K[NumSegments - 1];
        m1 = K[NumSegments - 1] - K[NumSegments - 2];
        break;
    case NumSegments - 1:
        // Beyond the last segment it's just a straight line
        p0 = K[NumSegments - 1];
        m0 = K[NumSegments - 1] - K[NumSegments - 2];
        p1 = p0 + m0;
        m1 = m0;
        break;
    }

    omt = 1.0f - t;
    res = (p0 * (1.0f + 2.0f *   t) + m0 *   t) * omt * omt
        + (p1 * (1.0f + 2.0f * omt) - m1 * omt) *   t *   t;

    return res;
}

// From OVR_DeviceConstants.h
enum DistortionEqnType
{
    Distortion_Poly4 = 0,
    Distortion_RecipPoly4 = 1,
    Distortion_CatmullRom10 = 2,
};

// Also based on function from OVR_Stereo.cpp
static float DistortionFnScaleRadiusSquared(enum DistortionEqnType Eqn, float const *K, float MaxR, float const CA0, float const CA1, float rsq)
{
    float scale = 1.0f;
    switch (Eqn)
    {
    case Distortion_Poly4:
        // This version is deprecated! Prefer one of the other two.
        scale = (K[0] + rsq * (K[1] + rsq * (K[2] + rsq * K[3])));
        break;
    case Distortion_RecipPoly4:
        scale = 1.0f / (K[0] + rsq * (K[1] + rsq * (K[2] + rsq * K[3])));
        break;
    case Distortion_CatmullRom10:{
                                     // A Catmull-Rom spline through the values 1.0, K[1], K[2] ... K[10]
                                     // evenly spaced in R^2 from 0.0 to MaxR^2
                                     // K[0] controls the slope at radius=0.0, rather than the actual value.
                                     float scaledRsq = (float)(NumSegments - 1) * rsq / (MaxR * MaxR);
                                     scale = EvalCatmullRom10Spline(K, scaledRsq);
    }break;
    }
    scale *= 1.0f + CA0 + CA1 * rsq;
    return scale;
}

// Computes inverse of DistortionFnScaleRadiusSquared function using binary search
// Function is monotonic increasing so this ought to work, although might be slow
static float DistortionFnScaleRadiusSquaredInv(enum DistortionEqnType Eqn, float const *K, float MaxR, float const CA0, float const CA1, float rsq)
{
    float low_guess = 0.0f, high_guess = 10.0f;
    // The "high_guess > 0.00001" is needed for the singular case where zero is the solution
    // With the relative error at 0.001 I observed a dot in the center on some frames, so lowered to 0.0001
    while ((high_guess - low_guess) / low_guess > 0.0001 && high_guess > 0.00001) {
        float mid_guess = (low_guess + high_guess) / 2.0f;
        float scale = DistortionFnScaleRadiusSquared(Eqn, K, MaxR, CA0, CA1, mid_guess);
        float mid_guess_value = scale * scale * mid_guess;
        if (rsq < mid_guess_value) {
            high_guess = mid_guess;
        }
        else {
            low_guess = mid_guess;
        }
    }
    return (low_guess + high_guess) / 2.0f;
}

#define NUM_EYES 2
#define NUM_CHANNELS 3

static av_cold void uninit(AVFilterContext *ctx)
{
    UnwarpVRContext *unwarpvr = ctx->priv;
    sws_freeContext(unwarpvr->sws);
    sws_freeContext(unwarpvr->isws[0]);
    sws_freeContext(unwarpvr->isws[1]);
    unwarpvr->sws = NULL;
    av_dict_free(&unwarpvr->opts);
    av_freep(&unwarpvr->inv_cache);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24,
        AV_PIX_FMT_RGBA,  AV_PIX_FMT_BGRA,
        AV_PIX_FMT_ABGR,  AV_PIX_FMT_ARGB,
        AV_PIX_FMT_0BGR,  AV_PIX_FMT_0RGB,
        AV_PIX_FMT_RGB0,  AV_PIX_FMT_BGR0,
        AV_PIX_FMT_NONE
    };

    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));
    return 0;
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    enum AVPixelFormat outfmt = outlink->format;
    UnwarpVRContext *unwarpvr = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    const AVPixFmtDescriptor *out_desc = av_pix_fmt_desc_get(outlink->format);
    int64_t w, h;
    double var_values[VARS_NB], res;
    char *expr;
    int ret;
    int factor_w, factor_h;

    var_values[VAR_IN_W]  = var_values[VAR_IW] = inlink->w;
    var_values[VAR_IN_H]  = var_values[VAR_IH] = inlink->h;
    var_values[VAR_OUT_W] = var_values[VAR_OW] = NAN;
    var_values[VAR_OUT_H] = var_values[VAR_OH] = NAN;
    var_values[VAR_A]     = (double) inlink->w / inlink->h;
    var_values[VAR_SAR]   = inlink->sample_aspect_ratio.num ?
        (double) inlink->sample_aspect_ratio.num / inlink->sample_aspect_ratio.den : 1;
    var_values[VAR_DAR]   = var_values[VAR_A] * var_values[VAR_SAR];
    var_values[VAR_HSUB]  = 1 << desc->log2_chroma_w;
    var_values[VAR_VSUB]  = 1 << desc->log2_chroma_h;
    var_values[VAR_OHSUB] = 1 << out_desc->log2_chroma_w;
    var_values[VAR_OVSUB] = 1 << out_desc->log2_chroma_h;

    /* evaluate width and height */
    av_expr_parse_and_eval(&res, (expr = unwarpvr->w_expr),
                           var_names, var_values,
                           NULL, NULL, NULL, NULL, NULL, 0, ctx);
    unwarpvr->w = var_values[VAR_OUT_W] = var_values[VAR_OW] = res;
    if ((ret = av_expr_parse_and_eval(&res, (expr = unwarpvr->h_expr),
                                      var_names, var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    unwarpvr->h = var_values[VAR_OUT_H] = var_values[VAR_OH] = res;
    /* evaluate again the width, as it may depend on the output height */
    if ((ret = av_expr_parse_and_eval(&res, (expr = unwarpvr->w_expr),
                                      var_names, var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    unwarpvr->w = res;

    w = unwarpvr->w;
    h = unwarpvr->h;

    /* Check if it is requested that the result has to be divisible by a some
     * factor (w or h = -n with n being the factor). */
    factor_w = 1;
    factor_h = 1;
    if (w < -1) {
        factor_w = -w;
    }
    if (h < -1) {
        factor_h = -h;
    }

    if (w < 0 && h < 0)
        unwarpvr->w = unwarpvr->h = 0;

    if (!(w = unwarpvr->w))
        w = inlink->w;
    if (!(h = unwarpvr->h))
        h = inlink->h;

    /* Make sure that the result is divisible by the factor we determined
     * earlier. If no factor was set, it is nothing will happen as the default
     * factor is 1 */
    if (w < 0)
        w = av_rescale(h, inlink->w, inlink->h * factor_w) * factor_w;
    if (h < 0)
        h = av_rescale(w, inlink->h, inlink->w * factor_h) * factor_h;

    /* Note that force_original_aspect_ratio may overwrite the previous set
     * dimensions so that it is not divisible by the set factors anymore. */
    if (unwarpvr->force_original_aspect_ratio) {
        int tmp_w = av_rescale(h, inlink->w, inlink->h);
        int tmp_h = av_rescale(w, inlink->h, inlink->w);

        if (unwarpvr->force_original_aspect_ratio == 1) {
             w = FFMIN(tmp_w, w);
             h = FFMIN(tmp_h, h);
        } else {
             w = FFMAX(tmp_w, w);
             h = FFMAX(tmp_h, h);
        }
    }

    if (w > INT_MAX || h > INT_MAX ||
        (h * inlink->w) > INT_MAX  ||
        (w * inlink->h) > INT_MAX)
        av_log(ctx, AV_LOG_ERROR, "Rescaled value for width or height is too big.\n");

    outlink->w = w;
    outlink->h = h;

    /* TODO: make algorithm configurable */

    unwarpvr->input_is_pal = desc->flags & AV_PIX_FMT_FLAG_PAL ||
                             desc->flags & AV_PIX_FMT_FLAG_PSEUDOPAL;
    if (outfmt == AV_PIX_FMT_PAL8) outfmt = AV_PIX_FMT_BGR8;
    unwarpvr->output_is_pal = av_pix_fmt_desc_get(outfmt)->flags & AV_PIX_FMT_FLAG_PAL ||
                              av_pix_fmt_desc_get(outfmt)->flags & AV_PIX_FMT_FLAG_PSEUDOPAL;

    if (unwarpvr->sws)
        sws_freeContext(unwarpvr->sws);
    if (unwarpvr->isws[0])
        sws_freeContext(unwarpvr->isws[0]);
    if (unwarpvr->isws[1])
        sws_freeContext(unwarpvr->isws[1]);
    unwarpvr->isws[0] = unwarpvr->isws[1] = unwarpvr->sws = NULL;
    if (inlink->w == outlink->w && inlink->h == outlink->h &&
        inlink->format == outlink->format)
        ;
    else {
        struct SwsContext **swscs[3] = { &unwarpvr->sws, &unwarpvr->isws[0], &unwarpvr->isws[1] };
        int i;

        for (i = 0; i < 3; i++) {
            struct SwsContext **s = swscs[i];
            *s = sws_alloc_context();
            if (!*s)
                return AVERROR(ENOMEM);

            if (unwarpvr->opts) {
                AVDictionaryEntry *e = NULL;

                while ((e = av_dict_get(unwarpvr->opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
                    if ((ret = av_opt_set(*s, e->key, e->value, 0)) < 0)
                        return ret;
                }
            }

            av_opt_set_int(*s, "srcw", inlink->w, 0);
            av_opt_set_int(*s, "srch", inlink->h >> !!i, 0);
            av_opt_set_int(*s, "src_format", inlink->format, 0);
            av_opt_set_int(*s, "dstw", outlink->w, 0);
            av_opt_set_int(*s, "dsth", outlink->h >> !!i, 0);
            av_opt_set_int(*s, "dst_format", outfmt, 0);
            av_opt_set_int(*s, "sws_flags", unwarpvr->flags, 0);

            av_opt_set_int(*s, "src_h_chr_pos", unwarpvr->in_h_chr_pos, 0);
            av_opt_set_int(*s, "src_v_chr_pos", unwarpvr->in_v_chr_pos, 0);
            av_opt_set_int(*s, "dst_h_chr_pos", unwarpvr->out_h_chr_pos, 0);
            av_opt_set_int(*s, "dst_v_chr_pos", unwarpvr->out_v_chr_pos, 0);

            if ((ret = sws_init_context(*s, NULL, NULL)) < 0)
                return ret;
            if (!unwarpvr->interlaced)
                break;
        }
    }

    if (inlink->sample_aspect_ratio.num){
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h * inlink->w, outlink->w * inlink->h}, inlink->sample_aspect_ratio);
    } else
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    av_log(ctx, AV_LOG_VERBOSE, "w:%d h:%d fmt:%s sar:%d/%d -> w:%d h:%d fmt:%s sar:%d/%d flags:0x%0x\n",
           inlink->w, inlink->h, av_get_pix_fmt_name( inlink->format),
           inlink->sample_aspect_ratio.num, inlink->sample_aspect_ratio.den,
           outlink->w, outlink->h, av_get_pix_fmt_name(outlink->format),
           outlink->sample_aspect_ratio.num, outlink->sample_aspect_ratio.den,
           unwarpvr->flags);

    // Initialize inv_cache
    {
        int i, j, eye_count;
        float MetersPerTanAngleAtCenter;
        float screenWidthMeters;
        float screenHeightMeters;
        float LensCenterXOffset; // For left eye, determined by physical parameters
        float TanEyeAngleScaleX, TanEyeAngleScaleY, DevicePPDInCenterX, DevicePPDInCenterY;
        enum DistortionEqnType Eqn = Distortion_CatmullRom10;
        float K[11];
        float MaxR = 1.0f;
        float ChromaticAberration[4];
        int DeviceResX, DeviceResY;
        int channel;
        int in_linesize;

        // Create temporary input frame just so we can get its linesize
        AVFrame* in = ff_get_video_buffer(inlink, inlink->w, inlink->h);
        if (!in) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        in_linesize = in->linesize[0];
        av_frame_free(&in);

        if (strcmp(unwarpvr->device, "RiftDK1") == 0) {
            MetersPerTanAngleAtCenter = 0.0425f;
            screenWidthMeters = 0.14976f;
            screenHeightMeters = screenWidthMeters / (1280.0f / 800.0f);
            LensCenterXOffset = 0.15197646600f;
            DeviceResX = 1280; DeviceResY = 800;

            if (strcmp(unwarpvr->sdkversion, "0.2.5c") == 0) {
                const float K_DK1[] = { 1.0f, 0.212f, 0.24f, 0.0f };
                const float ChromaticAberrationDK1[] = { 0.996f - 1.0f, -0.004f, 1.014f - 1.0f, 0.0f };
                Eqn = Distortion_Poly4;
                memmove(K, K_DK1, sizeof(K));
                memmove(ChromaticAberration, ChromaticAberrationDK1, sizeof(ChromaticAberration));
                MetersPerTanAngleAtCenter = 0.25f * screenWidthMeters; // Ensures TanEyeAngleScaleX = 1.0 to match 0.2.5c behavior
            }
            else if (strcmp(unwarpvr->sdkversion, "0.4.2") == 0) {
                // Use minimum eye relief distortion for now, but should be adjusted with eye relief
                const float K_DK1[] = { 1.0f, 1.06505f, 1.14725f, 1.2705f, 1.48f, 1.87f, 2.534f, 3.6f, 5.1f, 7.4f, 11.0f };
                const float ChromaticAberrationDK1[] = { -0.006f, 0.0f, 0.014f, 0.0f };
                memmove(K, K_DK1, sizeof(K));
                MaxR = sqrt(1.8f);
                // ChromaticAbberation does not vary by eye relief in DK1 in SDK 0.4.2
                memmove(ChromaticAberration, ChromaticAberrationDK1, sizeof(ChromaticAberration));
            }
            else {
                av_log(ctx, AV_LOG_ERROR, "Internal error: unhandled SDK version %s\n", unwarpvr->sdkversion);
                return AVERROR(EINVAL);
            }
        }
        else if (strcmp(unwarpvr->device, "RiftDK2") == 0) {
            // Distortion varies by SDK version but never by cup type or eye relief (for DK2 in 0.4.2)
            const float K_DK2[] = { 1.003f, 1.02f, 1.042f, 1.066f, 1.094f, 1.126f, 1.162f, 1.203f, 1.25f, 1.31f, 1.38f };
            // ChromaticAbberation varies by eye relief and lerps between the following two arrays
            const float ChromaticAberrationMin[] = { -0.0112f, -0.015f, 0.0187f, 0.015f };
            const float ChromaticAberrationMax[] = { -0.015f, -0.02f, 0.025f, 0.02f };
            memmove(K, K_DK2, sizeof(K));
            for (i = 0; i < sizeof(ChromaticAberration) / sizeof(*ChromaticAberration); i++) {
                ChromaticAberration[i] = ChromaticAberrationMin[i] + unwarpvr->eye_relief_dial / 10.0f * (ChromaticAberrationMax[i] - ChromaticAberrationMin[i]);
            }

            MetersPerTanAngleAtCenter = 0.036f;
            screenWidthMeters = 0.12576f;
            screenHeightMeters = 0.07074f;
            LensCenterXOffset = -0.00986003876f;
            DeviceResX = 1920; DeviceResY = 1080;
        }
        else {
            av_log(ctx, AV_LOG_ERROR,
                "Invalid device specified. Valid options: RiftDK1, RiftDK2\n");
            return AVERROR(EINVAL);
        }

        DevicePPDInCenterX = MetersPerTanAngleAtCenter / screenWidthMeters * DeviceResX;
        DevicePPDInCenterY = MetersPerTanAngleAtCenter / screenHeightMeters * DeviceResY;

        if (unwarpvr->ppd != 0.0f) {
            unwarpvr->scale_in_width *= (unwarpvr->ppd * 53.1301f) / DevicePPDInCenterX; // 53.1301 deg = tan(0.5) - (tan-0.5)
            unwarpvr->scale_in_height *= (unwarpvr->ppd * 53.1301f) / DevicePPDInCenterY;
        }

        // As computed in CalculateDistortionRenderDesc() distortion.TanEyeAngleScale in OVR_Stereo.cpp
        TanEyeAngleScaleX = 0.25f * screenWidthMeters / MetersPerTanAngleAtCenter;
        TanEyeAngleScaleY = 0.5f * screenHeightMeters / MetersPerTanAngleAtCenter;

        unwarpvr->inv_cache = av_malloc_array(outlink->w * outlink->h * NUM_CHANNELS, sizeof(int));
        if (!unwarpvr->inv_cache)
        {
            av_log(ctx, AV_LOG_ERROR, "unwarpvr: Out of memory allocating cache\n");
            return AVERROR(EINVAL);
        }
        for (i = 0; i < outlink->h * outlink->w * NUM_CHANNELS; i++) {
            unwarpvr->inv_cache[i] = -1;
        }
        for (eye_count = 0; eye_count < NUM_EYES; eye_count++) {
            int one_eye_multiplier = unwarpvr->left_eye_only ? 2 : 1;
            float lensCenterXOffsetEye;
            int in_eye = eye_count;
            int out_eye = eye_count;
            int in_width_per_eye = unwarpvr->mono_input ? inlink->w : inlink->w / 2;
            if (unwarpvr->left_eye_only && eye_count > 0)
                break;
            if (unwarpvr->swap_eyes)
                in_eye = 1 - in_eye;
            if (unwarpvr->mono_input)
                in_eye = 0;
            lensCenterXOffsetEye = ((!unwarpvr->forward_warp && in_eye) || (unwarpvr->forward_warp && out_eye)) ? -LensCenterXOffset : LensCenterXOffset;

            if (!unwarpvr->forward_warp)
            {
                for (i = 0; i < outlink->h; i++) {
                    for (j = 0; j < outlink->w / 2 * one_eye_multiplier; j++) {
                        float ndcx_raw, ndcy_raw, ndcx, ndcy, rsq;
                        float new_rsq[NUM_CHANNELS];

                        ndcx_raw = ((-1.0f + 2.0f * (j / (float)(outlink->w / 2 * one_eye_multiplier))) * one_eye_multiplier) / unwarpvr->scale_width;
                        ndcy_raw = (-1.0f + 2.0f * (i / (float)(outlink->h))) / unwarpvr->scale_height;
                        ndcx = ndcx_raw * ((float)outlink->w / DeviceResX); // Scale so changing input/output resolution only affects cropping, not scaling
                        ndcy = ndcy_raw * ((float)outlink->h / DeviceResY);
                        ndcx *= TanEyeAngleScaleX;
                        ndcy *= TanEyeAngleScaleY;
                        rsq = ndcx*ndcx + ndcy*ndcy;
                        new_rsq[0] = DistortionFnScaleRadiusSquaredInv(Eqn, K, MaxR, ChromaticAberration[0], ChromaticAberration[1], rsq);
                        new_rsq[1] = DistortionFnScaleRadiusSquaredInv(Eqn, K, MaxR, 0, 0, rsq);
                        new_rsq[2] = DistortionFnScaleRadiusSquaredInv(Eqn, K, MaxR, ChromaticAberration[2], ChromaticAberration[3], rsq);
                        for (channel = 0; channel < NUM_CHANNELS; channel++) {
                            float x, y;
                            int srcj, srci;
                            float ndcx_scaled, ndcy_scaled;
                            float scale = sqrt(new_rsq[channel] / rsq);
                            int output_idx = (i*outlink->w + eye_count*outlink->w / 2 + j)*NUM_CHANNELS + channel;

                            ndcx_scaled = ndcx * scale;
                            ndcy_scaled = ndcy * scale;
                            ndcx_scaled /= TanEyeAngleScaleX;
                            ndcy_scaled /= TanEyeAngleScaleY;
                            x = ((ndcx_scaled + lensCenterXOffsetEye) * unwarpvr->scale_in_width + 1.0f) / 2.0f * in_width_per_eye;
                            y = (ndcy_scaled * unwarpvr->scale_in_height + 1.0f) / 2.0f * inlink->h;

                            srcj = (int)x;
                            srci = (int)y;

                            if (srci >= 0 && srcj >= 0 && srci < inlink->h && srcj < in_width_per_eye)
                                unwarpvr->inv_cache[output_idx] = (srci*in_linesize) + (in_eye * in_width_per_eye + srcj)*NUM_CHANNELS + channel;
                        }
                    }
                }
            }
            else // if (unwarpvr->forward_warp)
            {
                for (i = 0; i < outlink->h; i++) {
                    for (j = 0; j < outlink->w / 2 * one_eye_multiplier; j++) {
                        float ndcx, ndcy, tanx_distorted, tany_distorted, rsq;
                        float scale[NUM_CHANNELS];

                        ndcx = ((-1.0f + 2.0f * j / (outlink->w / 2 * one_eye_multiplier)) * one_eye_multiplier) / unwarpvr->scale_width - lensCenterXOffsetEye;
                        ndcy = (-1.0f + 2.0f * i / outlink->h) / unwarpvr->scale_height;
                        tanx_distorted = ndcx * TanEyeAngleScaleX;
                        tany_distorted = ndcy * TanEyeAngleScaleY;
                        rsq = tanx_distorted*tanx_distorted + tany_distorted*tany_distorted;
                        scale[0] = DistortionFnScaleRadiusSquared(Eqn, K, MaxR, ChromaticAberration[0], ChromaticAberration[1], rsq);
                        scale[1] = DistortionFnScaleRadiusSquared(Eqn, K, MaxR, 0, 0, rsq);
                        scale[2] = DistortionFnScaleRadiusSquared(Eqn, K, MaxR, ChromaticAberration[2], ChromaticAberration[3], rsq);
                        for (channel = 0; channel < NUM_CHANNELS; channel++) {
                            float x, y;
                            int srcj, srci;
                            float tanx, tany, rt_ndcx, rt_ndcy;
                            int output_idx = (i*outlink->w + eye_count*outlink->w / 2 + j)*NUM_CHANNELS + channel;

                            tanx = tanx_distorted * scale[channel];
                            tany = tany_distorted * scale[channel];

                            rt_ndcx = tanx / TanEyeAngleScaleX;
                            rt_ndcy = tany / TanEyeAngleScaleY;

                            x = (rt_ndcx * unwarpvr->scale_in_width / 2.0f * DeviceResX / 2) + (in_width_per_eye / 2.0f);
                            y = (rt_ndcy * unwarpvr->scale_in_height / 2.0f * DeviceResY) + (inlink->h / 2.0f);

                            srcj = (int)x;
                            srci = (int)y;

                            if (srci >= 0 && srcj >= 0 && srci < inlink->h && srcj < in_width_per_eye)
                                unwarpvr->inv_cache[output_idx] = (srci*in_linesize) + (in_eye * in_width_per_eye + srcj)*NUM_CHANNELS + channel;
                        }
                    }
                }
            }
        }
    }

    return 0;

fail:
    av_log(NULL, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'.\n"
           "Maybe the expression for out_w:'%s' or for out_h:'%s' is self-referencing.\n",
           expr, unwarpvr->w_expr, unwarpvr->h_expr);
    return ret;
}

static int filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *ctx = link->dst;
    UnwarpVRContext *unwarpvr = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    const uint8_t *src = in->data[0];
    int i, j;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);
    out->width = outlink->w;
    out->height = outlink->h;

    {
        uint8_t *dst = out->data[0];
        int *inv_cache_p = unwarpvr->inv_cache;
        int jlimit = outlink->w * NUM_CHANNELS;
        int end_of_line_size = out->linesize[0] - jlimit;
        for (i = 0; i < outlink->h; i++) {
            for (j = 0; j < jlimit; j++, inv_cache_p++, dst++) {
                *dst = (*inv_cache_p == -1) ? 0 : src[*inv_cache_p];
            }
            dst += end_of_line_size;
        }
    }

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static const AVClass *child_class_next(const AVClass *prev)
{
    return prev ? NULL : sws_get_class();
}

#define OFFSET(x) offsetof(UnwarpVRContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption unwarpvr_options[] = {
    { "w",     "Output video width",          OFFSET(w_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "width", "Output video width",          OFFSET(w_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "h",     "Output video height",         OFFSET(h_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "height","Output video height",         OFFSET(h_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "flags", "Flags to pass to libswscale", OFFSET(flags_str), AV_OPT_TYPE_STRING, { .str = "bilinear" }, .flags = FLAGS },
    { "interl", "set interlacing", OFFSET(interlaced), AV_OPT_TYPE_INT, {.i64 = 0 }, -1, 1, FLAGS },
    { "size",   "set video size",          OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, FLAGS },
    { "s",      "set video size",          OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, FLAGS },
    {  "in_color_matrix", "set input YCbCr type",   OFFSET(in_color_matrix),  AV_OPT_TYPE_STRING, { .str = "auto" }, .flags = FLAGS },
    { "out_color_matrix", "set output YCbCr type",  OFFSET(out_color_matrix), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS },
    {  "in_range", "set input color range",  OFFSET( in_range), AV_OPT_TYPE_INT, {.i64 = AVCOL_RANGE_UNSPECIFIED }, 0, 2, FLAGS, "range" },
    { "out_range", "set output color range", OFFSET(out_range), AV_OPT_TYPE_INT, {.i64 = AVCOL_RANGE_UNSPECIFIED }, 0, 2, FLAGS, "range" },
    { "auto",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_UNSPECIFIED }, 0, 0, FLAGS, "range" },
    { "full",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_JPEG}, 0, 0, FLAGS, "range" },
    { "jpeg",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_JPEG}, 0, 0, FLAGS, "range" },
    { "mpeg",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_MPEG}, 0, 0, FLAGS, "range" },
    { "tv",     NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_MPEG}, 0, 0, FLAGS, "range" },
    { "pc",     NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_JPEG}, 0, 0, FLAGS, "range" },
    { "in_v_chr_pos",   "input vertical chroma position in luma grid/256"  ,   OFFSET(in_v_chr_pos),  AV_OPT_TYPE_INT, { .i64 = -513}, -513, 512, FLAGS },
    { "in_h_chr_pos",   "input horizontal chroma position in luma grid/256",   OFFSET(in_h_chr_pos),  AV_OPT_TYPE_INT, { .i64 = -513}, -513, 512, FLAGS },
    { "out_v_chr_pos",   "output vertical chroma position in luma grid/256"  , OFFSET(out_v_chr_pos), AV_OPT_TYPE_INT, { .i64 = -513}, -513, 512, FLAGS },
    { "out_h_chr_pos",   "output horizontal chroma position in luma grid/256", OFFSET(out_h_chr_pos), AV_OPT_TYPE_INT, { .i64 = -513}, -513, 512, FLAGS },
    { "force_original_aspect_ratio", "decrease or increase w/h if necessary to keep the original AR", OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 2, FLAGS, "force_oar" },
    { "disable",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0 }, 0, 0, FLAGS, "force_oar" },
    { "decrease", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1 }, 0, 0, FLAGS, "force_oar" },
    { "increase", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2 }, 0, 0, FLAGS, "force_oar" },
    { "swap_eyes", "swap the two eye views in the input before processing", OFFSET(swap_eyes), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS },
    { "left_eye_only", "render only the left eye view", OFFSET(left_eye_only), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS },
    { "scale_width", "scales width of output (1.0 for none)", OFFSET(scale_width), AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, -INFINITY, INFINITY, FLAGS },
    { "scale_height", "scales height of output (1.0 for none)", OFFSET(scale_height), AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, -INFINITY, INFINITY, FLAGS },
    { "scale_in_width", "sets scales of input (1.0 for none)", OFFSET(scale_in_width), AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, -INFINITY, INFINITY, FLAGS },
    { "scale_in_height", "sets scales of input (1.0 for none)", OFFSET(scale_in_height), AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, -INFINITY, INFINITY, FLAGS },
    { "eye_relief_dial", "setting of eye relief dial at time of recording (0-10, 10 is farthest out)", OFFSET(eye_relief_dial), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 10, FLAGS },
    { "forward_warp", "warps an undistorted image to suit a VR device, instead of unwarping", OFFSET(forward_warp), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS },
    { "ppd", "sets PPD (pixels per degree) of input in forward warp mode (by default same as output)", OFFSET(ppd), AV_OPT_TYPE_FLOAT, { .dbl = 0.0 }, -INFINITY, INFINITY, FLAGS },
    { "device", "indicates which HMD device was used to record the video", OFFSET(device), AV_OPT_TYPE_STRING, { .str = "RiftDK2" }, .flags = FLAGS },
    { "sdkversion", "indicates what version of the HMD device's SDK was used to record the video", OFFSET(sdkversion), AV_OPT_TYPE_STRING, { .str = "default" }, .flags = FLAGS },
    { "mono_input", "indicates that the input provides only one eye view which should be used for both eyes", OFFSET(mono_input), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS },
    { NULL }
};

static const AVClass unwarpvr_class = {
    .class_name       = "unwarpvr",
    .item_name        = av_default_item_name,
    .option           = unwarpvr_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
    .child_class_next = child_class_next,
};

static const AVFilterPad avfilter_vf_unwarpvr_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_unwarpvr_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
    },
    { NULL }
};

AVFilter ff_vf_unwarpvr = {
    .name          = "unwarpvr",
    .description   = NULL_IF_CONFIG_SMALL("Reverses the lens distortion correction and chromatic abberation correction performed by virtual reality head-mounted displays."),
    .init_dict     = init_dict,
    .uninit        = uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(UnwarpVRContext),
    .priv_class    = &unwarpvr_class,
    .inputs        = avfilter_vf_unwarpvr_inputs,
    .outputs       = avfilter_vf_unwarpvr_outputs,
};
