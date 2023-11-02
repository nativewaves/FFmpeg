/*
 * WebRTC-HTTP ingestion protocol (WHIP) muxer using libdatachannel
 *
 * Copyright (C) 2023 NativeWaves GmbH <contact@nativewaves.com>
 * This work is supported by FFG project 47168763.
 *
 * This file is part of FFmpeg.
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
 * License along with FFmpeg; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "avformat.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/time.h"
#include "mux.h"
#include "rtpenc.h"
#include "rtpenc_chain.h"
#include "rtsp.h"
#include "webrtc.h"

typedef struct WHIPContext {
    AVClass *av_class;
    DataChannelContext data_channel;
} WHIPContext;


static void whip_deinit(AVFormatContext* avctx);
static int whip_init(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    AVStream* stream;
    const AVCodecParameters* codecpar;
    int i, ret;
    const char* media_stream_id = NULL;
    rtcTrackInit track_init;
    const AVChannelLayout supported_layout = AV_CHANNEL_LAYOUT_STEREO;
    const RTPMuxContext* rtp_mux_ctx;
    DataChannelTrack* track;
    char sdp_stream[SDP_MAX_SIZE] = { 0 };
    char* fmtp;

    ctx->data_channel.avctx = avctx;
    webrtc_init_logger();
    ret = webrtc_init_connection(&ctx->data_channel);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize connection\n");
        goto fail;
    }

    if (!(ctx->data_channel.tracks = av_mallocz(sizeof(DataChannelTrack) * avctx->nb_streams))) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate tracks\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* configure tracks */
    media_stream_id = webrtc_generate_media_stream_id();
    for (i = 0; i < avctx->nb_streams; ++i) {
        stream = avctx->streams[i];
        codecpar = stream->codecpar;
        track = &ctx->data_channel.tracks[i];

        switch (codecpar->codec_type)
        {
            case AVMEDIA_TYPE_VIDEO:
                avpriv_set_pts_info(stream, 32, 1, 90000);
                break;
            case AVMEDIA_TYPE_AUDIO:
                if (codecpar->sample_rate != 48000) {
                    av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate\n");
                    ret = AVERROR(EINVAL);
                    goto fail;
                }
                if (av_channel_layout_compare(&codecpar->ch_layout, &supported_layout) != 0) {
                    av_log(avctx, AV_LOG_ERROR, "Unsupported channel layout\n");
                    ret = AVERROR(EINVAL);
                    goto fail;
                }
                avpriv_set_pts_info(stream, 32, 1, codecpar->sample_rate);
                break;
            default:
                continue;
        }

        ret = webrtc_init_urlcontext(&ctx->data_channel, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "webrtc_init_urlcontext failed\n");
            return ret;
        }

        ret = ff_rtp_chain_mux_open(&track->rtp_ctx, avctx, stream, track->rtp_url_context, RTP_MAX_PACKET_SIZE, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_rtp_chain_mux_open failed\n");
            av_freep(&track->rtp_url_context);
            return ret;
        }
        rtp_mux_ctx = (const RTPMuxContext*)ctx->data_channel.tracks[i].rtp_ctx->priv_data;

        memset(&track_init, 0, sizeof(rtcTrackInit));
        track_init.direction = RTC_DIRECTION_SENDONLY;
        track_init.payloadType = rtp_mux_ctx->payload_type;
        track_init.ssrc = rtp_mux_ctx->ssrc;
        track_init.mid = av_asprintf("%d", i);
        track_init.name = rtp_mux_ctx->cname;
        track_init.msid = media_stream_id;
        track_init.trackId = av_asprintf("%s-video-%d", media_stream_id, i);

        ret = webrtc_convert_codec(codecpar->codec_id, &track_init.codec);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to convert codec\n");
            goto fail;
        }

        /* parse fmtp from global header */
        ret = ff_sdp_write_media(sdp_stream, sizeof(sdp_stream), stream, i, NULL, NULL, 0, 0, NULL);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to write sdp\n");
            goto fail;
        }
        fmtp = strstr(sdp_stream, "a=fmtp:");
        if (fmtp) {
            track_init.profile = fmtp+10;
        }

        track->track_id = rtcAddTrackEx(ctx->data_channel.peer_connection, &track_init);
        if (track->track_id < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }

    av_freep(&media_stream_id);
    return 0;

fail:
    if (media_stream_id) {
        av_freep(&media_stream_id);
    }
    whip_deinit(avctx);
    return ret;
}

static int whip_write_header(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    int ret;
    int64_t timeout;

    ret = webrtc_create_resource(&ctx->data_channel);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create resource\n");
        goto fail;
    }

    /* wait for connection to be established */
    timeout = av_gettime_relative() + ctx->data_channel.connection_timeout;
    while (ctx->data_channel.state != RTC_CONNECTED) {
        if (ctx->data_channel.state == RTC_FAILED || ctx->data_channel.state == RTC_CLOSED || av_gettime_relative() > timeout) {
            av_log(avctx, AV_LOG_ERROR, "Failed to open connection\n");
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        av_log(avctx, AV_LOG_VERBOSE, "Waiting for PeerConnection to open\n");
        av_usleep(1000);
    }

    return 0;

fail:
    whip_deinit(avctx);
    return ret;
}

static int whip_write_packet(AVFormatContext* avctx, AVPacket* pkt)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    AVFormatContext* rtpctx = ctx->data_channel.tracks[pkt->stream_index].rtp_ctx;
    pkt->stream_index = 0;
    return av_write_frame(rtpctx, pkt);
}

static int whip_write_trailer(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    return webrtc_close_resource(&ctx->data_channel);
}

static void whip_deinit(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    webrtc_deinit(&ctx->data_channel);
}

static int whip_check_bitstream(AVFormatContext *s, AVStream *st, const AVPacket *pkt)
{
    if (st->codecpar->extradata_size && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        return ff_stream_add_bitstream_filter(st, "dump_extra", "freq=keyframe");
    return 1;
}

static int whip_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_OPUS:
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_PCM_MULAW:
        case AV_CODEC_ID_PCM_ALAW:
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_AV1:
        case AV_CODEC_ID_VP9:
            return 1;
        default:
            return 0;
    }
}

#define OFFSET(x) offsetof(WHIPContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    WEBRTC_OPTIONS(ENC, OFFSET(data_channel)),
    { NULL },
};

static const AVClass whip_muxer_class = {
    .class_name = "WHIP muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_whip_muxer = {
    .p.name             = "whip",
    .p.long_name        = NULL_IF_CONFIG_SMALL("WebRTC-HTTP ingestion protocol (WHIP) muxer"),
    .p.audio_codec      = AV_CODEC_ID_OPUS, // supported by major browsers
    .p.video_codec      = AV_CODEC_ID_H264,
    .p.flags            = AVFMT_NOFILE | AVFMT_GLOBALHEADER | AVFMT_EXPERIMENTAL,
    .p.priv_class       = &whip_muxer_class,
    .priv_data_size     = sizeof(WHIPContext),
    .write_packet       = whip_write_packet,
    .write_header       = whip_write_header,
    .write_trailer      = whip_write_trailer,
    .init               = whip_init,
    .deinit             = whip_deinit,
    .query_codec        = whip_query_codec,
    .check_bitstream    = whip_check_bitstream,
};
