/*
 * generic decoding-related code
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>
#include <string.h>

#include "config.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/imgutils.h"
#include "libavutil/intmath.h"

#include "avcodec.h"
#include "bytestream.h"
#include "decode.h"
#include "internal.h"
#include "thread.h"

static int apply_param_change(AVCodecContext *avctx, AVPacket *avpkt)
{
    int size = 0, ret;
    const uint8_t *data;
    uint32_t flags;

    data = av_packet_get_side_data(avpkt, AV_PKT_DATA_PARAM_CHANGE, &size);
    if (!data)
        return 0;

    if (!(avctx->codec->capabilities & AV_CODEC_CAP_PARAM_CHANGE)) {
        av_log(avctx, AV_LOG_ERROR, "This decoder does not support parameter "
               "changes, but PARAM_CHANGE side data was sent to it.\n");
        ret = AVERROR(EINVAL);
        goto fail2;
    }

    if (size < 4)
        goto fail;

    flags = bytestream_get_le32(&data);
    size -= 4;

#if FF_API_OLD_CHANNEL_LAYOUT
FF_DISABLE_DEPRECATION_WARNINGS
    if (flags & AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_COUNT) {
        if (size < 4)
            goto fail;
        avctx->channels = bytestream_get_le32(&data);
        size -= 4;
    }
    if (flags & AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_LAYOUT) {
        if (size < 8)
            goto fail;
        avctx->channel_layout = bytestream_get_le64(&data);
        size -= 8;
    }
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    if (flags & AV_SIDE_DATA_PARAM_CHANGE_SAMPLE_RATE) {
        if (size < 4)
            goto fail;
        avctx->sample_rate = bytestream_get_le32(&data);
        size -= 4;
    }
    if (flags & AV_SIDE_DATA_PARAM_CHANGE_DIMENSIONS) {
        if (size < 8)
            goto fail;
        avctx->width  = bytestream_get_le32(&data);
        avctx->height = bytestream_get_le32(&data);
        size -= 8;
        ret = ff_set_dimensions(avctx, avctx->width, avctx->height);
        if (ret < 0)
            goto fail2;
    }

    return 0;
fail:
    av_log(avctx, AV_LOG_ERROR, "PARAM_CHANGE side data too small.\n");
    ret = AVERROR_INVALIDDATA;
fail2:
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error applying parameter changes.\n");
        if (avctx->err_recognition & AV_EF_EXPLODE)
            return ret;
    }
    return 0;
}

static int extract_packet_props(AVCodecInternal *avci, const AVPacket *pkt)
{
    av_packet_unref(avci->last_pkt_props);
    if (pkt)
        return av_packet_copy_props(avci->last_pkt_props, pkt);
    return 0;
}

static int unrefcount_frame(AVCodecInternal *avci, AVFrame *frame)
{
    int ret;

    /* move the original frame to our backup */
    av_frame_unref(avci->to_free);
    av_frame_move_ref(avci->to_free, frame);

    /* now copy everything except the AVBufferRefs back
     * note that we make a COPY of the side data, so calling av_frame_free() on
     * the caller's frame will work properly */
    ret = av_frame_copy_props(frame, avci->to_free);
    if (ret < 0)
        return ret;

    memcpy(frame->data,     avci->to_free->data,     sizeof(frame->data));
    memcpy(frame->linesize, avci->to_free->linesize, sizeof(frame->linesize));
    if (avci->to_free->extended_data != avci->to_free->data) {
        int planes = av_get_channel_layout_nb_channels(avci->to_free->channel_layout);
        int size   = planes * sizeof(*frame->extended_data);

        if (!size) {
            av_frame_unref(frame);
            return AVERROR_BUG;
        }

        frame->extended_data = av_malloc(size);
        if (!frame->extended_data) {
            av_frame_unref(frame);
            return AVERROR(ENOMEM);
        }
        memcpy(frame->extended_data, avci->to_free->extended_data,
               size);
    } else
        frame->extended_data = frame->data;

    frame->format         = avci->to_free->format;
    frame->width          = avci->to_free->width;
    frame->height         = avci->to_free->height;
    frame->channel_layout = avci->to_free->channel_layout;
    frame->nb_samples     = avci->to_free->nb_samples;

    return 0;
}

static int bsfs_init(AVCodecContext *avctx)
{
    AVCodecInternal *avci = avctx->internal;
    DecodeFilterContext *s = &avci->filter;
    const char *bsfs_str;
    int ret;

    if (s->nb_bsfs)
        return 0;

    bsfs_str = avctx->codec->bsfs ? avctx->codec->bsfs : "null";
    while (bsfs_str && *bsfs_str) {
        AVBSFContext **tmp;
        const AVBitStreamFilter *filter;
        char *bsf;

        bsf = av_get_token(&bsfs_str, ",");
        if (!bsf) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        filter = av_bsf_get_by_name(bsf);
        if (!filter) {
            av_log(avctx, AV_LOG_ERROR, "A non-existing bitstream filter %s "
                   "requested by a decoder. This is a bug, please report it.\n",
                   bsf);
            ret = AVERROR_BUG;
            av_freep(&bsf);
            goto fail;
        }
        av_freep(&bsf);

        tmp = av_realloc_array(s->bsfs, s->nb_bsfs + 1, sizeof(*s->bsfs));
        if (!tmp) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        s->bsfs = tmp;
        s->nb_bsfs++;

        ret = av_bsf_alloc(filter, &s->bsfs[s->nb_bsfs - 1]);
        if (ret < 0)
            goto fail;

        if (s->nb_bsfs == 1) {
            /* We do not currently have an API for passing the input timebase into decoders,
             * but no filters used here should actually need it.
             * So we make up some plausible-looking number (the MPEG 90kHz timebase) */
            s->bsfs[s->nb_bsfs - 1]->time_base_in = (AVRational){ 1, 90000 };
            ret = avcodec_parameters_from_context(s->bsfs[s->nb_bsfs - 1]->par_in,
                                                  avctx);
        } else {
            s->bsfs[s->nb_bsfs - 1]->time_base_in = s->bsfs[s->nb_bsfs - 2]->time_base_out;
            ret = avcodec_parameters_copy(s->bsfs[s->nb_bsfs - 1]->par_in,
                                          s->bsfs[s->nb_bsfs - 2]->par_out);
        }
        if (ret < 0)
            goto fail;

        ret = av_bsf_init(s->bsfs[s->nb_bsfs - 1]);
        if (ret < 0)
            goto fail;
    }

    return 0;
fail:
    ff_decode_bsfs_uninit(avctx);
    return ret;
}

/* try to get one output packet from the filter chain */
static int bsfs_poll(AVCodecContext *avctx, AVPacket *pkt)
{
    DecodeFilterContext *s = &avctx->internal->filter;
    int idx, ret;

    /* start with the last filter in the chain */
    idx = s->nb_bsfs - 1;
    while (idx >= 0) {
        /* request a packet from the currently selected filter */
        ret = av_bsf_receive_packet(s->bsfs[idx], pkt);
        if (ret == AVERROR(EAGAIN)) {
            /* no packets available, try the next filter up the chain */
            ret = 0;
            idx--;
            continue;
        } else if (ret < 0 && ret != AVERROR_EOF) {
            return ret;
        }

        /* got a packet or EOF -- pass it to the caller or to the next filter
         * down the chain */
        if (idx == s->nb_bsfs - 1) {
            return ret;
        } else {
            idx++;
            ret = av_bsf_send_packet(s->bsfs[idx], ret < 0 ? NULL : pkt);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR,
                       "Error pre-processing a packet before decoding\n");
                av_packet_unref(pkt);
                return ret;
            }
        }
    }

    return AVERROR(EAGAIN);
}

int ff_decode_get_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    AVCodecInternal *avci = avctx->internal;
    int ret;

    if (avci->draining)
        return AVERROR_EOF;

    ret = bsfs_poll(avctx, pkt);
    if (ret == AVERROR_EOF)
        avci->draining = 1;
    if (ret < 0)
        return ret;

    ret = extract_packet_props(avctx->internal, pkt);
    if (ret < 0)
        goto finish;

    ret = apply_param_change(avctx, pkt);
    if (ret < 0)
        goto finish;

    if (avctx->codec->receive_frame)
        avci->compat_decode_consumed += pkt->size;

    return 0;
finish:
    av_packet_unref(pkt);
    return ret;
}

/*
 * The core of the receive_frame_wrapper for the decoders implementing
 * the simple API. Certain decoders might consume partial packets without
 * returning any output, so this function needs to be called in a loop until it
 * returns EAGAIN.
 **/
static int decode_simple_internal(AVCodecContext *avctx, AVFrame *frame)
{
    AVCodecInternal   *avci = avctx->internal;
    DecodeSimpleContext *ds = &avci->ds;
    AVPacket           *pkt = ds->in_pkt;
    int got_frame;
    int ret;

    if (!pkt->data && !avci->draining) {
        av_packet_unref(pkt);
        ret = ff_decode_get_packet(avctx, pkt);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
    }

    // Some codecs (at least wma lossless) will crash when feeding drain packets
    // after EOF was signaled.
    if (avci->draining_done)
        return AVERROR_EOF;

    if (!pkt->data &&
        !(avctx->codec->capabilities & AV_CODEC_CAP_DELAY ||
          avctx->active_thread_type & FF_THREAD_FRAME))
        return AVERROR_EOF;

    got_frame = 0;

    if (HAVE_THREADS && avctx->active_thread_type & FF_THREAD_FRAME) {
        ret = ff_thread_decode_frame(avctx, frame, &got_frame, pkt);
    } else {
        ret = avctx->codec->decode(avctx, frame, &got_frame, pkt);

        if (!(avctx->codec->caps_internal & FF_CODEC_CAP_SETS_PKT_DTS))
            frame->pkt_dts = pkt->dts;
        /* get_buffer is supposed to set frame parameters */
        if (!(avctx->codec->capabilities & AV_CODEC_CAP_DR1)) {
            frame->sample_aspect_ratio = avctx->sample_aspect_ratio;
            frame->width               = avctx->width;
            frame->height              = avctx->height;
            frame->format              = avctx->codec->type == AVMEDIA_TYPE_VIDEO ?
                                         avctx->pix_fmt : avctx->sample_fmt;
        }
    }

    emms_c();

    if (!got_frame)
        av_frame_unref(frame);

    if (ret >= 0 && avctx->codec->type == AVMEDIA_TYPE_VIDEO)
        ret = pkt->size;

    if (avctx->internal->draining && !got_frame)
        avci->draining_done = 1;

    avci->compat_decode_consumed += ret;

    if (ret >= pkt->size || ret < 0) {
        av_packet_unref(pkt);
    } else {
        int consumed = ret;

        pkt->data                += consumed;
        pkt->size                -= consumed;
        pkt->pts                  = AV_NOPTS_VALUE;
        pkt->dts                  = AV_NOPTS_VALUE;
        avci->last_pkt_props->pts = AV_NOPTS_VALUE;
        avci->last_pkt_props->dts = AV_NOPTS_VALUE;
    }

    if (got_frame)
        av_assert0(frame->buf[0]);

    return ret < 0 ? ret : 0;
}

static int decode_simple_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    int ret;

    while (!frame->buf[0]) {
        ret = decode_simple_internal(avctx, frame);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int decode_receive_frame_internal(AVCodecContext *avctx, AVFrame *frame)
{
    AVCodecInternal *avci = avctx->internal;
    int ret;

    av_assert0(!frame->buf[0]);

    if (avctx->codec->receive_frame)
        ret = avctx->codec->receive_frame(avctx, frame);
    else
        ret = decode_simple_receive_frame(avctx, frame);

    if (ret == AVERROR_EOF)
        avci->draining_done = 1;

    return ret;
}

int attribute_align_arg avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    AVCodecInternal *avci = avctx->internal;
    int ret = 0;

    if (!avcodec_is_open(avctx) || !av_codec_is_decoder(avctx->codec))
        return AVERROR(EINVAL);

    if (avctx->internal->draining)
        return AVERROR_EOF;

    ret = bsfs_init(avctx);
    if (ret < 0)
        return ret;

    av_packet_unref(avci->buffer_pkt);
    if (avpkt && (avpkt->data || avpkt->side_data_elems)) {
        ret = av_packet_ref(avci->buffer_pkt, avpkt);
        if (ret < 0)
            return ret;
    }

    ret = av_bsf_send_packet(avci->filter.bsfs[0], avci->buffer_pkt);
    if (ret < 0) {
        av_packet_unref(avci->buffer_pkt);
        return ret;
    }

    if (!avci->buffer_frame->buf[0]) {
        ret = decode_receive_frame_internal(avctx, avci->buffer_frame);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            return ret;
    }

    return 0;
}

static int calc_cropping_offsets(size_t offsets[4], const AVFrame *frame,
                                 const AVPixFmtDescriptor *desc)
{
    int i, j;

    for (i = 0; frame->data[i]; i++) {
        const AVComponentDescriptor *comp = NULL;
        int shift_x = (i == 1 || i == 2) ? desc->log2_chroma_w : 0;
        int shift_y = (i == 1 || i == 2) ? desc->log2_chroma_h : 0;

        if (desc->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_PSEUDOPAL) && i == 1) {
            offsets[i] = 0;
            break;
        }

        /* find any component descriptor for this plane */
        for (j = 0; j < desc->nb_components; j++) {
            if (desc->comp[j].plane == i) {
                comp = &desc->comp[j];
                break;
            }
        }
        if (!comp)
            return AVERROR_BUG;

        offsets[i] = (frame->crop_top  >> shift_y) * frame->linesize[i] +
                     (frame->crop_left >> shift_x) * comp->step;
    }

    return 0;
}

static int apply_cropping(AVCodecContext *avctx, AVFrame *frame)
{
    const AVPixFmtDescriptor *desc;
    size_t offsets[4];
    int i;

    /* make sure we are noisy about decoders returning invalid cropping data */
    if (frame->crop_left >= INT_MAX - frame->crop_right        ||
        frame->crop_top  >= INT_MAX - frame->crop_bottom       ||
        (frame->crop_left + frame->crop_right) >= frame->width ||
        (frame->crop_top + frame->crop_bottom) >= frame->height) {
        av_log(avctx, AV_LOG_WARNING,
               "Invalid cropping information set by a decoder: %zu/%zu/%zu/%zu "
               "(frame size %dx%d). This is a bug, please report it\n",
               frame->crop_left, frame->crop_right, frame->crop_top, frame->crop_bottom,
               frame->width, frame->height);
        frame->crop_left   = 0;
        frame->crop_right  = 0;
        frame->crop_top    = 0;
        frame->crop_bottom = 0;
        return 0;
    }

    if (!avctx->apply_cropping)
        return 0;

    desc = av_pix_fmt_desc_get(frame->format);
    if (!desc)
        return AVERROR_BUG;

    /* Apply just the right/bottom cropping for hwaccel formats. Bitstream
     * formats cannot be easily handled here either (and corresponding decoders
     * should not export any cropping anyway), so do the same for those as well.
     * */
    if (desc->flags & (AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL)) {
        frame->width      -= frame->crop_right;
        frame->height     -= frame->crop_bottom;
        frame->crop_right  = 0;
        frame->crop_bottom = 0;
        return 0;
    }

    /* calculate the offsets for each plane */
    calc_cropping_offsets(offsets, frame, desc);

    /* adjust the offsets to avoid breaking alignment */
    if (!(avctx->flags & AV_CODEC_FLAG_UNALIGNED)) {
        int min_log2_align = INT_MAX;

        for (i = 0; frame->data[i]; i++) {
            int log2_align = offsets[i] ? av_ctz(offsets[i]) : INT_MAX;
            min_log2_align = FFMIN(log2_align, min_log2_align);
        }

        if (min_log2_align < 5) {
            frame->crop_left &= ~((1 << min_log2_align) - 1);
            calc_cropping_offsets(offsets, frame, desc);
        }
    }

    for (i = 0; frame->data[i]; i++)
        frame->data[i] += offsets[i];

    frame->width      -= (frame->crop_left + frame->crop_right);
    frame->height     -= (frame->crop_top  + frame->crop_bottom);
    frame->crop_left   = 0;
    frame->crop_right  = 0;
    frame->crop_top    = 0;
    frame->crop_bottom = 0;

    return 0;
}

int attribute_align_arg avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    AVCodecInternal *avci = avctx->internal;
    int ret;

    av_frame_unref(frame);

    if (!avcodec_is_open(avctx) || !av_codec_is_decoder(avctx->codec))
        return AVERROR(EINVAL);

    ret = bsfs_init(avctx);
    if (ret < 0)
        return ret;

    if (avci->buffer_frame->buf[0]) {
        av_frame_move_ref(frame, avci->buffer_frame);
    } else {
        ret = decode_receive_frame_internal(avctx, frame);
        if (ret < 0)
            return ret;
    }

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = apply_cropping(avctx, frame);
        if (ret < 0) {
            av_frame_unref(frame);
            return ret;
        }
    }

    avctx->frame_number++;

    return 0;
}

static int compat_decode(AVCodecContext *avctx, AVFrame *frame,
                         int *got_frame, AVPacket *pkt)
{
    AVCodecInternal *avci = avctx->internal;
    int ret;

    av_assert0(avci->compat_decode_consumed == 0);

    *got_frame = 0;
    avci->compat_decode = 1;

    if (avci->compat_decode_partial_size > 0 &&
        avci->compat_decode_partial_size != pkt->size) {
        av_log(avctx, AV_LOG_ERROR,
               "Got unexpected packet size after a partial decode\n");
        ret = AVERROR(EINVAL);
        goto finish;
    }

    if (!avci->compat_decode_partial_size) {
        ret = avcodec_send_packet(avctx, pkt);
        if (ret == AVERROR_EOF)
            ret = 0;
        else if (ret == AVERROR(EAGAIN)) {
            /* we fully drain all the output in each decode call, so this should not
             * ever happen */
            ret = AVERROR_BUG;
            goto finish;
        } else if (ret < 0)
            goto finish;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(avctx, frame);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
            goto finish;
        }

        if (frame != avci->compat_decode_frame) {
            if (!avctx->refcounted_frames) {
                ret = unrefcount_frame(avci, frame);
                if (ret < 0)
                    goto finish;
            }

            *got_frame = 1;
            frame = avci->compat_decode_frame;
        } else {
            if (!avci->compat_decode_warned) {
                av_log(avctx, AV_LOG_WARNING, "The deprecated avcodec_decode_* "
                       "API cannot return all the frames for this decoder. "
                       "Some frames will be dropped. Update your code to the "
                       "new decoding API to fix this.\n");
                avci->compat_decode_warned = 1;
            }
        }

        if (avci->draining || (!avctx->codec->bsfs && avci->compat_decode_consumed < pkt->size))
            break;
    }

finish:
    if (ret == 0) {
        /* if there are any bsfs then assume full packet is always consumed */
        if (avctx->codec->bsfs)
            ret = pkt->size;
        else
            ret = FFMIN(avci->compat_decode_consumed, pkt->size);
    }
    avci->compat_decode_consumed = 0;
    avci->compat_decode_partial_size = (ret >= 0) ? pkt->size - ret : 0;

    return ret;
}

int attribute_align_arg avcodec_decode_video2(AVCodecContext *avctx, AVFrame *picture,
                                              int *got_picture_ptr,
                                              AVPacket *avpkt)
{
    return compat_decode(avctx, picture, got_picture_ptr, avpkt);
}

int attribute_align_arg avcodec_decode_audio4(AVCodecContext *avctx,
                                              AVFrame *frame,
                                              int *got_frame_ptr,
                                              AVPacket *avpkt)
{
    return compat_decode(avctx, frame, got_frame_ptr, avpkt);
}

int avcodec_decode_subtitle2(AVCodecContext *avctx, AVSubtitle *sub,
                             int *got_sub_ptr,
                             AVPacket *avpkt)
{
    int ret;

    ret = extract_packet_props(avctx->internal, avpkt);
    if (ret < 0)
        return ret;

    *got_sub_ptr = 0;
    ret = avctx->codec->decode(avctx, sub, got_sub_ptr, avpkt);
    if (*got_sub_ptr)
        avctx->frame_number++;
    return ret;
}

static int is_hwaccel_pix_fmt(enum AVPixelFormat pix_fmt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    return desc->flags & AV_PIX_FMT_FLAG_HWACCEL;
}

enum AVPixelFormat avcodec_default_get_format(struct AVCodecContext *s, const enum AVPixelFormat *fmt)
{
    while (*fmt != AV_PIX_FMT_NONE && is_hwaccel_pix_fmt(*fmt))
        ++fmt;
    return fmt[0];
}

static AVHWAccel *find_hwaccel(enum AVCodecID codec_id,
                               enum AVPixelFormat pix_fmt)
{
    AVHWAccel *hwaccel = NULL;

    while ((hwaccel = av_hwaccel_next(hwaccel)))
        if (hwaccel->id == codec_id
            && hwaccel->pix_fmt == pix_fmt)
            return hwaccel;
    return NULL;
}

static int setup_hwaccel(AVCodecContext *avctx,
                         const enum AVPixelFormat fmt,
                         const char *name)
{
    AVHWAccel *hwa = find_hwaccel(avctx->codec_id, fmt);
    int ret        = 0;

    if (!hwa) {
        av_log(avctx, AV_LOG_ERROR,
               "Could not find an AVHWAccel for the pixel format: %s",
               name);
        return AVERROR(ENOENT);
    }

    if (hwa->priv_data_size) {
        avctx->internal->hwaccel_priv_data = av_mallocz(hwa->priv_data_size);
        if (!avctx->internal->hwaccel_priv_data)
            return AVERROR(ENOMEM);
    }

    if (hwa->init) {
        ret = hwa->init(avctx);
        if (ret < 0) {
            av_freep(&avctx->internal->hwaccel_priv_data);
            return ret;
        }
    }

    avctx->hwaccel = hwa;

    return 0;
}

int ff_get_format(AVCodecContext *avctx, const enum AVPixelFormat *fmt)
{
    const AVPixFmtDescriptor *desc;
    enum AVPixelFormat *choices;
    enum AVPixelFormat ret;
    unsigned n = 0;

    while (fmt[n] != AV_PIX_FMT_NONE)
        ++n;

    av_assert0(n >= 1);
    avctx->sw_pix_fmt = fmt[n - 1];
    av_assert2(!is_hwaccel_pix_fmt(avctx->sw_pix_fmt));

    choices = av_malloc_array(n + 1, sizeof(*choices));
    if (!choices)
        return AV_PIX_FMT_NONE;

    memcpy(choices, fmt, (n + 1) * sizeof(*choices));

    for (;;) {
        if (avctx->hwaccel && avctx->hwaccel->uninit)
            avctx->hwaccel->uninit(avctx);
        av_freep(&avctx->internal->hwaccel_priv_data);
        avctx->hwaccel = NULL;

        av_buffer_unref(&avctx->hw_frames_ctx);

        ret = avctx->get_format(avctx, choices);

        desc = av_pix_fmt_desc_get(ret);
        if (!desc) {
            ret = AV_PIX_FMT_NONE;
            break;
        }

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            break;

        if (avctx->hw_frames_ctx) {
            AVHWFramesContext *hw_frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
            if (hw_frames_ctx->format != ret) {
                av_log(avctx, AV_LOG_ERROR, "Format returned from get_buffer() "
                       "does not match the format of provided AVHWFramesContext\n");
                ret = AV_PIX_FMT_NONE;
                break;
            }
        }

        if (!setup_hwaccel(avctx, ret, desc->name))
            break;

        /* Remove failed hwaccel from choices */
        for (n = 0; choices[n] != ret; n++)
            av_assert0(choices[n] != AV_PIX_FMT_NONE);

        do
            choices[n] = choices[n + 1];
        while (choices[n++] != AV_PIX_FMT_NONE);
    }

    av_freep(&choices);
    return ret;
}

static int update_frame_pool(AVCodecContext *avctx, AVFrame *frame)
{
    FramePool *pool = avctx->internal->pool;
    int i, ret;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO: {
        uint8_t *data[4];
        int linesize[4];
        int size[4] = { 0 };
        int w = frame->width;
        int h = frame->height;
        int tmpsize, unaligned;

        if (pool->format == frame->format &&
            pool->width == frame->width && pool->height == frame->height)
            return 0;

        avcodec_align_dimensions2(avctx, &w, &h, pool->stride_align);

        do {
            // NOTE: do not align linesizes individually, this breaks e.g. assumptions
            // that linesize[0] == 2*linesize[1] in the MPEG-encoder for 4:2:2
            av_image_fill_linesizes(linesize, avctx->pix_fmt, w);
            // increase alignment of w for next try (rhs gives the lowest bit set in w)
            w += w & ~(w - 1);

            unaligned = 0;
            for (i = 0; i < 4; i++)
                unaligned |= linesize[i] % pool->stride_align[i];
        } while (unaligned);

        tmpsize = av_image_fill_pointers(data, avctx->pix_fmt, h,
                                         NULL, linesize);
        if (tmpsize < 0)
            return -1;

        for (i = 0; i < 3 && data[i + 1]; i++)
            size[i] = data[i + 1] - data[i];
        size[i] = tmpsize - (data[i] - data[0]);

        for (i = 0; i < 4; i++) {
            av_buffer_pool_uninit(&pool->pools[i]);
            pool->linesize[i] = linesize[i];
            if (size[i]) {
                pool->pools[i] = av_buffer_pool_init(size[i] + 16, NULL);
                if (!pool->pools[i]) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
            }
        }
        pool->format = frame->format;
        pool->width  = frame->width;
        pool->height = frame->height;

        break;
        }
    case AVMEDIA_TYPE_AUDIO: {
        int ch     = frame->ch_layout.nb_channels;
        int planar = av_sample_fmt_is_planar(frame->format);
        int planes = planar ? avctx->ch_layout.nb_channels : 1;

        if (pool->format == frame->format && pool->planes == planes &&
            pool->channels == ch && frame->nb_samples == pool->samples)
            return 0;

        av_buffer_pool_uninit(&pool->pools[0]);
        ret = av_samples_get_buffer_size(&pool->linesize[0], ch,
                                         frame->nb_samples, frame->format, 0);
        if (ret < 0)
            goto fail;

        pool->pools[0] = av_buffer_pool_init(pool->linesize[0], NULL);
        if (!pool->pools[0]) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        pool->format     = frame->format;
        pool->planes     = planes;
        pool->channels   = ch;
        pool->samples = frame->nb_samples;
        break;
        }
    default: av_assert0(0);
    }
    return 0;
fail:
    for (i = 0; i < 4; i++)
        av_buffer_pool_uninit(&pool->pools[i]);
    pool->format = -1;
    pool->planes = pool->channels = pool->samples = 0;
    pool->width  = pool->height = 0;
    return ret;
}

static int audio_get_buffer(AVCodecContext *avctx, AVFrame *frame)
{
    FramePool *pool = avctx->internal->pool;
    int planes = pool->planes;
    int i;

    frame->linesize[0] = pool->linesize[0];

    if (planes > AV_NUM_DATA_POINTERS) {
        frame->extended_data = av_mallocz(planes * sizeof(*frame->extended_data));
        frame->nb_extended_buf = planes - AV_NUM_DATA_POINTERS;
        frame->extended_buf  = av_mallocz(frame->nb_extended_buf *
                                          sizeof(*frame->extended_buf));
        if (!frame->extended_data || !frame->extended_buf) {
            av_freep(&frame->extended_data);
            av_freep(&frame->extended_buf);
            return AVERROR(ENOMEM);
        }
    } else
        frame->extended_data = frame->data;

    for (i = 0; i < FFMIN(planes, AV_NUM_DATA_POINTERS); i++) {
        frame->buf[i] = av_buffer_pool_get(pool->pools[0]);
        if (!frame->buf[i])
            goto fail;
        frame->extended_data[i] = frame->data[i] = frame->buf[i]->data;
    }
    for (i = 0; i < frame->nb_extended_buf; i++) {
        frame->extended_buf[i] = av_buffer_pool_get(pool->pools[0]);
        if (!frame->extended_buf[i])
            goto fail;
        frame->extended_data[i + AV_NUM_DATA_POINTERS] = frame->extended_buf[i]->data;
    }

    if (avctx->debug & FF_DEBUG_BUFFERS)
        av_log(avctx, AV_LOG_DEBUG, "default_get_buffer called on frame %p", frame);

    return 0;
fail:
    av_frame_unref(frame);
    return AVERROR(ENOMEM);
}

static int video_get_buffer(AVCodecContext *s, AVFrame *pic)
{
    FramePool *pool = s->internal->pool;
    int i;

    if (pic->data[0]) {
        av_log(s, AV_LOG_ERROR, "pic->data[0]!=NULL in avcodec_default_get_buffer\n");
        return -1;
    }

    memset(pic->data, 0, sizeof(pic->data));
    pic->extended_data = pic->data;

    for (i = 0; i < 4 && pool->pools[i]; i++) {
        pic->linesize[i] = pool->linesize[i];

        pic->buf[i] = av_buffer_pool_get(pool->pools[i]);
        if (!pic->buf[i])
            goto fail;

        pic->data[i] = pic->buf[i]->data;
    }
    for (; i < AV_NUM_DATA_POINTERS; i++) {
        pic->data[i] = NULL;
        pic->linesize[i] = 0;
    }
    if (pic->data[1] && !pic->data[2])
        avpriv_set_systematic_pal2((uint32_t *)pic->data[1], s->pix_fmt);

    if (s->debug & FF_DEBUG_BUFFERS)
        av_log(s, AV_LOG_DEBUG, "default_get_buffer called on pic %p\n", pic);

    return 0;
fail:
    av_frame_unref(pic);
    return AVERROR(ENOMEM);
}

int avcodec_default_get_buffer2(AVCodecContext *avctx, AVFrame *frame, int flags)
{
    int ret;

    if (avctx->hw_frames_ctx)
        return av_hwframe_get_buffer(avctx->hw_frames_ctx, frame, 0);

    if ((ret = update_frame_pool(avctx, frame)) < 0)
        return ret;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        return video_get_buffer(avctx, frame);
    case AVMEDIA_TYPE_AUDIO:
        return audio_get_buffer(avctx, frame);
    default:
        return -1;
    }
}

int ff_decode_frame_props(AVCodecContext *avctx, AVFrame *frame)
{
    AVPacket *pkt = avctx->internal->last_pkt_props;
    int i;
    struct {
        enum AVPacketSideDataType packet;
        enum AVFrameSideDataType frame;
    } sd[] = {
        { AV_PKT_DATA_REPLAYGAIN ,   AV_FRAME_DATA_REPLAYGAIN },
        { AV_PKT_DATA_DISPLAYMATRIX, AV_FRAME_DATA_DISPLAYMATRIX },
        { AV_PKT_DATA_SPHERICAL,     AV_FRAME_DATA_SPHERICAL },
        { AV_PKT_DATA_STEREO3D,      AV_FRAME_DATA_STEREO3D },
        { AV_PKT_DATA_AUDIO_SERVICE_TYPE, AV_FRAME_DATA_AUDIO_SERVICE_TYPE },
    };

    frame->color_primaries = avctx->color_primaries;
    frame->color_trc       = avctx->color_trc;
    frame->colorspace      = avctx->colorspace;
    frame->color_range     = avctx->color_range;
    frame->chroma_location = avctx->chroma_sample_location;

    frame->reordered_opaque = avctx->reordered_opaque;

#if FF_API_PKT_PTS
FF_DISABLE_DEPRECATION_WARNINGS
    frame->pkt_pts = pkt->pts;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    frame->pts     = pkt->pts;

    for (i = 0; i < FF_ARRAY_ELEMS(sd); i++) {
        int size;
        uint8_t *packet_sd = av_packet_get_side_data(pkt, sd[i].packet, &size);
        if (packet_sd) {
            AVFrameSideData *frame_sd = av_frame_new_side_data(frame,
                                                               sd[i].frame,
                                                               size);
            if (!frame_sd)
                return AVERROR(ENOMEM);

            memcpy(frame_sd->data, packet_sd, size);
        }
    }

    return 0;
}

int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags)
{
    const AVHWAccel *hwaccel = avctx->hwaccel;
    int override_dimensions = 1;
    int ret;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (frame->width <= 0 || frame->height <= 0) {
            frame->width  = FFMAX(avctx->width, avctx->coded_width);
            frame->height = FFMAX(avctx->height, avctx->coded_height);
            override_dimensions = 0;
        }
        if (frame->format < 0)
            frame->format              = avctx->pix_fmt;
        if (!frame->sample_aspect_ratio.num)
            frame->sample_aspect_ratio = avctx->sample_aspect_ratio;

        if (av_image_check_sar(frame->width, frame->height,
                               frame->sample_aspect_ratio) < 0) {
            av_log(avctx, AV_LOG_WARNING, "ignoring invalid SAR: %u/%u\n",
                   frame->sample_aspect_ratio.num,
                   frame->sample_aspect_ratio.den);
            frame->sample_aspect_ratio = (AVRational){ 0, 1 };
        }

        if ((ret = av_image_check_size(avctx->width, avctx->height, 0, avctx)) < 0)
            return ret;
        break;
    case AVMEDIA_TYPE_AUDIO:
        if (!frame->sample_rate)
            frame->sample_rate    = avctx->sample_rate;
        if (frame->format < 0)
            frame->format         = avctx->sample_fmt;
        if (!frame->ch_layout.nb_channels) {
            ret = av_channel_layout_copy(&frame->ch_layout, &avctx->ch_layout);
            if (ret < 0)
                return ret;
        }
        ret = av_channel_layout_check(&frame->ch_layout);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid channel layout.\n");
            return ret;
        }
#if FF_API_OLD_CHANNEL_LAYOUT
FF_DISABLE_DEPRECATION_WARNINGS
        /* set the deprecated channel_layout field for callers
         * that didn't update to the new API yet */
        if (frame->ch_layout.nb_channels > FF_SANE_NB_CHANNELS) {
            av_log(avctx, AV_LOG_ERROR, "Too many channels.\n");
            return AVERROR(EINVAL);
        }
        if (frame->ch_layout.order == AV_CHANNEL_ORDER_NATIVE)
            frame->channel_layout = frame->ch_layout.u.mask;
        else {
            frame->channel_layout = av_get_default_channel_layout(frame->ch_layout.nb_channels);
            if (!frame->channel_layout)
                frame->channel_layout = (1ULL << frame->ch_layout.nb_channels) - 1;
        }
FF_ENABLE_DEPRECATION_WARNINGS
#endif
        break;
    default: return AVERROR(EINVAL);
    }

    ret = ff_decode_frame_props(avctx, frame);
    if (ret < 0)
        return ret;

    if (hwaccel) {
        if (hwaccel->alloc_frame) {
            ret = hwaccel->alloc_frame(avctx, frame);
            goto end;
        }
    } else
        avctx->sw_pix_fmt = avctx->pix_fmt;

    ret = avctx->get_buffer2(avctx, frame, flags);

end:
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO && !override_dimensions &&
        !(avctx->codec->caps_internal & FF_CODEC_CAP_EXPORTS_CROPPING)) {
        frame->width  = avctx->width;
        frame->height = avctx->height;
    }

    return ret;
}

int ff_reget_buffer(AVCodecContext *avctx, AVFrame *frame)
{
    AVFrame *tmp;
    int ret;

    av_assert0(avctx->codec_type == AVMEDIA_TYPE_VIDEO);

    if (!frame->data[0])
        return ff_get_buffer(avctx, frame, AV_GET_BUFFER_FLAG_REF);

    if (av_frame_is_writable(frame))
        return ff_decode_frame_props(avctx, frame);

    tmp = av_frame_alloc();
    if (!tmp)
        return AVERROR(ENOMEM);

    av_frame_move_ref(tmp, frame);

    ret = ff_get_buffer(avctx, frame, AV_GET_BUFFER_FLAG_REF);
    if (ret < 0) {
        av_frame_free(&tmp);
        return ret;
    }

    av_frame_copy(frame, tmp);
    av_frame_free(&tmp);

    return 0;
}

void avcodec_flush_buffers(AVCodecContext *avctx)
{
    avctx->internal->draining      = 0;
    avctx->internal->draining_done = 0;
    av_frame_unref(avctx->internal->buffer_frame);
    av_frame_unref(avctx->internal->compat_decode_frame);
    av_packet_unref(avctx->internal->buffer_pkt);
    avctx->internal->buffer_pkt_valid = 0;

    av_packet_unref(avctx->internal->ds.in_pkt);

    if (HAVE_THREADS && avctx->active_thread_type & FF_THREAD_FRAME)
        ff_thread_flush(avctx);
    else if (avctx->codec->flush)
        avctx->codec->flush(avctx);

    ff_decode_bsfs_uninit(avctx);

    if (!avctx->refcounted_frames)
        av_frame_unref(avctx->internal->to_free);
}

void ff_decode_bsfs_uninit(AVCodecContext *avctx)
{
    DecodeFilterContext *s = &avctx->internal->filter;
    int i;

    for (i = 0; i < s->nb_bsfs; i++)
        av_bsf_free(&s->bsfs[i]);
    av_freep(&s->bsfs);
    s->nb_bsfs = 0;
}
