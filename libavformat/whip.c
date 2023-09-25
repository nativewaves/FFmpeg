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

#include "internal.h"
#include "mux.h"
#include "url.h"
#include "version.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/log.h"
#include "libavutil/uuid.h"
#include "libavutil/random_seed.h"
#include "libavutil/time.h"
#include "rtp.h"
#include "rtc/rtc.h"

typedef struct WHIPContext {
    AVClass *av_class;
    int peer_connection;
    int* tracks;
    rtcState state;
    char* resource_location;

    char* bearer_token;
    int max_stored_packets_count;
    int64_t connection_timeout;
} WHIPContext;

static inline const char* whip_get_state_name(rtcState state)
{
    switch (state)
    {
        case RTC_NEW:
            return "RTC_NEW";
        case RTC_CONNECTING:
            return "RTC_CONNECTING";
        case RTC_CONNECTED:
            return "RTC_CONNECTED";
        case RTC_DISCONNECTED:
            return "RTC_DISCONNECTED";
        case RTC_FAILED:
            return "RTC_FAILED";
        case RTC_CLOSED:
            return "RTC_CLOSED";
        default:
            return "UNKNOWN";
    }
}

static void whip_on_state_change(int pc, rtcState state, void* ptr)
{
    AVFormatContext* avctx = (AVFormatContext*)ptr;
    WHIPContext* s = (WHIPContext*)avctx->priv_data;

    av_log(avctx, AV_LOG_VERBOSE, "Connection state changed from %s to %s\n", whip_get_state_name(s->state), whip_get_state_name(state));
    s->state = state;
}

static void whip_rtc_log(rtcLogLevel rtcLevel, const char *message)
{
    int level = AV_LOG_INFO;
    switch (rtcLevel)
    {
        case RTC_LOG_NONE:
            level = AV_LOG_QUIET;
            break;
        case RTC_LOG_DEBUG:
            level = AV_LOG_DEBUG;
            break;
        case RTC_LOG_VERBOSE:
        case RTC_LOG_INFO:
            level = AV_LOG_VERBOSE;
            break;
        case RTC_LOG_WARNING:
            level = AV_LOG_WARNING;
            break;
        case RTC_LOG_ERROR:
            level = AV_LOG_ERROR;
            break;
        case RTC_LOG_FATAL:
            level = AV_LOG_FATAL;
            break;
    }
    av_log(NULL, level, "[libdatachannel] %s\n", message);
}

static void generate_random_uuid(char buffer[37])
{
    AVUUID uuid;
    av_random_bytes(uuid, sizeof(uuid));
    av_uuid_unparse(uuid, buffer);
}

static void whip_deinit(AVFormatContext* avctx);
static int whip_init(AVFormatContext* avctx)
{
    WHIPContext* s = avctx->priv_data;
    rtcConfiguration config;
    AVStream* stream;
    const AVCodecParameters* codecpar;
    int i, ret;
    char media_stream_id[37];
    rtcTrackInit track_init;
    rtcPacketizationHandlerInit packetizer_init;
    uint32_t ssrc;
    uint8_t payload_type;
    uint32_t clock_rate;
    rtcCodec codec;
    const AVChannelLayout supported_layout = AV_CHANNEL_LAYOUT_STEREO;

    memset(&config, 0, sizeof(rtcConfiguration));

    rtcInitLogger(RTC_LOG_DEBUG, whip_rtc_log);

    if (!(s->peer_connection = rtcCreatePeerConnection(&config))) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create PeerConnection\n");
        return AVERROR_EXTERNAL;
    }
    rtcSetUserPointer(s->peer_connection, avctx);
    if (rtcSetStateChangeCallback(s->peer_connection, whip_on_state_change)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set state change callback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    /* configure tracks */
    generate_random_uuid(media_stream_id);
    if (!(s->tracks = av_malloc(sizeof(int) * avctx->nb_streams))) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate tracks\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    for (i = 0; i < avctx->nb_streams; ++i) {
        stream = avctx->streams[i];
        codecpar = stream->codecpar;

        ssrc = av_get_random_seed();
        payload_type = (uint8_t)ff_rtp_get_payload_type(NULL, codecpar, i);
        av_log(avctx, AV_LOG_VERBOSE, "ssrc: %u, payload_type: %u\n", ssrc, payload_type);

        if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (codecpar->sample_rate != 48000) {
                av_log(avctx, AV_LOG_ERROR, "Unsupported audio sample rate. Supported sample rate is 48000\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }

            if (av_channel_layout_compare(&codecpar->ch_layout, &supported_layout)) {
                av_log(avctx, AV_LOG_ERROR, "Unsupported audio channel layout. Supported layout is stereo\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }

            clock_rate = codecpar->sample_rate;
            avpriv_set_pts_info(stream, 32, 1, clock_rate);

            switch (codecpar->codec_id) {
                case AV_CODEC_ID_OPUS:
                    codec = RTC_CODEC_OPUS;
                    break;
                case AV_CODEC_ID_AAC:
                    codec = RTC_CODEC_AAC;
                    break;
                default:
                    av_log(avctx, AV_LOG_ERROR, "Unsupported audio codec\n");
                    ret = AVERROR(EINVAL);
                    goto fail;
            }

            memset(&track_init, 0, sizeof(rtcTrackInit));
            track_init.direction = RTC_DIRECTION_SENDONLY;
            track_init.codec = codec;
            track_init.payloadType = payload_type;
            track_init.ssrc = ssrc;
            track_init.mid = av_asprintf("%d", i);
            track_init.name = LIBAVFORMAT_IDENT;
            track_init.msid = media_stream_id;
            track_init.trackId = av_asprintf("%s-audio-%d", media_stream_id, i);

            memset(&packetizer_init, 0, sizeof(rtcPacketizationHandlerInit));
            packetizer_init.ssrc = ssrc;
            packetizer_init.cname = LIBAVFORMAT_IDENT;
            packetizer_init.payloadType = payload_type;
            packetizer_init.clockRate = clock_rate;

            s->tracks[i] = rtcAddTrackEx(s->peer_connection, &track_init);
            if (!s->tracks[i]) {
                av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }
            if (codec == RTC_CODEC_OPUS) {
                if (rtcSetOpusPacketizationHandler(s->tracks[i], &packetizer_init)) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to set Opus packetization handler\n");
                    ret = AVERROR_EXTERNAL;
                    goto fail;
                }
            }
            else if (codec == RTC_CODEC_AAC) {
                if (rtcSetAACPacketizationHandler(s->tracks[i], &packetizer_init)) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to set AAC packetization handler\n");
                    ret = AVERROR_EXTERNAL;
                    goto fail;
                }
            }
            if (rtcChainRtcpSrReporter(s->tracks[i])) {
                av_log(avctx, AV_LOG_ERROR, "Failed to chain RTCP SR reporter\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
            if (rtcChainRtcpNackResponder(s->tracks[i], s->max_stored_packets_count)) {
                av_log(avctx, AV_LOG_ERROR, "Failed to chain RTCP NACK responder\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
        }
        else if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            clock_rate = 90000;
            avpriv_set_pts_info(stream, 32, 1, clock_rate);

            switch (codecpar->codec_id)
            {
                case AV_CODEC_ID_H264:
                    codec = RTC_CODEC_H264;
                    break;
                case AV_CODEC_ID_HEVC:
                    codec = RTC_CODEC_H265;
                    break;
                default:
                    av_log(avctx, AV_LOG_ERROR, "Unsupported video codec\n");
                    ret = AVERROR(EINVAL);
                    goto fail;
            }

            memset(&track_init, 0, sizeof(rtcTrackInit));
            track_init.direction = RTC_DIRECTION_SENDONLY;
            track_init.codec = codec;
            track_init.payloadType = payload_type;
            track_init.ssrc = ssrc;
            track_init.mid = av_asprintf("%d", i);
            track_init.name = LIBAVFORMAT_IDENT;
            track_init.msid = media_stream_id;
            track_init.trackId = av_asprintf("%s-video-%d", media_stream_id, i);

            memset(&packetizer_init, 0, sizeof(rtcPacketizationHandlerInit));
            packetizer_init.ssrc = ssrc;
            packetizer_init.cname = LIBAVFORMAT_IDENT;
            packetizer_init.payloadType = payload_type;
            packetizer_init.clockRate = clock_rate;
            packetizer_init.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;

            if (!(s->tracks[i] = rtcAddTrackEx(s->peer_connection, &track_init))) {
                av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
            if (codec == RTC_CODEC_H264) {
                if (rtcSetH264PacketizationHandler(s->tracks[i], &packetizer_init)) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to set H264 packetization handler\n");
                    ret = AVERROR_EXTERNAL;
                    goto fail;
                }
            }
            else if (codec == RTC_CODEC_H265) {
                if (rtcSetH265PacketizationHandler(s->tracks[i], &packetizer_init)) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to set H265 packetization handler\n");
                    ret = AVERROR_EXTERNAL;
                    goto fail;
                }
            }
            if (rtcChainRtcpSrReporter(s->tracks[i])) {
                av_log(avctx, AV_LOG_ERROR, "Failed to chain RTCP SR reporter\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
            if (rtcChainRtcpNackResponder(s->tracks[i], s->max_stored_packets_count)) {
                av_log(avctx, AV_LOG_ERROR, "Failed to chain RTCP NACK responder\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
        }
    }

    if (rtcSetLocalDescription(s->peer_connection, "offer")) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set local description\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    return 0;

fail:
    whip_deinit(avctx);
    return ret;
}

static int whip_write_header(AVFormatContext* avctx)
{
    WHIPContext* s = avctx->priv_data;
    int ret;
    URLContext* h = NULL;
    int64_t timeout;
    char* headers;
    char offer_sdp[4096] = {0};
    char response[4096] = {0};

    if (rtcGetLocalDescription(s->peer_connection, offer_sdp, sizeof(offer_sdp)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get local description\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(avctx, AV_LOG_VERBOSE, "offer_sdp: %s\n", offer_sdp);

    /* alloc the http context */
    if ((ret = ffurl_alloc(&h, avctx->url, AVIO_FLAG_READ_WRITE, NULL)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "ffurl_alloc failed\n");
        goto fail;
    }
    /* set options */
    headers = av_asprintf("Content-type: application/sdp\r\n");
    if (s->bearer_token) {
        headers = av_asprintf("%sAuthorization: Bearer %s\r\n", headers, s->bearer_token);
    }
    av_log(avctx, AV_LOG_VERBOSE, "headers: %s\n", headers);
    av_opt_set(h->priv_data, "headers", headers, 0);
    av_opt_set(h->priv_data, "method", "POST", 0);
    av_opt_set_bin(h->priv_data, "post_data", (uint8_t*)offer_sdp, strlen(offer_sdp), 0);

    /* open the http context */
    if ((ret = ffurl_connect(h, NULL)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "ffurl_connect failed\n");
        goto fail;
    }

    /* read the server reply which contains a unique ID */
    ret = ffurl_read_complete(h, (unsigned char*)response, sizeof(response));
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "ffurl_read_complete failed\n");
        goto fail;
    }

    av_log(avctx, AV_LOG_VERBOSE, "response: %s\n", response);
    if (rtcSetRemoteDescription(s->peer_connection, response, "answer")) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set remote description\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    /* save resource location for later use */
    av_opt_get(h->priv_data, "new_location", AV_OPT_SEARCH_CHILDREN, (uint8_t**)&s->resource_location);
    av_log(avctx, AV_LOG_VERBOSE, "resource_location: %s\n", s->resource_location);

    /* close the http context */
    if ((ret = ffurl_closep(&h)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "ffurl_closep &failed\n");
        goto fail;
    }

    /* wait for connection to be established */
    timeout = av_gettime_relative() + s->connection_timeout;
    while (s->state != RTC_CONNECTED) {
        if (s->state == RTC_FAILED || s->state == RTC_CLOSED || av_gettime_relative() > timeout) {
            av_log(avctx, AV_LOG_ERROR, "Failed to open connection\n");
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        av_log(avctx, AV_LOG_VERBOSE, "Waiting for PeerConnection to open\n");
        av_usleep(100000);
    }

    return 0;

fail:
    if (h) {
        ffurl_closep(&h);
    }
    whip_deinit(avctx);
    return ret;
}

static int whip_write_packet(AVFormatContext* avctx, AVPacket* pkt)
{
    const WHIPContext* s = avctx->priv_data;

    if (s->state == RTC_DISCONNECTED || s->state == RTC_FAILED || s->state == RTC_CLOSED) {
        return AVERROR_EOF;
    }

    if (pkt->pts < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid packet PTS, dropping packet\n");
        return AVERROR(EINVAL);
    }

    if (rtcSetTrackRtpTimestamp(s->tracks[pkt->stream_index], (uint32_t)pkt->pts)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set track RTP timestamp\n");
        return AVERROR_EXTERNAL;
    }

    if (rtcSendMessage(s->tracks[pkt->stream_index], (const char*)pkt->data, pkt->size)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to send message\n");
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static int whip_write_trailer(AVFormatContext* avctx)
{
    WHIPContext* s = avctx->priv_data;
    URLContext* h = NULL;
    int ret;
    char* headers;

    if (s->resource_location) {
        av_log(avctx, AV_LOG_VERBOSE, "Closing resource %s\n", s->resource_location);

        /* alloc the http context */
        if ((ret = ffurl_alloc(&h, s->resource_location, AVIO_FLAG_READ_WRITE, NULL)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "ffurl_alloc failed\n");
            goto fail;
        }

        /* set options */
        if (s->bearer_token) {
            headers = av_asprintf("Authorization: Bearer %s\r\n", s->bearer_token);
            av_log(avctx, AV_LOG_VERBOSE, "headers: %s\n", headers);
        }
        av_opt_set(h->priv_data, "method", "DELETE", 0);

        /* open the http context */
        if ((ret = ffurl_connect(h, NULL)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "ffurl_connect failed\n");
            goto fail;
        }

        /* close the http context */
        if ((ret = ffurl_closep(&h)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "ffurl_close failed\n");
            goto fail;
        }

        av_freep(&s->resource_location);
    }

    return 0;

fail:
    if (h) {
        ffurl_closep(&h);
    }
    return ret;
}

static void whip_deinit(AVFormatContext* avctx)
{
    WHIPContext* s = avctx->priv_data;
    if (s->tracks) {
        for (int i = 0; i < avctx->nb_streams; ++i) {
            if (s->tracks[i]) {
                rtcDeleteTrack(s->tracks[i]);
            }
        }
        av_freep(&s->tracks);
    }
    if (s->peer_connection) {
        rtcDeletePeerConnection(s->peer_connection);
        s->peer_connection = 0;
    }
}

static int whip_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_OPUS:
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
            return 1;
        default:
            return 0;
    }
}

#define OFFSET(x) offsetof(WHIPContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "bearer_token", "optional Bearer token for authentication and authorization", OFFSET(bearer_token), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { "max_stored_packets_count", "maximum number of stored packets for retransmission", OFFSET(max_stored_packets_count), AV_OPT_TYPE_INT, { .i64 = 100 }, 0, INT_MAX, ENC },
    { "connection_timeout", "timeout in seconds for establishing a connection", OFFSET(connection_timeout), AV_OPT_TYPE_DURATION, { .i64 = 10000000 }, 100000, INT_MAX, ENC },
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
    .p.flags            = AVFMT_GLOBALHEADER | AVFMT_NOFILE,
    .p.priv_class       = &whip_muxer_class,
    .priv_data_size     = sizeof(WHIPContext),
    .write_packet       = whip_write_packet,
    .write_header       = whip_write_header,
    .write_trailer      = whip_write_trailer,
    .init               = whip_init,
    .deinit             = whip_deinit,
    .query_codec        = whip_query_codec,
};
