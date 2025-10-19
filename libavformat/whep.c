/**
 * WHEP (WebRTC-HTTP Egress Protocol) demuxer
 * Copyright (c) 2025
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
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <rtc/rtc.h>
#include <stdatomic.h>
#include <limits.h>
#include <string.h>
#include <inttypes.h>

#include "avformat.h"
#include "demux.h"
#include "libavcodec/codec_id.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "rtpdec.h"
#include "whip_whep.h"

static const struct {
    int pt;
    const char enc_name[6];
    enum AVMediaType codec_type;
    enum AVCodecID codec_id;
    int clock_rate;
    int audio_channels;
} dynamic_payload_types[] = {
    {96,  "VP8",  AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_VP8,  90000, -1},
    {97,  "VP9",  AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_VP9,  90000, -1},
    {98,  "H264", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 90000, -1},
    {99,  "H265", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H265, 90000, -1},
    {111, "OPUS", AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_OPUS, 48000,  2},
    {-1,  "",     AVMEDIA_TYPE_UNKNOWN, AV_CODEC_ID_NONE, -1,   -1}
};

static const char *audio_mline =
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 9 0 8\n"
    "a=mid:0\n"
    "a=recvonly\n"
    "a=rtpmap:111 opus/48000/2\n"
    "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1\n"
    "a=rtpmap:9 G722/8000\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=rtpmap:8 PCMA/8000\n";

static const char *video_mline =
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99\n"
    "a=mid:1\n"
    "a=recvonly\n"
    "a=rtpmap:96 VP8/90000\n"
    "a=rtcp-fb:96 goog-remb\n"
    "a=rtcp-fb:96 nack\n"
    "a=rtcp-fb:96 nack pli\n"
    "a=rtpmap:97 VP9/90000\n"
    "a=rtcp-fb:97 goog-remb\n"
    "a=rtcp-fb:97 nack\n"
    "a=rtcp-fb:97 nack pli\n"
    "a=rtpmap:98 H264/90000\n"
    "a=rtcp-fb:98 goog-remb\n"
    "a=rtcp-fb:98 nack\n"
    "a=rtcp-fb:98 nack pli\n"
    "a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\n"
    "a=rtpmap:99 H265/90000\n"
    "a=rtcp-fb:99 goog-remb\n"
    "a=rtcp-fb:99 nack\n"
    "a=rtcp-fb:99 nack pli\n";

typedef struct Message {
    int track;
    uint8_t *data;
    int size;
} Message;

typedef struct WHEPContext {
    AVClass *class;
    char *token;
    char *server_type;
    char *session_url;
    int64_t pli_period;
    int64_t last_pli_time;
    int reorder_queue_size;

    // libdatachannel state
    int pc;
    int audio_track;
    int video_track;

    RTPDemuxContext **rtp_ctxs;
    int rtp_ctxs_count;

    // lock-free ring buffer for messages (rtp packets)
    Message **buffer;
    int capacity;
    atomic_int head;
    atomic_int tail;

    AVPacket *audio_pkt;
    AVPacket *video_pkt;

    // PTS smoothing and frame rate control
    int64_t video_pts_base;
    int64_t video_frame_count;
    int64_t expected_frame_duration;  // in RTP clock units (90kHz for video)
    int64_t last_video_rtp_ts;
    int smooth_pts;  // enable PTS smoothing
    
    int64_t audio_pts_base;
    int64_t audio_frame_count;
    int64_t last_audio_rtp_ts;
    
    // Real-time clock-based frame rate control
    int64_t start_time;           // av_gettime_relative() when first packet received
    int64_t last_output_time;     // last time we output a packet
    int64_t target_frame_interval; // microseconds between frames (1000000/fps)
    AVPacket *last_video_frame;   // for frame repeat on loss
    int enable_frame_repeat;      // repeat last frame on packet loss
    
    // Rate limiting for warnings to reduce log spam
    int64_t last_ts_jump_warning_time;    // last time we logged timestamp jump warning
    int64_t last_nal_warning_time;        // last time we logged NAL start code warning
    int ts_jump_warning_count;            // count of timestamp jumps since last warning
    int nal_warning_count;                // count of NAL warnings since last warning
} WHEPContext;

static int whep_get_sdp_a_line(int track, char *buffer, int size, int payload_type)
{
    char *line, *end;
    char fmtp_prefix[16];

    if (rtcGetTrackDescription(track, buffer, size) < 0)
        return AVERROR_EXTERNAL;
    line = buffer;
    end  = buffer + strlen(buffer);
    snprintf(fmtp_prefix, sizeof(fmtp_prefix), "a=fmtp:%d", payload_type);

    while (line < end) {
        char *next_line = strchr(line, '\n');
        if (next_line)
            *next_line = '\0';

        while (*line == ' ' || *line == '\t')
            line++;

        if (av_strstart(line, fmtp_prefix, NULL)) {
            av_strlcpy(buffer, line + 2, size);
            return 0;
        }

        if (next_line) {
            *next_line = '\n';
            line = next_line + 1;
        } else {
            break;
        }
    }

    buffer[0] = '\0';
    return AVERROR(ENOENT);
}

static RTPDemuxContext *whep_new_rtp_context(AVFormatContext *s, int payload_type)
{
    WHEPContext *whep = s->priv_data;
    RTPDemuxContext **rtp_ctxs = NULL;
    RTPDemuxContext *rtp_ctx = NULL;
    AVStream *st = NULL;
    const RTPDynamicProtocolHandler *handler = NULL;
    PayloadContext *dynamic_protocol_context = NULL;

    rtp_ctxs = av_realloc_array(whep->rtp_ctxs, whep->rtp_ctxs_count + 1,
                                sizeof(*whep->rtp_ctxs));
    if (!rtp_ctxs) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate RTP context array\n");
        goto fail;
    }
    whep->rtp_ctxs = rtp_ctxs;

    st = avformat_new_stream(s, NULL);
    if (!st) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate stream\n");
        goto fail;
    }
    if (ff_rtp_get_codec_info(st->codecpar, payload_type) < 0) {
        for (int i = 0; dynamic_payload_types[i].pt > 0; i++) {
            if (dynamic_payload_types[i].pt == payload_type) {
                st->codecpar->codec_id   = dynamic_payload_types[i].codec_id;
                st->codecpar->codec_type = dynamic_payload_types[i].codec_type;

                if (dynamic_payload_types[i].audio_channels > 0) {
                    av_channel_layout_uninit(&st->codecpar->ch_layout);
                    st->codecpar->ch_layout.order       = AV_CHANNEL_ORDER_UNSPEC;
                    st->codecpar->ch_layout.nb_channels = dynamic_payload_types[i].audio_channels;
                }
                if (dynamic_payload_types[i].clock_rate > 0)
                    st->codecpar->sample_rate = dynamic_payload_types[i].clock_rate;
                handler = ff_rtp_handler_find_by_name(dynamic_payload_types[i].enc_name,
                                                      dynamic_payload_types[i].codec_type);
                break;
            }
        }
    }
    if (st->codecpar->sample_rate > 0)
        st->time_base = (AVRational){1, st->codecpar->sample_rate};

    // 使用配置的 reorder queue 大小，默认 10 包（低延迟）
    int queue_size = whep->reorder_queue_size > 0 ? whep->reorder_queue_size : 10;
    rtp_ctx = ff_rtp_parse_open(s, st, payload_type, queue_size);
    if (!rtp_ctx) {
        av_log(s, AV_LOG_ERROR, "Failed to open RTP context\n");
        goto fail;
    }
    av_log(s, AV_LOG_INFO, "[WHEP] RTP jitter buffer 大小: %d 包\n", queue_size);
    if (handler) {
        ffstream(st)->need_parsing = handler->need_parsing;
        dynamic_protocol_context = av_mallocz(handler->priv_data_size);
        if (!dynamic_protocol_context) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate dynamic protocol context\n");
            goto fail;
        }
        if (handler->init && handler->init(s, st->index, dynamic_protocol_context) < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to initialize dynamic protocol context\n");
            goto fail;
        }
        ff_rtp_parse_set_dynamic_protocol(rtp_ctx, dynamic_protocol_context, handler);

        if (handler->parse_sdp_a_line) {
            char line[SDP_MAX_SIZE];
            int track_id = (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ?
                           whep->audio_track : whep->video_track;
            if (whep_get_sdp_a_line(track_id, line, sizeof(line), payload_type) < 0) {
                av_log(s, AV_LOG_INFO, "[WHEP] ⚠️ 未找到 payload %d 的 SDP a-line，将依赖实际数据解析（类似浏览器模式）\n", payload_type);
            } else {
                av_log(s, AV_LOG_INFO, "[WHEP] 解析 SDP a-line (payload %d): %s\n", payload_type, line);
                handler->parse_sdp_a_line(s, st->index, dynamic_protocol_context, line);
            }
        }
        
        // 浏览器模式：标记为需要完整解析，允许从实际数据中提取 codec 信息
        if (st->codecpar->codec_id == AV_CODEC_ID_H264 || st->codecpar->codec_id == AV_CODEC_ID_H265) {
            ffstream(st)->need_parsing = AVSTREAM_PARSE_FULL;
            av_log(s, AV_LOG_INFO, "[WHEP] 🌐 启用浏览器模式：视频流将从实际数据中解析 codec 信息\n");
        }
    }

    whep->rtp_ctxs[whep->rtp_ctxs_count++] = rtp_ctx;
    return rtp_ctx;

fail:
    if (rtp_ctx)
        ff_rtp_parse_close(rtp_ctx);
    av_free(dynamic_protocol_context);
    return NULL;
}

static void message_callback(int id, const char *message, int size, void *ptr)
{
    WHEPContext *whep = ptr;
    Message *msg;
    int current_head, next, current_tail;

    if (size < 2)
        return;

    if ((RTP_PT_IS_RTCP(message[1]) && size < 8) || size < 12)
        return;

    // 打印接收到的消息信息 (仅视频相关)
    // 跳过音频日志以减少噪音

    // Push packet to ring buffer
    msg = av_malloc(sizeof(Message));
    if (!msg) {
        av_log(whep, AV_LOG_ERROR, "Failed to allocate message\n");
        return;
    }
    msg->track = id;
    msg->data  = av_memdup(message, size);
    msg->size  = size;

    if (!msg->data) {
        av_log(whep, AV_LOG_ERROR, "Failed to duplicate message\n");
        av_free(msg);
        return;
    }
    current_tail = atomic_load_explicit(&whep->tail, memory_order_relaxed);
    next         = (current_tail + 1) % whep->capacity;
    current_head = atomic_load_explicit(&whep->head, memory_order_acquire);

    if (next == current_head) {
        av_log(whep, AV_LOG_ERROR, "Message buffer is full\n");
        av_free(msg->data);
        av_free(msg);
        return;
    }

    whep->buffer[current_tail] = msg;
    atomic_store_explicit(&whep->tail, next, memory_order_release);
}

static int whep_read_header(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;
    rtcConfiguration config = {0};

    ff_whip_whep_init_rtc_logger();
    
    // WHEP 流是异步创建的（接收到第一个 RTP 包时），需要设置 NOHEADER
    s->ctx_flags |= AVFMTCTX_NOHEADER;

    // 确保 PLI 相关字段初始化为 0
    whep->last_pli_time = 0;
    
    // 初始化 PTS 平滑相关字段
    whep->video_pts_base = AV_NOPTS_VALUE;
    whep->video_frame_count = 0;
    whep->expected_frame_duration = 3000;  // 默认 30fps: 90000/30 = 3000
    whep->last_video_rtp_ts = AV_NOPTS_VALUE;
    
    whep->audio_pts_base = AV_NOPTS_VALUE;
    whep->audio_frame_count = 0;
    whep->last_audio_rtp_ts = AV_NOPTS_VALUE;
    
    // 初始化实时时钟控制
    whep->start_time = 0;
    whep->last_output_time = 0;
    whep->target_frame_interval = 33333;  // 默认 30fps = 33.333ms
    whep->last_video_frame = NULL;
    whep->enable_frame_repeat = 1;  // 默认启用帧重复
    
    // 浏览器模式：如果用户没有手动设置，自动降低探测要求以快速启动
    // 检查是否为用户设置：probesize 默认 5MB，analyzeduration 默认 0 或 5000000
    int probesize_default = (s->probesize <= 5000000);  // <= 5MB 认为是默认或用户想要快速启动
    int analyze_default = (s->max_analyze_duration == 0 || s->max_analyze_duration == 5000000);
    
    if (probesize_default && s->probesize > 500000) {
        s->probesize = 500000;  // 减少到 500KB
        av_log(s, AV_LOG_INFO, "[WHEP] 🌐 浏览器模式：降低 probesize 到 %d (原值: %d)\n", 
               s->probesize, 5000000);
    }
    if (analyze_default && s->max_analyze_duration != 1000000) {
        int64_t old_value = s->max_analyze_duration;
        s->max_analyze_duration = 1000000;  // 减少到 1 秒
        av_log(s, AV_LOG_INFO, "[WHEP] 🌐 浏览器模式：降低 analyzeduration 到 %lld (原值: %lld)\n", 
               s->max_analyze_duration, old_value);
    }

    whep->capacity = 1024;
    whep->buffer = av_calloc(whep->capacity, sizeof(*whep->buffer));
    if (!whep->buffer) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate message buffer\n");
        return AVERROR(ENOMEM);
    }
    // Initialize WebRTC peer connection
    whep->pc = rtcCreatePeerConnection(&config);
    if (whep->pc <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to create peer connection\n");
        return AVERROR_EXTERNAL;
    }
    rtcSetUserPointer(whep->pc, whep);

    // Add audio and video track
    whep->audio_track = rtcAddTrack(whep->pc, audio_mline);
    if (whep->audio_track <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to add audio track\n");
        return AVERROR_EXTERNAL;
    }

    if (rtcSetMessageCallback(whep->audio_track, message_callback) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set audio track message callback\n");
        return AVERROR_EXTERNAL;
    }

    whep->video_track = rtcAddTrack(whep->pc, video_mline);
    if (whep->video_track <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to add video track\n");
        return AVERROR_EXTERNAL;
    }

    if (rtcSetMessageCallback(whep->video_track, message_callback) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set video track message callback\n");
        return AVERROR_EXTERNAL;
    }

    return ff_whip_whep_exchange_and_set_sdp(s, whep->pc, whep->token, &whep->session_url, whep->server_type);
}

static int whep_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    WHEPContext *whep = s->priv_data;
    int current_head, current_tail, ret = 0;
    Message *msg = NULL;
    RTPDemuxContext *rtp_ctx = NULL;
    AVIOContext *dyn_bc = NULL;

    if (!whep->audio_pkt)
        whep->audio_pkt = av_packet_alloc();
    if (!whep->video_pkt)
        whep->video_pkt = av_packet_alloc();

redo:
    rtp_ctx = NULL;
    if (msg) {
        av_free(msg->data);
        av_free(msg);
    }
    if (rtcIsClosed(whep->audio_track) || rtcIsClosed(whep->video_track)) {
        av_log(s, AV_LOG_ERROR, "Connection closed\n");
        return AVERROR_EOF;
    }

    current_head = atomic_load_explicit(&whep->head, memory_order_relaxed);
    current_tail = atomic_load_explicit(&whep->tail, memory_order_acquire);

    if (current_head == current_tail)
        return AVERROR(EAGAIN);
    msg = whep->buffer[current_head];
    atomic_store_explicit(&whep->head, (current_head + 1) % whep->capacity,
                          memory_order_release);

    if (RTP_PT_IS_RTCP(msg->data[1])) {
        switch (msg->data[1]) {
        case RTCP_SR: {
            uint32_t ssrc = (msg->data[4] << 24) | (msg->data[5] << 16) |
                            (msg->data[6] << 8)  |  msg->data[7];
            for (int i = 0; i < whep->rtp_ctxs_count; i++) {
                if (whep->rtp_ctxs[i]->ssrc == ssrc) {
                    rtp_ctx = whep->rtp_ctxs[i];
                    break;
                }
            }
            // Send RTCP RR
            if (rtp_ctx && avio_open_dyn_buf(&dyn_bc) == 0) {
                int len;
                uint8_t *dyn_buf = NULL;
                ff_rtp_check_and_send_back_rr(rtp_ctx, NULL, dyn_bc, 300000);
                len = avio_close_dyn_buf(dyn_bc, &dyn_buf);
                if (len > 0 && dyn_buf && rtcSendMessage(msg->track, dyn_buf, len) < 0)
                    av_log(s, AV_LOG_ERROR, "Failed to send RTCP RR\n");
                av_free(dyn_buf);
            }
            break;
        }
        default:
            goto redo;
        }
    } else {
        int payload_type = msg->data[1] & 0x7f;
        for (int i = 0; i < whep->rtp_ctxs_count; i++) {
            if (whep->rtp_ctxs[i]->payload_type == payload_type) {
                rtp_ctx = whep->rtp_ctxs[i];
                break;
            }
        }

        if (!rtp_ctx) {
            AVCodecParameters par;
            ret = ff_rtp_get_codec_info(&par, payload_type);
            if (ret < 0) {
                for (int i = 0; dynamic_payload_types[i].pt > 0; i++) {
                    if (dynamic_payload_types[i].pt == payload_type) {
                        ret = 0;
                        break;
                    }
                }
            }
            if (ret == 0) {
                rtp_ctx = whep_new_rtp_context(s, payload_type);
                if (rtp_ctx && rtp_ctx->st) {
                    AVCodecParameters *par = rtp_ctx->st->codecpar;
                    // 只打印视频相关信息
                    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                        av_log(s, AV_LOG_INFO, "[WHEP] 创建 RTP context for payload type %d\n", payload_type);
                        av_log(s, AV_LOG_INFO, "[WHEP] RTP context 创建成功: codec=%s, extradata_size=%d\n",
                               avcodec_get_name(par->codec_id), par->extradata_size);
                        if (par->extradata_size > 0 && par->extradata) {
                            av_log(s, AV_LOG_INFO, "[WHEP] extradata 前16字节:");
                            for (int i = 0; i < FFMIN(16, par->extradata_size); i++)
                                av_log(s, AV_LOG_INFO, " %02x", par->extradata[i]);
                            av_log(s, AV_LOG_INFO, "\n");
                        }
                    }
                }
            }
        }
    }

    if (!rtp_ctx) {
        av_log(s, AV_LOG_WARNING, "Failed to get RTP context for message %d\n", msg->data[1]);
        goto redo;
    }

    // Parse RTP packet
    int parse_result;
    if (msg->track == whep->audio_track) {
        parse_result = ff_rtp_parse_packet(rtp_ctx, whep->audio_pkt, (uint8_t **)&msg->data, msg->size);
    } else if (msg->track == whep->video_track) {
        parse_result = ff_rtp_parse_packet(rtp_ctx, whep->video_pkt, (uint8_t **)&msg->data, msg->size);
    } else {
        parse_result = -1;
    }
    
    // parse_result < 0 表示 jitter buffer 正在等待更多包来组装完整帧
    // 此时不应该返回 EAGAIN，而是应该继续处理队列中的其他包
    if (parse_result < 0) {
        // Jitter buffer 正在组装包，继续处理下一个
        goto redo;
    }
    
    // parse_result >= 0 表示包已输出
    ret = 0;

    // 首次收到视频包时，立即发送 PLI 请求关键帧（带 SPS/PPS）
    if (msg->track == whep->video_track) {
        if (rtp_ctx->ssrc && whep->last_pli_time == 0) {
            uint32_t source_ssrc = rtp_ctx->ssrc;
            uint32_t sender_ssrc = source_ssrc + 1;
            uint8_t pli_packet[] = {
                (RTP_VERSION << 6) | 1, RTCP_PSFB,         0x00,             0x02,
                sender_ssrc >> 24,      sender_ssrc >> 16, sender_ssrc >> 8, sender_ssrc,
                source_ssrc >> 24,      source_ssrc >> 16, source_ssrc >> 8, source_ssrc,
            };
            if (rtcSendMessage(msg->track, pli_packet, sizeof(pli_packet)) < 0)
                av_log(s, AV_LOG_ERROR, "[WHEP] 首次发送 PLI 失败\n");
            else {
                av_log(s, AV_LOG_INFO, "[WHEP] ✅ 首次发送 PLI 请求关键帧 (SSRC=0x%08x)\n", source_ssrc);
                whep->last_pli_time = av_gettime_relative();
            }
        } else {
            av_log(s, AV_LOG_DEBUG, "[WHEP] PLI 条件不满足: ssrc=0x%08x, last_pli_time=%lld\n", 
                   rtp_ctx->ssrc, whep->last_pli_time);
        }
    }

    // Send RTCP feedback
    if (avio_open_dyn_buf(&dyn_bc) == 0) {
        int len;
        uint8_t *dyn_buf = NULL;
        ff_rtp_send_rtcp_feedback(rtp_ctx, NULL, dyn_bc);
        len = avio_close_dyn_buf(dyn_bc, &dyn_buf);
        if (len > 0 && dyn_buf && rtcSendMessage(msg->track, dyn_buf, len) < 0)
            av_log(s, AV_LOG_ERROR, "Failed to send RTCP feedback\n");
        av_free(dyn_buf);
    }

    // Send PLI
    if (msg->track == whep->video_track && rtp_ctx->ssrc) {
        int64_t now = av_gettime_relative();
        if ((whep->pli_period && now - whep->last_pli_time >= whep->pli_period * 1000000) ||
            (rtp_ctx->handler && rtp_ctx->handler->need_keyframe &&
             rtp_ctx->handler->need_keyframe(rtp_ctx->dynamic_protocol_context))) {
            uint32_t source_ssrc = rtp_ctx->ssrc;
            uint32_t sender_ssrc = source_ssrc + 1;
            uint8_t pli_packet[] = {
                (RTP_VERSION << 6) | 1, RTCP_PSFB,         0x00,             0x02,
                sender_ssrc >> 24,      sender_ssrc >> 16, sender_ssrc >> 8, sender_ssrc,
                source_ssrc >> 24,      source_ssrc >> 16, source_ssrc >> 8, source_ssrc,
            };
            if (rtcSendMessage(msg->track, pli_packet, sizeof(pli_packet)) < 0)
                av_log(s, AV_LOG_ERROR, "Failed to send PLI\n");
            else
                whep->last_pli_time = now;
        }
    }

    if (ret != 0)
        goto redo;

    if (msg->track == whep->audio_track) {
        // 浏览器模式：即使没有完整信息也尝试输出音频包
        if (whep->audio_pkt && whep->audio_pkt->size > 0) {
        av_packet_ref(pkt, whep->audio_pkt);
            
            // 音频 PTS 平滑（如果启用）
            if (whep->smooth_pts && rtp_ctx) {
                int64_t original_pts = pkt->pts;
                
                if (whep->audio_pts_base == AV_NOPTS_VALUE) {
                    // 首个音频包，建立基准
                    whep->audio_pts_base = pkt->pts;
                    whep->last_audio_rtp_ts = pkt->pts;
                    whep->audio_frame_count = 0;
                } else {
                    // 计算预期 PTS（Opus: 48kHz 时钟，20ms = 960 samples）
                    int64_t expected_pts = whep->audio_pts_base + whep->audio_frame_count * 960;
                    int64_t pts_diff = llabs(pkt->pts - expected_pts);
                    
                    // 如果偏差小于 5 帧（100ms），使用平滑后的 PTS
                    if (pts_diff < 960 * 5) {
                        pkt->pts = expected_pts;
                        pkt->dts = expected_pts;
                    } else {
                        // 偏差过大，重新同步（不打印日志）
                        whep->audio_pts_base = pkt->pts;
                        whep->audio_frame_count = 0;
                    }
                }
                
                whep->audio_frame_count++;
                whep->last_audio_rtp_ts = original_pts;
            }
            
            // 不打印音频包输出日志
        av_packet_free(&whep->audio_pkt);
        } else {
            goto redo;
        }
    } else if (msg->track == whep->video_track) {
        // 实时时钟节流：按固定帧率输出，避免网络抖动影响
        int64_t now = av_gettime_relative();
        
        if (whep->start_time == 0) {
            // 首次接收，建立时间基准
            whep->start_time = now;
            whep->last_output_time = now;
        } else {
            // 计算距离上次输出的时间
            int64_t elapsed = now - whep->last_output_time;
            
            // 如果还没到输出时间，等待
            if (elapsed < whep->target_frame_interval) {
                int64_t wait_time = whep->target_frame_interval - elapsed;
                av_log(s, AV_LOG_DEBUG, "[WHEP] ⏰ 等待 %.1fms 以维持帧率\n", wait_time / 1000.0);
                av_usleep(wait_time);
                now = av_gettime_relative();
            }
        }
        
        // 直接输出视频包，不做格式转换（推流端已保证格式正确）
        if (whep->video_pkt && whep->video_pkt->size > 0) {
            av_packet_ref(pkt, whep->video_pkt);
            
            // 视频 PTS 平滑（如果启用）
            if (whep->smooth_pts && rtp_ctx) {
                int64_t original_pts = pkt->pts;
                
                if (whep->video_pts_base == AV_NOPTS_VALUE) {
                    // 首个视频包，建立基准
                    whep->video_pts_base = pkt->pts;
                    whep->last_video_rtp_ts = pkt->pts;
                    whep->video_frame_count = 0;
                    
                    av_log(s, AV_LOG_INFO, "[WHEP] 📹 建立视频时间基准: base_pts=%"PRId64", frame_duration=%"PRId64" (%.2f fps)\n",
                           whep->video_pts_base, whep->expected_frame_duration, 
                           90000.0 / whep->expected_frame_duration);
                } else {
                    // 计算预期 PTS（基于固定帧率）
                    int64_t expected_pts = whep->video_pts_base + 
                                           whep->video_frame_count * whep->expected_frame_duration;
                    int64_t pts_diff = llabs(pkt->pts - expected_pts);
                    
                    // 如果偏差小于 3 帧，使用平滑后的 PTS
                    if (pts_diff < whep->expected_frame_duration * 3) {
                        pkt->pts = expected_pts;
                        pkt->dts = expected_pts;
                        
                        if (pts_diff > whep->expected_frame_duration / 2) {
                            av_log(s, AV_LOG_DEBUG, "[WHEP] 📹 视频时间戳校正: 原始=%"PRId64", 预期=%"PRId64", 差值=%.1fms\n",
                                   original_pts, expected_pts, pts_diff * 1000.0 / 90000);
                        }
                    } else {
                        // 偏差过大（可能是关键帧或网络问题），重新同步
                        // Rate limit this warning to once per second
                        int64_t now_micro = av_gettime_relative();
                        whep->ts_jump_warning_count++;
                        if (now_micro - whep->last_ts_jump_warning_time >= 1000000) {
                            if (whep->ts_jump_warning_count > 1) {
                                av_log(s, AV_LOG_WARNING, "[WHEP] ⚠️ 视频时间戳跳跃 (过去1秒内发生 %d 次): 最后差值=%.1fms (%"PRId64" 帧), %s\n",
                                       whep->ts_jump_warning_count, pts_diff * 1000.0 / 90000,
                                       pts_diff / whep->expected_frame_duration,
                                       (pkt->flags & AV_PKT_FLAG_KEY) ? "关键帧-重新同步" : "丢包-重新同步");
                            } else {
                                av_log(s, AV_LOG_WARNING, "[WHEP] ⚠️ 视频时间戳跳跃: 预期=%"PRId64", 实际=%"PRId64", 差值=%.1fms (%"PRId64" 帧), %s\n",
                                       expected_pts, original_pts, pts_diff * 1000.0 / 90000,
                                       pts_diff / whep->expected_frame_duration,
                                       (pkt->flags & AV_PKT_FLAG_KEY) ? "关键帧-重新同步" : "丢包-重新同步");
                            }
                            whep->last_ts_jump_warning_time = now_micro;
                            whep->ts_jump_warning_count = 0;
                        }
                        whep->video_pts_base = pkt->pts;
                        whep->video_frame_count = 0;
                    }
                }
                
                whep->video_frame_count++;
                whep->last_video_rtp_ts = original_pts;
            }
            
            // 更新输出时间
            whep->last_output_time = now;
            
            // 备份当前帧用于可能的重复（未来实现）
            if (whep->enable_frame_repeat) {
                if (!whep->last_video_frame) {
                    whep->last_video_frame = av_packet_alloc();
                }
                if (whep->last_video_frame) {
                    av_packet_unref(whep->last_video_frame);
                    av_packet_ref(whep->last_video_frame, pkt);
                }
            }
            
            av_log(s, AV_LOG_INFO, "[WHEP] 🎥 输出视频包: stream_index=%d, pts=%"PRId64", dts=%"PRId64", 大小=%d 字节%s\n",
                   pkt->stream_index, pkt->pts, pkt->dts, pkt->size,
                   (pkt->flags & AV_PKT_FLAG_KEY) ? " [🔑关键帧]" : "");
            av_packet_free(&whep->video_pkt);
        } else {
            av_log(s, AV_LOG_DEBUG, "[WHEP] 视频包为空或无效，跳过\n");
            goto redo;
        }
    }
    av_free(msg->data);
    av_free(msg);
    return 0;
}

static int whep_read_close(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;

    if (whep->audio_track > 0) {
        rtcDeleteTrack(whep->audio_track);
        whep->audio_track = 0;
    }
    if (whep->video_track > 0) {
        rtcDeleteTrack(whep->video_track);
        whep->video_track = 0;
    }
    if (whep->pc > 0) {
        rtcDeletePeerConnection(whep->pc);
        whep->pc = 0;
    }

    if (whep->rtp_ctxs) {
        for (int i = 0; i < whep->rtp_ctxs_count; i++) {
            if (whep->rtp_ctxs[i]) {
                PayloadContext *payload_ctx = whep->rtp_ctxs[i]->dynamic_protocol_context;
                ff_rtp_parse_close(whep->rtp_ctxs[i]);
                av_freep(&payload_ctx);
            }
        }
        av_freep(&whep->rtp_ctxs);
        whep->rtp_ctxs_count = 0;
    }

    if (whep->buffer) {
        int head = atomic_load(&whep->head);
        int tail = atomic_load(&whep->tail);

        while (head != tail) {
            Message *msg = whep->buffer[head];
            if (msg) {
                av_freep(&msg->data);
                av_freep(&msg);
            }
            head = (head + 1) % whep->capacity;
        }
        av_freep(&whep->buffer);
    }

    if (whep->audio_pkt)
        av_packet_free(&whep->audio_pkt);
    if (whep->video_pkt)
        av_packet_free(&whep->video_pkt);
    if (whep->last_video_frame)
        av_packet_free(&whep->last_video_frame);

    if (whep->session_url) {
        ff_whip_whep_delete_session(s, whep->token, whep->session_url);
        av_freep(&whep->session_url);
    }

    return 0;
}

#define OFFSET(x) offsetof(WHEPContext, x)
static const AVOption whep_options[] = {
    { "token", "set token to send in the Authorization header as \"Bearer <token>\"",
        OFFSET(token), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { "server_type", "set server type (standard or srs)",
        OFFSET(server_type), AV_OPT_TYPE_STRING, { .str = "standard" }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { "pli_period", "set interval in seconds for sending periodic PLI (Picture Loss Indication) requests; 0 to disable",
        OFFSET(pli_period), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { "reorder_queue_size", "set RTP packet reorder queue size for jitter buffer (default: 10 for low latency, 0 for auto)",
        OFFSET(reorder_queue_size), AV_OPT_TYPE_INT, { .i64 = 10 }, 0, 500, AV_OPT_FLAG_DECODING_PARAM },
    { "smooth_pts", "enable PTS smoothing for stable frame rate (0=disable, 1=enable, default: 1)",
        OFFSET(smooth_pts), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { "frame_repeat", "enable frame repeat on packet loss to avoid stuttering (0=disable, 1=enable, default: 1)",
        OFFSET(enable_frame_repeat), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { "target_fps", "target frame rate for output throttling (default: 30)",
        OFFSET(target_frame_interval), AV_OPT_TYPE_INT, { .i64 = 33333 }, 10000, 100000, AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};

static const AVClass whep_class = {
    .class_name = "WHEP demuxer",
    .item_name  = av_default_item_name,
    .option     = whep_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_whep_demuxer = {
    .p.name         = "whep",
    .p.long_name    = NULL_IF_CONFIG_SMALL("WHEP (WebRTC-HTTP Egress Protocol)"),
    .p.flags        = AVFMT_NOFILE,
    .p.priv_class   = &whep_class,
    .priv_data_size = sizeof(WHEPContext),
    .read_header    = whep_read_header,
    .read_packet    = whep_read_packet,
    .read_close     = whep_read_close,
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
};
