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
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include "avformat.h"
#include "demux.h"
#include "libavcodec/codec_id.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"
#include "libavutil/random_seed.h"
#include "libavutil/fifo.h"
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
   {96, "VP8", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_VP8,  90000, -1},
   {97, "VP9", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_VP9,  90000, -1},
   {98, "H264", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 90000, -1},
   {99, "H265", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H265, 90000, -1},
   {111, "OPUS", AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_OPUS, 48000, 2},
   {-1, "", AVMEDIA_TYPE_UNKNOWN, AV_CODEC_ID_NONE, -1, -1}
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
     "a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f;sprop-parameter-sets=Z0IAH6kAUAW7+AiAAA==,aM4yyA==\n"  // ← 添加这里
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
    char *session_url;
    int64_t pli_period;
    int64_t last_pli_time;

    // libdatachannel state
    int pc;
    int audio_track;
    int video_track;

    RTPDemuxContext **rtp_ctxs;
    int rtp_ctxs_count;

    AVPacket *audio_pkt;
    AVPacket *video_pkt;

    // Packet FIFO for complete frames
    AVFifo *pkt_fifo;
    pthread_mutex_t pkt_fifo_lock;
    pthread_cond_t pkt_fifo_cond;

    // Flow control state
    int64_t last_remb_time;
    int64_t dropped_packets;
    int64_t total_packets;
    int flow_control_enabled;
    float flow_control_threshold;
    int max_bitrate;
    int min_bitrate;
    int waiting_for_keyframe;  // Flag to indicate we're dropping until next keyframe

    // Store AVFormatContext for callbacks
    AVFormatContext *avfmt_ctx;
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
 
 static RTPDemuxContext* whep_new_rtp_context(AVFormatContext *s, int payload_type)
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

     av_log(s, AV_LOG_DEBUG, "Allocate RTP context array: %p\n", rtp_ctxs);
     whep->rtp_ctxs = rtp_ctxs;
 
     st = avformat_new_stream(s, NULL);
     if (!st) {
         av_log(s, AV_LOG_ERROR, "Failed to allocate stream\n");
         goto fail;
     }

     av_log(s, AV_LOG_DEBUG, "Allocate stream: %p\n", st);
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
 
     rtp_ctx = ff_rtp_parse_open(s, st, payload_type, 256);
     if (!rtp_ctx) {
         av_log(s, AV_LOG_ERROR, "Failed to open RTP context\n");
         goto fail;
     }

     av_log(s, AV_LOG_DEBUG, "Open RTP context: %p\n", rtp_ctx);
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
                 av_log(s, AV_LOG_WARNING, "No SDP a-line for payload type %d\n", payload_type);
             } else {
                 handler->parse_sdp_a_line(s, st->index, dynamic_protocol_context, line);
             }
         }
     }

    // 确保 H.264/HEVC 视频流使用完整解析，这样多个 NAL 单元会被合并成一帧
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (st->codecpar->codec_id == AV_CODEC_ID_H264 || 
            st->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            ffstream(st)->need_parsing = AVSTREAM_PARSE_FULL;
            av_log(s, AV_LOG_INFO, "Enabled AVSTREAM_PARSE_FULL for %s to merge NAL units\n",
                   st->codecpar->codec_id == AV_CODEC_ID_H264 ? "H.264" : "HEVC");
        }
    }
 
     whep->rtp_ctxs[whep->rtp_ctxs_count++] = rtp_ctx;
     av_log(s, AV_LOG_DEBUG, "Add RTP context: %p\n", rtp_ctx);
     return rtp_ctx;
 
 fail:
     av_log(s, AV_LOG_DEBUG, "Fail to add RTP context: %p\n", rtp_ctx);
     if (rtp_ctx)
         ff_rtp_parse_close(rtp_ctx);
     av_free(dynamic_protocol_context);
    return NULL;
}

// Send RTCP REMB (Receiver Estimated Maximum Bitrate) feedback
static void send_remb_feedback(AVFormatContext *s, WHEPContext *whep, int track_id, uint32_t ssrc, uint32_t bitrate)
{
    // REMB format (RFC draft-alvestrand-rmcat-remb)
    // 0                   1                   2                   3
    // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |V=2|P| FMT=15  |   PT=206      |          length               |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                  SSRC of packet sender                        |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                  SSRC of media source                         |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |  Unique identifier 'R' 'E' 'M' 'B'                            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |  Num SSRC     | BR Exp    |  BR Mantissa                      |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |   SSRC feedback                                               |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    uint32_t sender_ssrc = ssrc + 1; // Use SSRC + 1 as sender
    
    // Calculate BR Exp and Mantissa (bitrate = mantissa * 2^exp)
    uint8_t exp = 0;
    uint32_t mantissa = bitrate;
    while (mantissa > 0x3FFFF && exp < 63) { // 18-bit mantissa max
        mantissa >>= 1;
        exp++;
    }
    
    uint8_t remb_packet[24] = {
        (RTP_VERSION << 6) | 15,  // V=2, P=0, FMT=15 (Application layer FB)
        206,                       // PT=206 (PSFB)
        0x00, 0x05,               // Length = 5 (6 * 4 bytes - 4)
        sender_ssrc >> 24, sender_ssrc >> 16, sender_ssrc >> 8, sender_ssrc,
        0, 0, 0, 0,               // Media source SSRC (0 for now)
        'R', 'E', 'M', 'B',       // Unique identifier
        1,                        // Num SSRC = 1
        (exp << 2) | ((mantissa >> 16) & 0x03),  // BR Exp (6 bits) + Mantissa high 2 bits
        (mantissa >> 8) & 0xFF,   // Mantissa middle 8 bits
        mantissa & 0xFF,          // Mantissa low 8 bits
        ssrc >> 24, ssrc >> 16, ssrc >> 8, ssrc  // SSRC feedback
    };
    
    if (rtcSendMessage(track_id, (const char *)remb_packet, sizeof(remb_packet)) < 0) {
        av_log(s, AV_LOG_WARNING, "Failed to send REMB feedback (bitrate: %u bps)\n", bitrate);
    } else {
        av_log(s, AV_LOG_DEBUG, "Sent REMB feedback: %u bps (exp=%u, mantissa=%u)\n", 
               bitrate, exp, mantissa);
    }
}

static void message_callback(int id, const char *message, int size, void *ptr)
{
    WHEPContext *whep = ptr;
    AVFormatContext *s = whep->avfmt_ctx;
    uint8_t *data = NULL;
    AVIOContext *dyn_bc = NULL;
    RTPDemuxContext *rtp_ctx = NULL;
    AVPacket *pkt = NULL;
    int ret = -1;

    if (size < 2)
        return;

    if ((RTP_PT_IS_RTCP(message[1]) && size < 8) || size < 12)
        return;

    // ignore audio
    if (id == whep->audio_track)
        return;

    // Handle RTCP packets
    if (RTP_PT_IS_RTCP(message[1])) {
        if (message[1] == RTCP_SR) {
            uint32_t ssrc = (message[4] << 24) | (message[5] << 16) | 
                           (message[6] << 8) | message[7];
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
                if (len > 0 && dyn_buf) {
                    if (rtcSendMessage(id, (const char *)dyn_buf, len) < 0)
                        av_log(s, AV_LOG_ERROR, "Failed to send RTCP RR\n");
                    av_free(dyn_buf);
                }
            }
        }
        return; // Skip other RTCP messages
    }

    // Handle RTP packets
    int payload_type = message[1] & 0x7f;
    
    // Find or create RTP context
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
            av_log(s, AV_LOG_DEBUG, "Create RTP context for payload type %d\n", payload_type);
            rtp_ctx = whep_new_rtp_context(s, payload_type);
        }
    }

    if (!rtp_ctx) {
        av_log(s, AV_LOG_WARNING, "Failed to get RTP context for payload type %d\n", payload_type);
        return;
    }

    // Send RTCP feedback
    if (avio_open_dyn_buf(&dyn_bc) == 0) {
        int len;
        uint8_t *dyn_buf = NULL;
        ff_rtp_send_rtcp_feedback(rtp_ctx, NULL, dyn_bc);
        len = avio_close_dyn_buf(dyn_bc, &dyn_buf);
        if (len > 0 && dyn_buf) {
            if (rtcSendMessage(id, (const char *)dyn_buf, len) < 0)
                av_log(s, AV_LOG_ERROR, "Failed to send RTCP feedback\n");
            av_free(dyn_buf);
        }
    }

    // Send PLI for video track
    if (id == whep->video_track && rtp_ctx->ssrc) {
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
            if (rtcSendMessage(id, (const char *)pli_packet, sizeof(pli_packet)) < 0)
                av_log(s, AV_LOG_ERROR, "Failed to send PLI\n");
            else
                whep->last_pli_time = now;
        }
    }

    // Parse RTP packet and put complete frames into pkt_fifo
    data = av_memdup(message, size);
    if (!data) {
        av_log(s, AV_LOG_ERROR, "Failed to duplicate message\n");
        return;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate packet\n");
        av_free(data);
        return;
    }

    // Parse the RTP packet
    ret = ff_rtp_parse_packet(rtp_ctx, pkt, &data, size);
    av_free(data); // Free the duplicated data after parsing

    // Loop to get all complete frames from the RTP parser
    while (ret >= 0) {
        // av_log(s, AV_LOG_DEBUG, "Parse RTP packet ret: %d, size: %d\n", ret, pkt->size);
        if (pkt->size > 0) {
            // We have a complete frame, save it to pkt_fifo
            AVPacket *pkt_copy = av_packet_alloc();
            if (pkt_copy && av_packet_ref(pkt_copy, pkt) == 0) {
                pthread_mutex_lock(&whep->pkt_fifo_lock);
                
                size_t fifo_size = av_fifo_can_read(whep->pkt_fifo);
                size_t fifo_capacity = av_fifo_can_write(whep->pkt_fifo) + fifo_size;
                float usage_ratio = (float)fifo_size / fifo_capacity;
                
                whep->total_packets++;
                
                // Flow control: Check FIFO usage
                if (av_fifo_can_write(whep->pkt_fifo) >= 1) {
                    av_fifo_write(whep->pkt_fifo, &pkt_copy, 1);
                    pthread_cond_signal(&whep->pkt_fifo_cond);
                    
                    // Adaptive flow control using REMB
                    if (whep->flow_control_enabled && 
                        usage_ratio > whep->flow_control_threshold && 
                        rtp_ctx->ssrc && id == whep->video_track) {
                        int64_t now = av_gettime_relative();
                        // Send REMB at most once per second
                        if (!whep->last_remb_time || (now - whep->last_remb_time) >= 1000000) {
                            // Calculate target bitrate based on usage ratio
                            // Linear mapping from threshold to 1.0 -> max_bitrate to min_bitrate
                            float pressure = (usage_ratio - whep->flow_control_threshold) / 
                                           (1.0f - whep->flow_control_threshold);
                            pressure = pressure > 1.0f ? 1.0f : pressure;
                            
                            uint32_t target_bitrate = whep->max_bitrate - 
                                (uint32_t)((whep->max_bitrate - whep->min_bitrate) * pressure);
                            
                            if (target_bitrate < whep->min_bitrate)
                                target_bitrate = whep->min_bitrate;
                            
                            send_remb_feedback(s, whep, id, rtp_ctx->ssrc, target_bitrate);
                            whep->last_remb_time = now;
                            
                            av_log(s, AV_LOG_DEBUG, 
                                   "Flow control: FIFO %.1f%% full, requesting %u bps\n",
                                   usage_ratio * 100.0f, target_bitrate);
                        }
                    }
                } else {
                    // FIFO is full - start dropping until next keyframe
                    if (!whep->waiting_for_keyframe) {
                        whep->waiting_for_keyframe = 1;
                        av_log(s, AV_LOG_WARNING, 
                               "FIFO full (%.1f%%), entering drop mode until next keyframe\n",
                               usage_ratio * 100.0f);
                        
                        // Send aggressive REMB when entering drop mode
                        if (whep->flow_control_enabled && rtp_ctx->ssrc && id == whep->video_track) {
                            send_remb_feedback(s, whep, id, rtp_ctx->ssrc, whep->min_bitrate);
                            whep->last_remb_time = av_gettime_relative();
                        }
                    }
                    
                    // Check if this is a keyframe
                    if (pkt_copy->flags & AV_PKT_FLAG_KEY) {
                        // This is a keyframe - try to make room for it
                        // Drop oldest packets until we have space
                        AVPacket *old_pkt = NULL;
                        while (av_fifo_can_write(whep->pkt_fifo) == 0) {
                            if (av_fifo_read(whep->pkt_fifo, &old_pkt, 1) >= 0) {
                                av_packet_free(&old_pkt);
                            } else {
                                break;
                            }
                        }
                        
                        // Now write the keyframe
                        if (av_fifo_write(whep->pkt_fifo, &pkt_copy, 1) >= 0) {
                            pthread_cond_signal(&whep->pkt_fifo_cond);
                            whep->waiting_for_keyframe = 0;  // Reset flag after keyframe
                            av_log(s, AV_LOG_INFO, 
                                   "Keyframe received, exiting drop mode (dropped %lld packets, %.2f%%)\n",
                                   (long long)whep->dropped_packets,
                                   (float)whep->dropped_packets / whep->total_packets * 100.0f);
                        } else {
                            av_log(s, AV_LOG_ERROR, "Failed to write keyframe to FIFO\n");
                            av_packet_free(&pkt_copy);
                        }
                    } else {
                        // Not a keyframe - drop it
                        whep->dropped_packets++;
                        av_log(s, AV_LOG_DEBUG, 
                               "Dropping non-keyframe packet (drop rate: %.2f%%, %lld/%lld)\n",
                               (float)whep->dropped_packets / whep->total_packets * 100.0f,
                               (long long)whep->dropped_packets, (long long)whep->total_packets);
                        av_packet_free(&pkt_copy);
                    }
                }
                pthread_mutex_unlock(&whep->pkt_fifo_lock);
            } else {
                av_packet_free(&pkt_copy);
            }
            av_packet_unref(pkt);
        }
        
        // Try to get next complete frame from parser's internal buffer
        ret = ff_rtp_parse_packet(rtp_ctx, pkt, NULL, 0);
    }

    av_packet_free(&pkt);
}
 
static int whep_read_header(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;
    rtcConfiguration config = {0};
    int ret;

    config.disableAutoNegotiation = true;
    ff_whip_whep_init_rtc_logger();
    s->ctx_flags |= AVFMTCTX_NOHEADER;

    // Store AVFormatContext pointer for callback
    whep->avfmt_ctx = s;

    // Initialize flow control state
    whep->waiting_for_keyframe = 0;
    whep->dropped_packets = 0;
    whep->total_packets = 0;
    whep->last_remb_time = 0;

    // Initialize packet FIFO (can store 100 AVPacket pointers)
    whep->pkt_fifo = av_fifo_alloc2(2048, sizeof(AVPacket *), 0);
    if (!whep->pkt_fifo) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate packet FIFO\n");
        return AVERROR(ENOMEM);
    }

    // Initialize mutex and condition variable
    if ((ret = pthread_mutex_init(&whep->pkt_fifo_lock, NULL)) != 0) {
        av_log(s, AV_LOG_ERROR, "Failed to initialize mutex: %s\n", av_err2str(AVERROR(ret)));
        av_fifo_freep2(&whep->pkt_fifo);
        return AVERROR(ret);
    }

    if ((ret = pthread_cond_init(&whep->pkt_fifo_cond, NULL)) != 0) {
        av_log(s, AV_LOG_ERROR, "Failed to initialize condition variable: %s\n", av_err2str(AVERROR(ret)));
        pthread_mutex_destroy(&whep->pkt_fifo_lock);
        av_fifo_freep2(&whep->pkt_fifo);
        return AVERROR(ret);
    }

    // Initialize WebRTC peer connection
    whep->pc = rtcCreatePeerConnection(&config);
    if (whep->pc <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to create peer connection\n");
        pthread_cond_destroy(&whep->pkt_fifo_cond);
        pthread_mutex_destroy(&whep->pkt_fifo_lock);
        av_fifo_freep2(&whep->pkt_fifo);
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

    return ff_whip_whep_exchange_and_set_sdp(s, whep->pc, whep->token, &whep->session_url);
}
 
static int whep_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    WHEPContext *whep = s->priv_data;
    AVPacket *pkt_from_fifo = NULL;
    struct timespec ts;
    int ret;

    // Check if connection is closed
    if (rtcIsClosed(whep->audio_track) && rtcIsClosed(whep->video_track)) {
        av_log(s, AV_LOG_ERROR, "Connection closed\n");
        return AVERROR_EOF;
    }

    // Try to read packet from FIFO
    pthread_mutex_lock(&whep->pkt_fifo_lock);
    
    // Wait for data to be available (with timeout to check connection status)
    while (av_fifo_can_read(whep->pkt_fifo) == 0) {
        // Check if connection is closed while waiting
        if (rtcIsClosed(whep->audio_track) && rtcIsClosed(whep->video_track)) {
            pthread_mutex_unlock(&whep->pkt_fifo_lock);
            av_log(s, AV_LOG_ERROR, "Connection closed while waiting for packets\n");
            return AVERROR_EOF;
        }
        
        // Set timeout to 100ms
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000; // 100ms
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        
        // Wait for signal with timeout
        ret = pthread_cond_timedwait(&whep->pkt_fifo_cond, &whep->pkt_fifo_lock, &ts);
        if (ret == ETIMEDOUT) {
            // Timeout - check connection status in next iteration
            av_log(s, AV_LOG_TRACE, "Timeout waiting for packet, checking connection status\n");
            continue;
        } else if (ret != 0) {
            pthread_mutex_unlock(&whep->pkt_fifo_lock);
            av_log(s, AV_LOG_ERROR, "Error waiting for packet: %d\n", ret);
            return AVERROR(ret);
        }
    }

    size_t available = av_fifo_can_read(whep->pkt_fifo);
    av_fifo_read(whep->pkt_fifo, &pkt_from_fifo, 1);
    size_t remaining = av_fifo_can_read(whep->pkt_fifo);
    pthread_mutex_unlock(&whep->pkt_fifo_lock);

    // Move packet content to output parameter
    av_packet_move_ref(pkt, pkt_from_fifo);
    av_packet_free(&pkt_from_fifo);
    
    // Only print video packets to avoid audio log spam
    if (pkt->stream_index >= 0 && pkt->stream_index < s->nb_streams) {
        AVStream *st = s->streams[pkt->stream_index];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            av_log(s, AV_LOG_DEBUG, "Read from FIFO: available=%zu, remaining=%zu\n", available, remaining);
            av_log(s, AV_LOG_DEBUG, "Read VIDEO packet stream: %d, pts: %lld, dts: %lld, size: %d\n", 
                   pkt->stream_index, pkt->pts, pkt->dts, pkt->size);
        }
    }
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

    // Clean up packet FIFO
    if (whep->pkt_fifo) {
        AVPacket *pkt;
        while (av_fifo_read(whep->pkt_fifo, &pkt, 1) >= 0) {
            av_packet_free(&pkt);
        }
        av_fifo_freep2(&whep->pkt_fifo);
    }

    // Destroy mutex and condition variable
    pthread_cond_destroy(&whep->pkt_fifo_cond);
    pthread_mutex_destroy(&whep->pkt_fifo_lock);

    if (whep->audio_pkt)
        av_packet_free(&whep->audio_pkt);
    if (whep->video_pkt)
        av_packet_free(&whep->video_pkt);

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
    { "pli_period", "set interval in seconds for sending periodic PLI (Picture Loss Indication) requests; 0 to disable",
        OFFSET(pli_period), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { "flow_control", "enable adaptive flow control using RTCP REMB feedback",
        OFFSET(flow_control_enabled), AV_OPT_TYPE_BOOL, {.i64 = 1 }, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { "flow_control_threshold", "FIFO usage ratio (0.0-1.0) to trigger flow control",
        OFFSET(flow_control_threshold), AV_OPT_TYPE_FLOAT, {.dbl = 0.75 }, 0.0, 1.0, AV_OPT_FLAG_DECODING_PARAM },
    { "max_bitrate", "maximum bitrate in bps for flow control (0=unlimited)",
        OFFSET(max_bitrate), AV_OPT_TYPE_INT, {.i64 = 5000000 }, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { "min_bitrate", "minimum bitrate in bps for flow control",
        OFFSET(min_bitrate), AV_OPT_TYPE_INT, {.i64 = 300000 }, 100000, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
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