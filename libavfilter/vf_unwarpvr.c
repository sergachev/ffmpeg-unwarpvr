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
const char *profile_path = "ProfileDB.json";
hfdsjknfkjsdn // TODO! Profile path on Linux
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

    int eye_relief_dial;
} UnwarpVRContext;

static av_cold int ovr_parse_error(AVFilterContext *ctx, json_t *root, const char *reason)
{
    av_log(ctx, AV_LOG_ERROR,
        "Error encountered parsing Oculus SDK profile (%s).\n", reason);
    json_decref(root);
    return AVERROR(EINVAL);
}

static av_cold int init_dict(AVFilterContext *ctx, AVDictionary **opts)
{
    UnwarpVRContext *unwarpvr = ctx->priv;
    int i, j, ret;
    int found_user_profile = 0;
    json_t *root, *json, *tagged_data;
    json_error_t error;
    const char* default_user = NULL;
    const char* selected_product = "RiftDK2";

#ifdef _WIN32
    char profile_path[FILENAME_MAX + 1];
    ExpandEnvironmentStrings(unexpanded_profile_path, profile_path, sizeof(profile_path) / sizeof(*profile_path));
#else
    hfdsjknfkjsdn // TODO!
#endif

    // Default settings if not specified in JSON
    unwarpvr->eye_relief_dial = 3;

    root = json_load_file(profile_path, 0, &error);
    if (!root) {
        av_log(ctx, AV_LOG_ERROR,
            "Could not find Oculus SDK profile. Oculus Runtime may not be installed.\n");
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

static av_cold void uninit(AVFilterContext *ctx)
{
    UnwarpVRContext *unwarpvr = ctx->priv;
    sws_freeContext(unwarpvr->sws);
    sws_freeContext(unwarpvr->isws[0]);
    sws_freeContext(unwarpvr->isws[1]);
    unwarpvr->sws = NULL;
    av_dict_free(&unwarpvr->opts);
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

            av_opt_set_int(*s, "srcw", inlink ->w, 0);
            av_opt_set_int(*s, "srch", inlink ->h >> !!i, 0);
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
           inlink ->w, inlink ->h, av_get_pix_fmt_name( inlink->format),
           inlink->sample_aspect_ratio.num, inlink->sample_aspect_ratio.den,
           outlink->w, outlink->h, av_get_pix_fmt_name(outlink->format),
           outlink->sample_aspect_ratio.num, outlink->sample_aspect_ratio.den,
           unwarpvr->flags);
    return 0;

fail:
    av_log(NULL, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'.\n"
           "Maybe the expression for out_w:'%s' or for out_h:'%s' is self-referencing.\n",
           expr, unwarpvr->w_expr, unwarpvr->h_expr);
    return ret;
}

const int NumSegments = 11;

// From OVR_Stereo.cpp, evaluates Catmull-Rom spline based on provided K values
float EvalCatmullRom10Spline ( float const *K, float scaledVal );
float EvalCatmullRom10Spline ( float const *K, float scaledVal )
{
    float t, omt, res;
    float p0, p1;
    float m0, m1;
    int k;

    float scaledValFloor = floorf ( scaledVal );
    scaledValFloor = fmaxf ( 0.0f, fminf ( (float)(NumSegments-1), scaledValFloor ) );
    t = scaledVal - scaledValFloor;
    k = (int)scaledValFloor;

    switch ( k )
    {
    case 0:
        // Curve starts at 1.0 with gradient K[1]-K[0]
        p0 = 1.0f;
        m0 =        ( K[1] - K[0] );    // general case would have been (K[1]-K[-1])/2
        p1 = K[1];
        m1 = 0.5f * ( K[2] - K[0] );
        break;
    default:
        // General case
        p0 = K[k  ];
        m0 = 0.5f * ( K[k+1] - K[k-1] );
        p1 = K[k+1];
        m1 = 0.5f * ( K[k+2] - K[k  ] );
        break;
    case NumSegments-2:
        // Last tangent is just the slope of the last two points.
        p0 = K[NumSegments-2];
        m0 = 0.5f * ( K[NumSegments-1] - K[NumSegments-2] );
        p1 = K[NumSegments-1];
        m1 = K[NumSegments-1] - K[NumSegments-2];
        break;
    case NumSegments-1:
        // Beyond the last segment it's just a straight line
        p0 = K[NumSegments-1];
        m0 = K[NumSegments-1] - K[NumSegments-2];
        p1 = p0 + m0;
        m1 = m0;
        break;
    }

    omt = 1.0f - t;
    res  = ( p0 * ( 1.0f + 2.0f *   t ) + m0 *   t ) * omt * omt
         + ( p1 * ( 1.0f + 2.0f * omt ) - m1 * omt ) *   t *   t;

    return res;
}

// Computes inverse of CatmullRom spline function using binary search
// Function is monotonic increasing so this ought to work, although might be slow
float EvalCatmullRom10SplineInv ( float const *K, float const CA0, float const CA1, float scaledVal );
float EvalCatmullRom10SplineInv ( float const *K, float const CA0, float const CA1, float scaledVal ) {
    float low_guess = 0.0f, high_guess=1.5f;
    // The "high_guess > 0.00001" is needed for the singular case where zero is the solution
    // With the relative error at 0.001 I observed a dot in the center on some frames, so lowered to 0.0001
    while ((high_guess - low_guess)/low_guess > 0.0001 && high_guess > 0.00001) {
        float mid_guess = (low_guess + high_guess)/2.0f;
        float scale = EvalCatmullRom10Spline(K, (float)(NumSegments - 1) * mid_guess) * (1.0f + CA0 + CA1 * mid_guess);
        float mid_guess_value = scale * scale * mid_guess;
        if (scaledVal < mid_guess_value) {
            high_guess = mid_guess;
        } else {
            low_guess = mid_guess;
        }
    }
    return low_guess;
}

static int filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *ctx = link->dst;
    UnwarpVRContext *unwarpvr = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    const uint8_t *src = in->data[0];

    uint8_t *dstrow;
    int i, j, channel, eye;
    // Distortion varies by SDK version but never by cup type or eye relief (in 0.4.2)
    const float K[] = {1.003f, 1.02f, 1.042f, 1.066f, 1.094f, 1.126f, 1.162f, 1.203f, 1.25f, 1.31f, 1.38f};
    // ChromaticAbberation varies by eye relief and lerps between the following two arrays
    const float ChromaticAberrationMin[] = {-0.0112f, -0.015f, 0.0187f, 0.015f};
    const float ChromaticAberrationMax[] = {-0.015f, -0.02f, 0.025f, 0.02f};
    float ChromaticAberration[4];
    static float* inv_cache = NULL;
    float *inv_cache_p;

    for (i=0; i < sizeof(ChromaticAberration)/sizeof(*ChromaticAberration); i++) {
        ChromaticAberration[i] = ChromaticAberrationMin[i] + unwarpvr->eye_relief_dial / 10.0f * (ChromaticAberrationMax[i] - ChromaticAberrationMin[i]);
    }

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);
    out->width  = outlink->w;
    out->height = outlink->h;

    for (eye = 0; eye < 2; eye++) {
        const float lensCenterXOffset = eye ? 0.00986003876 : -0.00986003876; // For DK2, determined by physical parameters

#define NUM_CHANNELS 3
#define NUM_DIMENSIONS 2
        if (inv_cache == NULL) {
            // These are from distortion.TanEyeAngleScale.x and distortion.TanEyeAngleScale.y in OVR_Stereo.cpp
            // TODO: These might vary based on settings
            const float TanEyeAngleScaleX = 0.874f;
            const float TanEyeAngleScaleY = 0.9825f;
            int channel;

            inv_cache = (float *)malloc(sizeof(float) * outlink->w/2 * outlink->h * NUM_CHANNELS * NUM_DIMENSIONS);
            inv_cache_p = inv_cache;
            for (i = 0; i < outlink->h; i++) {
                for (j = 0; j < outlink->w/2; j++) {
                    float ndcx_raw = (-1.0f + 2.0f * ( j / (float)(outlink->w/2) ));
                    float ndcy_raw = (-1.0f + 2.0f * ( i / (float)(outlink->h) ));
                    // float ndcy_raw = (-1.0f + 2.0f * ( i / (float)(outlink->h) )) / 2.0f; // Fixes aspect ratio for YouTube SBS 3D
                    float ndcx = ndcx_raw * ((float)outlink->w/in->width) * TanEyeAngleScaleX;
                    float ndcy = ndcy_raw * ((float)outlink->h/in->height) * TanEyeAngleScaleY;
                    float rsq = ndcx*ndcx + ndcy*ndcy;
                    float new_rsq[NUM_CHANNELS];
                    new_rsq[0] = EvalCatmullRom10SplineInv(K, ChromaticAberration[0], ChromaticAberration[1], rsq);
                    new_rsq[1] = EvalCatmullRom10SplineInv(K, 0, 0, rsq);
                    new_rsq[2] = EvalCatmullRom10SplineInv(K, ChromaticAberration[2], ChromaticAberration[3], rsq);
                    for (channel=0; channel < NUM_CHANNELS; channel++) {
                        float scale = sqrt(new_rsq[channel]/rsq);
                        *inv_cache_p = ((float)outlink->w/in->width) * scale;
                        inv_cache_p++;
                        *inv_cache_p = ((float)outlink->h/in->height) * scale;
                        inv_cache_p++;
                    }
                }
            }
        }

        inv_cache_p = inv_cache;
        dstrow = out->data[0];
        for (i = 0; i < outlink->h; i++) {
            uint8_t *dst = dstrow + eye*outlink->w/2*NUM_CHANNELS;

            for (j = 0; j < outlink->w/2; j++) {

                // Convert render target pixel to NDC (screen resolution ranging from -1 to 1, larger values outside that, based on Oculus's OVR_Stereo.cpp)
                float ndcx_raw = (-1.0f + 2.0f * ( j / (float)(outlink->w/2) ));
                float ndcy_raw = (-1.0f + 2.0f * ( i / (float)(outlink->h) ));

                for (channel = 0; channel < 3; channel++, dst++) {
                    // Convert screen NDC ([-1,1]) to screen pixel
                    float x, y;
                    int srcj, srci;
                    float ndcx_scaled, ndcy_scaled;

                    ndcx_scaled = ndcx_raw * (*inv_cache_p);
                    inv_cache_p++;
                    ndcy_scaled = ndcy_raw * (*inv_cache_p);
                    inv_cache_p++;

                    x = (ndcx_scaled + lensCenterXOffset + 1.0f)/2.0f * in->width/2;
                    y = (ndcy_scaled + 1.0f)/2.0f * in->height;

                    srcj = (int)x;
                    srci = (int)y;
                    if (srci >= 0 && srcj >= 0 && srci < in->height && srcj < in->width/2) {
                        const uint8_t *srcpix = src + (srci*in->linesize[0]) + (eye*in->width/2 + srcj)*NUM_CHANNELS;
                        *dst = srcpix[channel];
                    } else {
                        *dst = 0;
                    }
                }
            }

            dstrow += out->linesize[0];
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
