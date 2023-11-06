/*
 * WebRTC-HTTP egress protocol (WHEP) demuxer using libdatachannel
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

#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/random_seed.h"
#include "version.h"
#include "rtsp.h"
#include "webrtc.h"

typedef struct WHEPContext {
    const AVClass *av_class;
    DataChannelContext data_channel;
} WHEPContext;

static int whep_read_header(AVFormatContext* avctx)
{
    WHEPContext*const ctx = (WHEPContext*const)avctx->priv_data;
    int ret, i;
    char media_stream_id[37] = { 0 };
    rtcTrackInit track_init;
    AVDictionary* options = NULL;
    const AVInputFormat* infmt;
    AVStream* stream;
    FFIOContext sdp_pb;

    webrtc_init_logger();
    ret = webrtc_init_connection(&ctx->data_channel);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize connection\n");
        goto fail;
    }

    /* configure audio and video track */
    ret = webrtc_generate_media_stream_id(media_stream_id);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to generate media stream id\n");
        goto fail;
    }
    ctx->data_channel.tracks = av_mallocz(2 * sizeof(DataChannelTrack));
    ctx->data_channel.nb_tracks = 2;
    ctx->data_channel.avctx = avctx;
    if (!ctx->data_channel.tracks) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    for (i=0; i < ctx->data_channel.nb_tracks; i++) {
        ctx->data_channel.tracks[i].avctx = avctx;
    }

    /* configure video track */
    memset(&track_init, 0, sizeof(rtcTrackInit));
    track_init.direction = RTC_DIRECTION_RECVONLY;
    track_init.codec = RTC_CODEC_H264; // TODO: support more codecs once libdatachannel C api supports them
    track_init.payloadType = 96;
    track_init.ssrc = av_get_random_seed();
    track_init.mid = "0";
    track_init.name = LIBAVFORMAT_IDENT;
    track_init.msid = media_stream_id;
    track_init.trackId = av_asprintf("%s-video", media_stream_id);
    track_init.profile = "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1";

    ctx->data_channel.tracks[0].track_id = rtcAddTrackEx(ctx->data_channel.peer_connection, &track_init);
    if (!ctx->data_channel.tracks[0].track_id) {
        av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    /* configure audio track */
    memset(&track_init, 0, sizeof(rtcTrackInit));
    track_init.direction = RTC_DIRECTION_RECVONLY;
    track_init.codec = RTC_CODEC_OPUS; // TODO: support more codecs once libdatachannel C api supports them
    track_init.payloadType = 97;
    track_init.ssrc = av_get_random_seed();
    track_init.mid = "1";
    track_init.name = LIBAVFORMAT_IDENT;
    track_init.msid = media_stream_id;
    track_init.trackId = av_asprintf("%s-audio", media_stream_id);
    track_init.profile = "minptime=10;maxaveragebitrate=96000;stereo=1;sprop-stereo=1;useinbandfec=1";

    ctx->data_channel.tracks[1].track_id = rtcAddTrackEx(ctx->data_channel.peer_connection, &track_init);
    if (!ctx->data_channel.tracks[1].track_id) {
        av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    /* create resource */
    ret = webrtc_create_resource(&ctx->data_channel);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "webrtc_create_resource failed\n");
        goto fail;
    }

    /* initialize SDP muxer per track */
    for (int i = 0; i < ctx->data_channel.nb_tracks; i++) {
        char sdp_track[SDP_MAX_SIZE] = { 0 };
        ret = rtcGetTrackDescription(ctx->data_channel.tracks[i].track_id, sdp_track, sizeof(sdp_track));
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "rtcGetTrackDescription failed\n");
            goto fail;
        }

        ffio_init_read_context(&sdp_pb, (uint8_t*)sdp_track, strlen(sdp_track));

        infmt = av_find_input_format("sdp");
        if (!infmt)
            goto fail;
        ctx->data_channel.tracks[i].rtp_ctx = avformat_alloc_context();
        if (!ctx->data_channel.tracks[i].rtp_ctx) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ctx->data_channel.tracks[i].rtp_ctx->max_delay = avctx->max_delay;
        ctx->data_channel.tracks[i].rtp_ctx->pb = &sdp_pb.pub;
        ctx->data_channel.tracks[i].rtp_ctx->interrupt_callback = avctx->interrupt_callback;

        if ((ret = ff_copy_whiteblacklists(ctx->data_channel.tracks[i].rtp_ctx, avctx)) < 0)
            goto fail;

        av_dict_set(&options, "sdp_flags", "custom_io", 0);

        ret = avformat_open_input(&ctx->data_channel.tracks[i].rtp_ctx, "temp.sdp", infmt, &options);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "avformat_open_input failed\n");
            goto fail;
        }

        ret = webrtc_init_urlcontext(&ctx->data_channel, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "webrtc_init_urlcontext failed\n");
            goto fail;
        }
        ret = ffio_fdopen(&ctx->data_channel.tracks[i].rtp_ctx->pb, ctx->data_channel.tracks[i].rtp_url_context);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "ffio_fdopen failed\n");
            goto fail;
        }

        /* copy codec parameters */
        stream = avformat_new_stream(avctx, NULL);
        if (!stream) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = avcodec_parameters_copy(stream->codecpar, ctx->data_channel.tracks[i].rtp_ctx->streams[0]->codecpar);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "avcodec_parameters_copy failed\n");
            goto fail;
        }
        stream->time_base = ctx->data_channel.tracks[i].rtp_ctx->streams[0]->time_base;
    }

    return 0;

fail:
    webrtc_deinit(&ctx->data_channel);
    return ret;
}

static int whep_read_close(AVFormatContext* avctx)
{
    WHEPContext*const ctx = (WHEPContext*const)avctx->priv_data;
    int ret = 0;

    /* close resource */
    ret = webrtc_close_resource(&ctx->data_channel);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "webrtc_close_resource failed\n");
    }

    webrtc_deinit(&ctx->data_channel);

    return ret;
}

static int whep_read_packet(AVFormatContext* avctx, AVPacket* pkt)
{
    const WHEPContext*const s = (const WHEPContext*const)avctx->priv_data;
    const DataChannelTrack*const track = &s->data_channel.tracks[pkt->stream_index];
    pkt->stream_index = 0;
    return av_read_frame(track->rtp_ctx, pkt);
}


#define OFFSET(x) offsetof(WHEPContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    WEBRTC_OPTIONS(DEC, OFFSET(data_channel)),
    { NULL },
};

static const AVClass whep_demuxer_class = {
    .class_name = "WHEP demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const AVInputFormat ff_whep_demuxer = {
    .name             = "whep",
    .long_name        = NULL_IF_CONFIG_SMALL("WebRTC-HTTP egress protocol (WHEP) demuxer"),
    .flags            = AVFMT_NOFILE | AVFMT_EXPERIMENTAL,
    .priv_class       = &whep_demuxer_class,
    .priv_data_size   = sizeof(WHEPContext),
    .read_header      = whep_read_header,
    .read_packet      = whep_read_packet,
    .read_close       = whep_read_close,
};
