/*
 * WebRTC-HTTP ingestion/egress protocol (WHIP/WHEP) common code
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
#include "avio_internal.h"
#include "libavcodec/codec_id.h"
#include "url.h"
#include "rtc/rtc.h"

#define RTP_MAX_PACKET_SIZE 1450

typedef struct DataChannelTrack {
    AVFormatContext *avctx;
    int track_id;
    AVFormatContext *rtp_ctx;
    URLContext *rtp_url_context;
} DataChannelTrack;

typedef struct DataChannelContext {
    AVFormatContext *avctx;
    int peer_connection;
    rtcState state;
    DataChannelTrack *tracks;
    int nb_tracks;
    const char *resource_location;

    /* options */
    char* bearer_token;
    int64_t connection_timeout;
    int64_t rw_timeout;
} DataChannelContext;

#define WEBRTC_OPTIONS(FLAGS, offset) \
    { "bearer_token", "optional Bearer token for authentication and authorization", offset+offsetof(DataChannelContext, bearer_token), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS }, \
    { "connection_timeout", "timeout for establishing a connection", offset+offsetof(DataChannelContext, connection_timeout), AV_OPT_TYPE_DURATION, { .i64 = 10000000 }, 100000, INT_MAX, FLAGS }, \
    { "rw_timeout", "timeout for receiving/writing data", offset+offsetof(DataChannelContext, rw_timeout), AV_OPT_TYPE_DURATION, { .i64 = 1000000 }, 100000, INT_MAX, FLAGS }

extern int webrtc_close_resource(DataChannelContext*const ctx);
extern int webrtc_convert_codec(enum AVCodecID codec_id, rtcCodec* rtc_codec);
extern int webrtc_create_resource(DataChannelContext*const ctx);
extern void webrtc_deinit(DataChannelContext*const ctx);
extern const char* webrtc_generate_media_stream_id(void);
extern int webrtc_init_connection(DataChannelContext*const ctx);
extern void webrtc_init_logger(void);
extern int webrtc_init_urlcontext(DataChannelContext*const ctx, int track_idx);
