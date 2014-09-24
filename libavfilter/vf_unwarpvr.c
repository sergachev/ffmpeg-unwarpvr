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
} UnwarpVRContext;

static av_cold int init_dict(AVFilterContext *ctx, AVDictionary **opts)
{
    UnwarpVRContext *scale = ctx->priv;
    int ret;

    if (scale->size_str && (scale->w_expr || scale->h_expr)) {
        av_log(ctx, AV_LOG_ERROR,
               "Size and width/height expressions cannot be set at the same time.\n");
            return AVERROR(EINVAL);
    }

    if (scale->w_expr && !scale->h_expr)
        FFSWAP(char *, scale->w_expr, scale->size_str);

    if (scale->size_str) {
        char buf[32];
        if ((ret = av_parse_video_size(&scale->w, &scale->h, scale->size_str)) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Invalid size '%s'\n", scale->size_str);
            return ret;
        }
        snprintf(buf, sizeof(buf)-1, "%d", scale->w);
        av_opt_set(scale, "w", buf, 0);
        snprintf(buf, sizeof(buf)-1, "%d", scale->h);
        av_opt_set(scale, "h", buf, 0);
    }
    if (!scale->w_expr)
        av_opt_set(scale, "w", "iw", 0);
    if (!scale->h_expr)
        av_opt_set(scale, "h", "ih", 0);

    av_log(ctx, AV_LOG_VERBOSE, "w:%s h:%s flags:'%s' interl:%d\n",
           scale->w_expr, scale->h_expr, (char *)av_x_if_null(scale->flags_str, ""), scale->interlaced);

    scale->flags = 0;

    if (scale->flags_str) {
        const AVClass *class = sws_get_class();
        const AVOption    *o = av_opt_find(&class, "sws_flags", NULL, 0,
                                           AV_OPT_SEARCH_FAKE_OBJ);
        int ret = av_opt_eval_flags(&class, o, scale->flags_str, &scale->flags);
        if (ret < 0)
            return ret;
    }
    scale->opts = *opts;
    *opts = NULL;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    UnwarpVRContext *scale = ctx->priv;
    sws_freeContext(scale->sws);
    sws_freeContext(scale->isws[0]);
    sws_freeContext(scale->isws[1]);
    scale->sws = NULL;
    av_dict_free(&scale->opts);
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

static const int *parse_yuv_type(const char *s, enum AVColorSpace colorspace)
{
    if (!s)
        s = "bt601";

    if (s && strstr(s, "bt709")) {
        colorspace = AVCOL_SPC_BT709;
    } else if (s && strstr(s, "fcc")) {
        colorspace = AVCOL_SPC_FCC;
    } else if (s && strstr(s, "smpte240m")) {
        colorspace = AVCOL_SPC_SMPTE240M;
    } else if (s && (strstr(s, "bt601") || strstr(s, "bt470") || strstr(s, "smpte170m"))) {
        colorspace = AVCOL_SPC_BT470BG;
    }

    if (colorspace < 1 || colorspace > 7) {
        colorspace = AVCOL_SPC_BT470BG;
    }

    return sws_getCoefficients(colorspace);
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    enum AVPixelFormat outfmt = outlink->format;
    UnwarpVRContext *scale = ctx->priv;
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
    av_expr_parse_and_eval(&res, (expr = scale->w_expr),
                           var_names, var_values,
                           NULL, NULL, NULL, NULL, NULL, 0, ctx);
    scale->w = var_values[VAR_OUT_W] = var_values[VAR_OW] = res;
    if ((ret = av_expr_parse_and_eval(&res, (expr = scale->h_expr),
                                      var_names, var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    scale->h = var_values[VAR_OUT_H] = var_values[VAR_OH] = res;
    /* evaluate again the width, as it may depend on the output height */
    if ((ret = av_expr_parse_and_eval(&res, (expr = scale->w_expr),
                                      var_names, var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    scale->w = res;

    w = scale->w;
    h = scale->h;

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
        scale->w = scale->h = 0;

    if (!(w = scale->w))
        w = inlink->w;
    if (!(h = scale->h))
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
    if (scale->force_original_aspect_ratio) {
        int tmp_w = av_rescale(h, inlink->w, inlink->h);
        int tmp_h = av_rescale(w, inlink->h, inlink->w);

        if (scale->force_original_aspect_ratio == 1) {
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

    scale->input_is_pal = desc->flags & AV_PIX_FMT_FLAG_PAL ||
                          desc->flags & AV_PIX_FMT_FLAG_PSEUDOPAL;
    if (outfmt == AV_PIX_FMT_PAL8) outfmt = AV_PIX_FMT_BGR8;
    scale->output_is_pal = av_pix_fmt_desc_get(outfmt)->flags & AV_PIX_FMT_FLAG_PAL ||
                           av_pix_fmt_desc_get(outfmt)->flags & AV_PIX_FMT_FLAG_PSEUDOPAL;

    if (scale->sws)
        sws_freeContext(scale->sws);
    if (scale->isws[0])
        sws_freeContext(scale->isws[0]);
    if (scale->isws[1])
        sws_freeContext(scale->isws[1]);
    scale->isws[0] = scale->isws[1] = scale->sws = NULL;
    if (inlink->w == outlink->w && inlink->h == outlink->h &&
        inlink->format == outlink->format)
        ;
    else {
        struct SwsContext **swscs[3] = {&scale->sws, &scale->isws[0], &scale->isws[1]};
        int i;

        for (i = 0; i < 3; i++) {
            struct SwsContext **s = swscs[i];
            *s = sws_alloc_context();
            if (!*s)
                return AVERROR(ENOMEM);

            if (scale->opts) {
                AVDictionaryEntry *e = NULL;

                while ((e = av_dict_get(scale->opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
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
            av_opt_set_int(*s, "sws_flags", scale->flags, 0);

            av_opt_set_int(*s, "src_h_chr_pos", scale->in_h_chr_pos, 0);
            av_opt_set_int(*s, "src_v_chr_pos", scale->in_v_chr_pos, 0);
            av_opt_set_int(*s, "dst_h_chr_pos", scale->out_h_chr_pos, 0);
            av_opt_set_int(*s, "dst_v_chr_pos", scale->out_v_chr_pos, 0);

            if ((ret = sws_init_context(*s, NULL, NULL)) < 0)
                return ret;
            if (!scale->interlaced)
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
           scale->flags);
    return 0;

fail:
    av_log(NULL, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'.\n"
           "Maybe the expression for out_w:'%s' or for out_h:'%s' is self-referencing.\n",
           expr, scale->w_expr, scale->h_expr);
    return ret;
}

static int scale_slice(AVFilterLink *link, AVFrame *out_buf, AVFrame *cur_pic, struct SwsContext *sws, int y, int h, int mul, int field)
{
    UnwarpVRContext *scale = link->dst->priv;
    const uint8_t *in[4];
    uint8_t *out[4];
    int in_stride[4],out_stride[4];
    int i;

    for(i=0; i<4; i++){
        int vsub= ((i+1)&2) ? scale->vsub : 0;
         in_stride[i] = cur_pic->linesize[i] * mul;
        out_stride[i] = out_buf->linesize[i] * mul;
         in[i] = cur_pic->data[i] + ((y>>vsub)+field) * cur_pic->linesize[i];
        out[i] = out_buf->data[i] +            field  * out_buf->linesize[i];
    }
    if(scale->input_is_pal)
         in[1] = cur_pic->data[1];
    if(scale->output_is_pal)
        out[1] = out_buf->data[1];

    return sws_scale(sws, in, in_stride, y/mul, h,
                         out,out_stride);
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
    UnwarpVRContext *scale = link->dst->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    AVFrame *out;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(link->format);
    char buf[32];
    int in_range;

    const uint8_t *src = in->data[0];

    uint8_t *dstrow;
    int i, j, channel, eye;
    const float K[] = {1.003f, 1.02f, 1.042f, 1.066f, 1.094f, 1.126f, 1.162f, 1.203f, 1.25f, 1.31f, 1.38f};
    // const float ChromaticAberration[] = {-0.0112f, -0.015f, 0.0187f, 0.015f};
    const float ChromaticAberration[] = {-0.0131f, -0.0175f, 0.02185f, 0.0175};
    static float* inv_cache = NULL;
    float *inv_cache_p;

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

    if(   in->width  != link->w
       || in->height != link->h
       || in->format != link->format) {
        int ret;
        snprintf(buf, sizeof(buf)-1, "%d", outlink->w);
        av_opt_set(scale, "w", buf, 0);
        snprintf(buf, sizeof(buf)-1, "%d", outlink->h);
        av_opt_set(scale, "h", buf, 0);

        link->dst->inputs[0]->format = in->format;
        link->dst->inputs[0]->w      = in->width;
        link->dst->inputs[0]->h      = in->height;

        if ((ret = config_props(outlink)) < 0)
            return ret;
    }

    if (!scale->sws)
        return ff_filter_frame(outlink, in);

    scale->hsub = desc->log2_chroma_w;
    scale->vsub = desc->log2_chroma_h;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    av_frame_copy_props(out, in);
    out->width  = outlink->w;
    out->height = outlink->h;

    if(scale->output_is_pal)
        avpriv_set_systematic_pal2((uint32_t*)out->data[1], outlink->format == AV_PIX_FMT_PAL8 ? AV_PIX_FMT_BGR8 : outlink->format);

    in_range = av_frame_get_color_range(in);

    if (   scale->in_color_matrix
        || scale->out_color_matrix
        || scale-> in_range != AVCOL_RANGE_UNSPECIFIED
        || in_range != AVCOL_RANGE_UNSPECIFIED
        || scale->out_range != AVCOL_RANGE_UNSPECIFIED) {
        int in_full, out_full, brightness, contrast, saturation;
        const int *inv_table, *table;

        sws_getColorspaceDetails(scale->sws, (int **)&inv_table, &in_full,
                                 (int **)&table, &out_full,
                                 &brightness, &contrast, &saturation);

        if (scale->in_color_matrix)
            inv_table = parse_yuv_type(scale->in_color_matrix, av_frame_get_colorspace(in));
        if (scale->out_color_matrix)
            table     = parse_yuv_type(scale->out_color_matrix, AVCOL_SPC_UNSPECIFIED);

        if (scale-> in_range != AVCOL_RANGE_UNSPECIFIED)
            in_full  = (scale-> in_range == AVCOL_RANGE_JPEG);
        else if (in_range != AVCOL_RANGE_UNSPECIFIED)
            in_full  = (in_range == AVCOL_RANGE_JPEG);
        if (scale->out_range != AVCOL_RANGE_UNSPECIFIED)
            out_full = (scale->out_range == AVCOL_RANGE_JPEG);

        sws_setColorspaceDetails(scale->sws, inv_table, in_full,
                                 table, out_full,
                                 brightness, contrast, saturation);
        if (scale->isws[0])
            sws_setColorspaceDetails(scale->isws[0], inv_table, in_full,
                                     table, out_full,
                                     brightness, contrast, saturation);
        if (scale->isws[1])
            sws_setColorspaceDetails(scale->isws[1], inv_table, in_full,
                                     table, out_full,
                                     brightness, contrast, saturation);
    }

    av_reduce(&out->sample_aspect_ratio.num, &out->sample_aspect_ratio.den,
              (int64_t)in->sample_aspect_ratio.num * outlink->h * link->w,
              (int64_t)in->sample_aspect_ratio.den * outlink->w * link->h,
              INT_MAX);

    if(scale->interlaced>0 || (scale->interlaced<0 && in->interlaced_frame)){
        scale_slice(link, out, in, scale->isws[0], 0, (link->h+1)/2, 2, 0);
        scale_slice(link, out, in, scale->isws[1], 0,  link->h   /2, 2, 1);
    }else{
        scale_slice(link, out, in, scale->sws, 0, link->h, 1, 0);
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
