/**
 * WHEP_2 (WebRTC-HTTP Egress Protocol 2) demuxer
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

// RTP stream state for timestamp synchronization
typedef struct RTPStreamContext {
    RTPDemuxContext *rtp_ctx;   // FFmpeg RTP demuxer context (handles depacketization)
    int stream_index;            // AVStream index
    int64_t last_rtcp_time;      // Last RTCP RR send time
} RTPStreamContext;

typedef struct WHEP2Context {
    AVClass *class;
    char *token;
    char *session_url;

    // libdatachannel state
    int pc;
    int audio_track;
    int video_track;

    // Parsed packet FIFO (output from RTP parser)
    AVFifo *pkt_fifo;
    pthread_mutex_t pkt_fifo_lock;
    pthread_cond_t pkt_fifo_cond;

    // Store AVFormatContext for callbacks
    AVFormatContext *avfmt_ctx;
    
    // Stream mapping: track ID -> RTPStreamContext
    RTPStreamContext audio_stream;
    RTPStreamContext video_stream;
    
    // RTP contexts array (for cleanup)
    RTPDemuxContext **rtp_ctxs;
    int rtp_ctxs_count;
} WHEP2Context;

static void message_callback(int id, const char *message, int size, void *ptr)
{
    WHEP2Context *whep2 = ptr;
    AVFormatContext *s = whep2->avfmt_ctx;
    Message *msg = NULL;
    int is_rtp_rtcp = 0;

    if (size < 2)
        return;

    // Check if this is RTP/RTCP packet (audio/video data)
    // RTP version should be 2 (0x80 in first byte), and valid payload type
    if ((message[0] & 0xC0) == 0x80) {  // RTP version 2
        uint8_t payload_type = message[1] & 0x7F;
        
        // Check if it's RTCP (payload type 200-204) or RTP
        if (RTP_PT_IS_RTCP(message[1])) {
            if (size >= 8) {
                is_rtp_rtcp = 1;
                av_log(s, AV_LOG_TRACE, "Received RTCP packet, track=%d, size=%d\n", id, size);
            } else {
                av_log(s, AV_LOG_WARNING, "Invalid RTCP packet size: %d\n", size);
                return;
            }
        } else if (size >= 12) {  // Minimum RTP packet size
            is_rtp_rtcp = 1;
            av_log(s, AV_LOG_TRACE, "Received RTP packet, track=%d, PT=%d, size=%d\n", 
                   id, payload_type, size);
        }
    }

    // Handle non-RTP/RTCP signaling packets (DataChannel, SCTP, etc.)
    if (!is_rtp_rtcp) {
        av_log(s, AV_LOG_DEBUG, "Received non-RTP/RTCP signaling packet, track=%d, size=%d\n", id, size);
        
        // Log first few bytes for debugging
        if (size >= 4) {
            av_log(s, AV_LOG_DEBUG, "  First 4 bytes: %02X %02X %02X %02X\n",
                   (uint8_t)message[0], (uint8_t)message[1], 
                   (uint8_t)message[2], (uint8_t)message[3]);
        }
        
        // Check for common signaling message types
        if (size >= 1) {
            // SCTP message (typically starts with specific patterns)
            if ((uint8_t)message[0] == 0x00 || (uint8_t)message[0] == 0x01) {
                av_log(s, AV_LOG_DEBUG, "  Possible SCTP control message\n");
            }
            // DataChannel protocol message
            else if ((uint8_t)message[0] == 0x03) {
                av_log(s, AV_LOG_DEBUG, "  Possible DataChannel OPEN message\n");
            }
            else if ((uint8_t)message[0] == 0x02) {
                av_log(s, AV_LOG_DEBUG, "  Possible DataChannel ACK message\n");
            }
            // String message (printable ASCII or JSON)
            else if (message[0] >= 0x20 && message[0] <= 0x7E) {
                char preview[65];
                int preview_len = size < 64 ? size : 64;
                memcpy(preview, message, preview_len);
                preview[preview_len] = '\0';
                av_log(s, AV_LOG_INFO, "  Text message: %s%s\n", 
                       preview, size > 64 ? "..." : "");
            }
        }
        
        // For now, we don't queue non-RTP/RTCP packets to the FIFO
        // as they are typically not media data
        return;
    }

    // Process RTP/RTCP packets (audio/video)
    // Allocate message structure
    msg = av_mallocz(sizeof(Message));
    if (!msg) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate message\n");
        return;
    }

    msg->track = id;
    msg->size = size;
    msg->data = av_memdup(message, size);
    if (!msg->data) {
        av_log(s, AV_LOG_ERROR, "Failed to duplicate message data\n");
        av_free(msg);
        return;
    }

    // Put message into FIFO
    pthread_mutex_lock(&whep2->rtp_fifo_lock);
    if (av_fifo_can_write(whep2->rtp_fifo) >= 1) {
        av_fifo_write(whep2->rtp_fifo, &msg, 1);
        pthread_cond_signal(&whep2->rtp_fifo_cond);
    } else {
        av_log(s, AV_LOG_WARNING, "RTP FIFO full, dropping packet\n");
        av_free(msg->data);
        av_free(msg);
    }
    pthread_mutex_unlock(&whep2->rtp_fifo_lock);
}

static void state_change_callback(int pc, rtcState state, void *ptr)
{
    WHEP2Context *whep2 = ptr;
    AVFormatContext *s = whep2->avfmt_ctx;
    const char *state_str[] = {"NEW", "CONNECTING", "CONNECTED", "DISCONNECTED", "FAILED", "CLOSED"};
    
    if (state >= 0 && state < 6) {
        av_log(s, AV_LOG_INFO, "Peer connection state changed to: %s\n", state_str[state]);
    }
    
    if (state == RTC_FAILED || state == RTC_DISCONNECTED) {
        av_log(s, AV_LOG_WARNING, "Connection state is: %s\n", state_str[state]);
    }
}

static void gathering_state_callback(int pc, rtcGatheringState state, void *ptr)
{
    WHEP2Context *whep2 = ptr;
    AVFormatContext *s = whep2->avfmt_ctx;
    const char *state_str[] = {"NEW", "IN_PROGRESS", "COMPLETE"};
    
    if (state >= 0 && state < 3) {
        av_log(s, AV_LOG_DEBUG, "ICE gathering state: %s\n", state_str[state]);
    }
    
    if (state == RTC_GATHERING_COMPLETE) {
        av_log(s, AV_LOG_INFO, "ICE gathering completed, connection should be ready\n");
    }
}

// ICE candidate callback - handles remote ICE candidates from SRS
static void local_candidate_callback(int pc, const char *candidate, const char *mid, void *ptr)
{
    WHEP2Context *whep2 = ptr;
    AVFormatContext *s = whep2->avfmt_ctx;
    
    av_log(s, AV_LOG_DEBUG, "Local ICE candidate: %s (mid: %s)\n", 
           candidate ? candidate : "null", mid ? mid : "null");
}

// Data channel callback - for SRS signaling messages
static void datachannel_callback(int pc, int dc, void *ptr)
{
    WHEP2Context *whep2 = ptr;
    AVFormatContext *s = whep2->avfmt_ctx;
    
    char label[256] = {0};
    if (rtcGetDataChannelLabel(dc, label, sizeof(label)) >= 0) {
        av_log(s, AV_LOG_INFO, "Data channel opened: %s (id: %d)\n", label, dc);
    } else {
        av_log(s, AV_LOG_INFO, "Data channel opened (id: %d)\n", dc);
    }
}

// Open callback for tracks - called when track is ready
static void open_callback(int id, void *ptr)
{
    WHEP2Context *whep2 = ptr;
    AVFormatContext *s = whep2->avfmt_ctx;
    
    av_log(s, AV_LOG_INFO, "Track opened and ready to receive data (track id: %d)\n", id);
}

// Error callback for detailed error reporting
static void error_callback(int pc, const char *error, void *ptr)
{
    WHEP2Context *whep2 = ptr;
    AVFormatContext *s = whep2->avfmt_ctx;
    
    av_log(s, AV_LOG_ERROR, "WebRTC error: %s\n", error ? error : "unknown error");
}

// Convert RTP timestamp to PTS
static int64_t rtp_timestamp_to_pts(RTPStream *rtp_stream, uint32_t rtp_ts, int64_t arrival_time)
{
    int64_t pts;
    
    // Initialize base timestamp on first packet
    if (rtp_stream->base_pts == AV_NOPTS_VALUE) {
        rtp_stream->base_rtp_ts = rtp_ts;
        rtp_stream->base_pts = arrival_time;
        return arrival_time;
    }
    
    // Calculate PTS based on RTP timestamp difference
    // PTS = base_pts + (rtp_ts - base_rtp_ts) * AV_TIME_BASE / clock_rate
    int64_t rtp_delta = (int64_t)((uint32_t)(rtp_ts - rtp_stream->base_rtp_ts));
    pts = rtp_stream->base_pts + av_rescale_q(rtp_delta, 
                                               (AVRational){1, rtp_stream->clock_rate},
                                               (AVRational){1, AV_TIME_BASE});
    
    return pts;
}

// Send RTCP Receiver Report
static void send_rtcp_rr(WHEP2Context *whep2, int track_id, RTPStream *rtp_stream)
{
    AVFormatContext *s = whep2->avfmt_ctx;
    uint8_t rtcp_buf[32];  // RTCP RR packet
    int rtcp_len = 0;
    int64_t now = av_gettime_relative();
    
    // Only send RTCP every 5 seconds
    if (rtp_stream->last_rtcp_time > 0 && 
        (now - rtp_stream->last_rtcp_time) < 5000000) {
        return;
    }
    
    // Build RTCP RR packet (simplified version)
    // V=2, P=0, RC=1, PT=201 (RR)
    rtcp_buf[0] = 0x81;  // V=2, P=0, RC=1
    rtcp_buf[1] = 201;   // PT=201 (Receiver Report)
    rtcp_buf[2] = 0x00;  // Length (high byte)
    rtcp_buf[3] = 0x07;  // Length (low byte) - 7 32-bit words
    
    // SSRC of packet sender (our SSRC, can be random)
    uint32_t our_ssrc = 0x12345678;  // Should be initialized properly
    rtcp_buf[4] = (our_ssrc >> 24) & 0xFF;
    rtcp_buf[5] = (our_ssrc >> 16) & 0xFF;
    rtcp_buf[6] = (our_ssrc >> 8) & 0xFF;
    rtcp_buf[7] = our_ssrc & 0xFF;
    
    // Report block
    // SSRC of source being reported
    rtcp_buf[8] = (rtp_stream->ssrc >> 24) & 0xFF;
    rtcp_buf[9] = (rtp_stream->ssrc >> 16) & 0xFF;
    rtcp_buf[10] = (rtp_stream->ssrc >> 8) & 0xFF;
    rtcp_buf[11] = rtp_stream->ssrc & 0xFF;
    
    // Fraction lost + cumulative packets lost
    uint8_t fraction_lost = 0;  // Calculate from statistics
    uint32_t packets_lost = rtp_stream->packets_lost;
    rtcp_buf[12] = fraction_lost;
    rtcp_buf[13] = (packets_lost >> 16) & 0xFF;
    rtcp_buf[14] = (packets_lost >> 8) & 0xFF;
    rtcp_buf[15] = packets_lost & 0xFF;
    
    // Extended highest sequence number received
    uint32_t ext_seq = rtp_stream->last_seq;
    rtcp_buf[16] = (ext_seq >> 24) & 0xFF;
    rtcp_buf[17] = (ext_seq >> 16) & 0xFF;
    rtcp_buf[18] = (ext_seq >> 8) & 0xFF;
    rtcp_buf[19] = ext_seq & 0xFF;
    
    // Interarrival jitter (simplified, set to 0)
    rtcp_buf[20] = 0;
    rtcp_buf[21] = 0;
    rtcp_buf[22] = 0;
    rtcp_buf[23] = 0;
    
    // Last SR timestamp (LSR) - set to 0 if no SR received
    rtcp_buf[24] = 0;
    rtcp_buf[25] = 0;
    rtcp_buf[26] = 0;
    rtcp_buf[27] = 0;
    
    // Delay since last SR (DLSR) - set to 0
    rtcp_buf[28] = 0;
    rtcp_buf[29] = 0;
    rtcp_buf[30] = 0;
    rtcp_buf[31] = 0;
    
    rtcp_len = 32;
    
    // Send RTCP packet via track
    if (rtcSendMessage(track_id, (const char *)rtcp_buf, rtcp_len) < 0) {
        av_log(s, AV_LOG_WARNING, "Failed to send RTCP RR\n");
    } else {
        av_log(s, AV_LOG_DEBUG, "Sent RTCP RR (packets_received=%u, packets_lost=%u)\n",
               rtp_stream->packets_received, rtp_stream->packets_lost);
        rtp_stream->last_rtcp_time = now;
    }
}

// Helper function to create AVStream from payload type
static int create_stream_from_pt(AVFormatContext *s, int payload_type, 
                                  enum AVMediaType media_type, RTPStream *rtp_stream)
{
    AVStream *st;
    const struct {
        int pt;
        const char enc_name[6];
        enum AVMediaType codec_type;
        enum AVCodecID codec_id;
        int clock_rate;
        int audio_channels;
    } *pt_entry = NULL;
    
    // Find matching payload type
    for (int i = 0; dynamic_payload_types[i].pt != -1; i++) {
        if (dynamic_payload_types[i].pt == payload_type &&
            dynamic_payload_types[i].codec_type == media_type) {
            pt_entry = &dynamic_payload_types[i];
            break;
        }
    }
    
    if (!pt_entry) {
        av_log(s, AV_LOG_ERROR, "Unsupported payload type: %d for media type: %d\n", 
               payload_type, media_type);
        return AVERROR(EINVAL);
    }
    
    st = avformat_new_stream(s, NULL);
    if (!st) {
        av_log(s, AV_LOG_ERROR, "Failed to create new stream\n");
        return AVERROR(ENOMEM);
    }
    
    st->codecpar->codec_type = pt_entry->codec_type;
    st->codecpar->codec_id = pt_entry->codec_id;
    
    // Set time base to RTP clock rate
    st->time_base = (AVRational){1, pt_entry->clock_rate};
    
    if (pt_entry->codec_type == AVMEDIA_TYPE_AUDIO) {
        st->codecpar->sample_rate = pt_entry->clock_rate;
        if (pt_entry->audio_channels > 0) {
            st->codecpar->ch_layout.nb_channels = pt_entry->audio_channels;
            if (pt_entry->audio_channels == 2) {
                st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
            } else if (pt_entry->audio_channels == 1) {
                st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
            }
        }
    }
    
    // Initialize RTP stream state
    rtp_stream->stream_index = st->index;
    rtp_stream->clock_rate = pt_entry->clock_rate;
    rtp_stream->base_rtp_ts = 0;
    rtp_stream->base_pts = AV_NOPTS_VALUE;
    rtp_stream->last_rtp_ts = 0;
    rtp_stream->last_seq = 0;
    rtp_stream->packets_received = 0;
    rtp_stream->bytes_received = 0;
    rtp_stream->packets_lost = 0;
    rtp_stream->last_rtcp_time = 0;
    
    av_log(s, AV_LOG_INFO, "Created %s stream (index=%d): codec=%s, clock_rate=%d\n",
           av_get_media_type_string(pt_entry->codec_type), st->index,
           avcodec_get_name(pt_entry->codec_id), pt_entry->clock_rate);
    
    return 0;
}

static int whep2_read_header(AVFormatContext *s)
{
    WHEP2Context *whep2 = s->priv_data;
    rtcConfiguration config = {0};
    const char *ice_servers[] = {
        "stun:stun.l.google.com:19302",  // Google STUN server
        NULL
    };
    int ret;

    // Configure ICE servers for NAT traversal (important for SRS connections)
    config.iceServers = ice_servers;
    config.iceServersCount = 1;
    config.disableAutoNegotiation = true;
    
    ff_whip_whep_init_rtc_logger();
    s->ctx_flags |= AVFMTCTX_NOHEADER;

    // Store AVFormatContext pointer for callback
    whep2->avfmt_ctx = s;

    // Initialize RTP packet FIFO (can store 2048 Message pointers)
    whep2->rtp_fifo = av_fifo_alloc2(2048, sizeof(Message *), 0);
    if (!whep2->rtp_fifo) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate RTP FIFO\n");
        return AVERROR(ENOMEM);
    }

    // Initialize mutex and condition variable
    if ((ret = pthread_mutex_init(&whep2->rtp_fifo_lock, NULL)) != 0) {
        av_log(s, AV_LOG_ERROR, "Failed to initialize mutex: %s\n", av_err2str(AVERROR(ret)));
        av_fifo_freep2(&whep2->rtp_fifo);
        return AVERROR(ret);
    }

    if ((ret = pthread_cond_init(&whep2->rtp_fifo_cond, NULL)) != 0) {
        av_log(s, AV_LOG_ERROR, "Failed to initialize condition variable: %s\n", av_err2str(AVERROR(ret)));
        pthread_mutex_destroy(&whep2->rtp_fifo_lock);
        av_fifo_freep2(&whep2->rtp_fifo);
        return AVERROR(ret);
    }

    // Initialize WebRTC peer connection
    whep2->pc = rtcCreatePeerConnection(&config);
    if (whep2->pc <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to create peer connection\n");
        pthread_cond_destroy(&whep2->rtp_fifo_cond);
        pthread_mutex_destroy(&whep2->rtp_fifo_lock);
        av_fifo_freep2(&whep2->rtp_fifo);
        return AVERROR_EXTERNAL;
    }
    rtcSetUserPointer(whep2->pc, whep2);

    // Register all callbacks for SRS connection monitoring
    rtcSetStateChangeCallback(whep2->pc, state_change_callback);
    rtcSetGatheringStateChangeCallback(whep2->pc, gathering_state_callback);
    rtcSetLocalCandidateCallback(whep2->pc, local_candidate_callback);
    rtcSetDataChannelCallback(whep2->pc, datachannel_callback);
    
    // Optional: set error callback if available
    #ifdef RTC_ENABLE_WEBSOCKET
    // rtcSetErrorCallback is not standard in libdatachannel
    #endif
    
    av_log(s, AV_LOG_DEBUG, "WebRTC peer connection created successfully\n");
    av_log(s, AV_LOG_DEBUG, "ICE servers configured: %s\n", ice_servers[0]);

    // Add audio track
    av_log(s, AV_LOG_DEBUG, "Adding audio track...\n");
    whep2->audio_track = rtcAddTrack(whep2->pc, audio_mline);
    if (whep2->audio_track <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to add audio track\n");
        return AVERROR_EXTERNAL;
    }
    av_log(s, AV_LOG_DEBUG, "Audio track added (ID: %d)\n", whep2->audio_track);

    // Set callbacks for audio track
    rtcSetUserPointer(whep2->audio_track, whep2);
    if (rtcSetMessageCallback(whep2->audio_track, message_callback) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set audio track message callback\n");
        return AVERROR_EXTERNAL;
    }
    if (rtcSetOpenCallback(whep2->audio_track, open_callback) < 0) {
        av_log(s, AV_LOG_WARNING, "Failed to set audio track open callback\n");
    }

    // Add video track
    av_log(s, AV_LOG_DEBUG, "Adding video track...\n");
    whep2->video_track = rtcAddTrack(whep2->pc, video_mline);
    if (whep2->video_track <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to add video track\n");
        return AVERROR_EXTERNAL;
    }
    av_log(s, AV_LOG_DEBUG, "Video track added (ID: %d)\n", whep2->video_track);

    // Set callbacks for video track
    rtcSetUserPointer(whep2->video_track, whep2);
    if (rtcSetMessageCallback(whep2->video_track, message_callback) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set video track message callback\n");
        return AVERROR_EXTERNAL;
    }
    if (rtcSetOpenCallback(whep2->video_track, open_callback) < 0) {
        av_log(s, AV_LOG_WARNING, "Failed to set video track open callback\n");
    }

    // Perform SDP exchange with WHEP server
    av_log(s, AV_LOG_INFO, "Starting SDP exchange with WHEP server: %s\n", s->url);
    
    ret = ff_whip_whep_exchange_and_set_sdp(s, whep2->pc, whep2->token, &whep2->session_url);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to exchange SDP: %s\n", av_err2str(ret));
        return ret;
    }

    if (whep2->session_url) {
        av_log(s, AV_LOG_INFO, "SDP exchange successful, session URL: %s\n", whep2->session_url);
    } else {
        av_log(s, AV_LOG_INFO, "SDP exchange successful\n");
    }

    // Create streams for audio and video
    // Using default payload types from our offer
    av_log(s, AV_LOG_INFO, "Creating media streams...\n");
    
    // Create audio stream (OPUS, PT=111)
    ret = create_stream_from_pt(s, 111, AVMEDIA_TYPE_AUDIO, &whep2->audio_stream);
    if (ret < 0) {
        av_log(s, AV_LOG_WARNING, "Failed to create audio stream: %s\n", av_err2str(ret));
    }
    
    // Create video stream (H264, PT=98 as default)
    // In a real implementation, we should parse the SDP answer to get the actual codec
    ret = create_stream_from_pt(s, 98, AVMEDIA_TYPE_VIDEO, &whep2->video_stream);
    if (ret < 0) {
        av_log(s, AV_LOG_WARNING, "Failed to create video stream: %s\n", av_err2str(ret));
    }
    
    // Initialize timing
    whep2->first_packet_time = 0;

    av_log(s, AV_LOG_INFO, "WHEP session established, waiting for media packets...\n");
    
    return 0;
}

static int whep2_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    WHEP2Context *whep2 = s->priv_data;
    Message *msg = NULL;
    RTPStream *rtp_stream = NULL;
    int ret;
    int64_t arrival_time;
    
    // Wait for RTP packet from FIFO
    pthread_mutex_lock(&whep2->rtp_fifo_lock);
    while (av_fifo_can_read(whep2->rtp_fifo) == 0) {
        // Use timed wait to allow interruption
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;  // 1 second timeout
        
        ret = pthread_cond_timedwait(&whep2->rtp_fifo_cond, &whep2->rtp_fifo_lock, &ts);
        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&whep2->rtp_fifo_lock);
            return AVERROR(EAGAIN);
        }
        
        // Check for interrupt
        if (ff_check_interrupt(&s->interrupt_callback)) {
            pthread_mutex_unlock(&whep2->rtp_fifo_lock);
            return AVERROR_EXIT;
        }
    }
    
    av_fifo_read(whep2->rtp_fifo, &msg, 1);
    pthread_mutex_unlock(&whep2->rtp_fifo_lock);
    
    if (!msg || !msg->data || msg->size < 12) {
        av_log(s, AV_LOG_ERROR, "Invalid RTP packet\n");
        if (msg) {
            av_free(msg->data);
            av_free(msg);
        }
        return AVERROR_INVALIDDATA;
    }
    
    arrival_time = av_gettime_relative();
    if (whep2->first_packet_time == 0) {
        whep2->first_packet_time = arrival_time;
    }
    
    // Parse RTP header
    uint8_t *rtp_data = msg->data;
    int rtp_size = msg->size;
    
    // Check RTP version (should be 2)
    if ((rtp_data[0] & 0xC0) != 0x80) {
        av_log(s, AV_LOG_ERROR, "Invalid RTP version\n");
        av_free(msg->data);
        av_free(msg);
        return AVERROR_INVALIDDATA;
    }
    
    uint8_t payload_type = rtp_data[1] & 0x7F;
    uint16_t sequence = (rtp_data[2] << 8) | rtp_data[3];
    uint32_t timestamp = (rtp_data[4] << 24) | (rtp_data[5] << 16) | 
                         (rtp_data[6] << 8) | rtp_data[7];
    uint32_t ssrc = (rtp_data[8] << 24) | (rtp_data[9] << 16) | 
                    (rtp_data[10] << 8) | rtp_data[11];
    
    // Determine which stream this packet belongs to
    if (msg->track == whep2->audio_track) {
        rtp_stream = &whep2->audio_stream;
    } else if (msg->track == whep2->video_track) {
        rtp_stream = &whep2->video_stream;
    } else {
        av_log(s, AV_LOG_WARNING, "Unknown track ID: %d\n", msg->track);
        av_free(msg->data);
        av_free(msg);
        return AVERROR_INVALIDDATA;
    }
    
    // Update SSRC if not set
    if (rtp_stream->ssrc == 0) {
        rtp_stream->ssrc = ssrc;
    }
    
    // Calculate RTP header size
    int csrc_count = (rtp_data[0] & 0x0F);
    int has_extension = (rtp_data[0] & 0x10) != 0;
    int header_size = 12 + (csrc_count * 4);
    
    if (has_extension && rtp_size >= header_size + 4) {
        uint16_t ext_len = (rtp_data[header_size + 2] << 8) | rtp_data[header_size + 3];
        header_size += 4 + (ext_len * 4);
    }
    
    if (header_size >= rtp_size) {
        av_log(s, AV_LOG_ERROR, "Invalid RTP header size\n");
        av_free(msg->data);
        av_free(msg);
        return AVERROR_INVALIDDATA;
    }
    
    // Extract payload
    uint8_t *payload = rtp_data + header_size;
    int payload_size = rtp_size - header_size;
    
    // Handle padding
    if (rtp_data[0] & 0x20) {  // Padding bit set
        if (payload_size > 0) {
            int padding_len = payload[payload_size - 1];
            if (padding_len <= payload_size) {
                payload_size -= padding_len;
            }
        }
    }
    
    if (payload_size <= 0) {
        av_log(s, AV_LOG_WARNING, "Empty RTP payload\n");
        av_free(msg->data);
        av_free(msg);
        return AVERROR_INVALIDDATA;
    }
    
    // Create AVPacket
    ret = av_new_packet(pkt, payload_size);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate packet\n");
        av_free(msg->data);
        av_free(msg);
        return ret;
    }
    
    memcpy(pkt->data, payload, payload_size);
    pkt->stream_index = rtp_stream->stream_index;
    
    // Convert RTP timestamp to PTS
    pkt->pts = rtp_timestamp_to_pts(rtp_stream, timestamp, arrival_time - whep2->first_packet_time);
    pkt->dts = pkt->pts;  // For simplicity, set DTS = PTS
    
    // Update RTP stream statistics
    rtp_stream->packets_received++;
    rtp_stream->bytes_received += payload_size;
    
    // Detect packet loss
    if (rtp_stream->last_seq != 0) {
        uint16_t expected_seq = rtp_stream->last_seq + 1;
        if (sequence != expected_seq) {
            int lost = (sequence - expected_seq) & 0xFFFF;
            rtp_stream->packets_lost += lost;
            av_log(s, AV_LOG_WARNING, "Packet loss detected: expected seq=%u, got seq=%u (lost=%d)\n",
                   expected_seq, sequence, lost);
        }
    }
    rtp_stream->last_seq = sequence;
    rtp_stream->last_rtp_ts = timestamp;
    
    // Periodically send RTCP RR
    send_rtcp_rr(whep2, msg->track, rtp_stream);
    
    av_log(s, AV_LOG_TRACE, "RTP packet: track=%d, PT=%d, seq=%u, ts=%u, size=%d, pts=%ld\n",
           msg->track, payload_type, sequence, timestamp, payload_size, pkt->pts);
    
    // Clean up message
    av_free(msg->data);
    av_free(msg);
    
    return 0;
}

static int whep2_read_close(AVFormatContext *s)
{
    WHEP2Context *whep2 = s->priv_data;

    if (whep2->audio_track > 0) {
        rtcDeleteTrack(whep2->audio_track);
        whep2->audio_track = 0;
    }
    if (whep2->video_track > 0) {
        rtcDeleteTrack(whep2->video_track);
        whep2->video_track = 0;
    }
    if (whep2->pc > 0) {
        rtcDeletePeerConnection(whep2->pc);
        whep2->pc = 0;
    }

    // Clean up RTP FIFO
    if (whep2->rtp_fifo) {
        Message *msg;
        while (av_fifo_read(whep2->rtp_fifo, &msg, 1) >= 0) {
            av_free(msg->data);
            av_free(msg);
        }
        av_fifo_freep2(&whep2->rtp_fifo);
    }

    // Destroy mutex and condition variable
    pthread_cond_destroy(&whep2->rtp_fifo_cond);
    pthread_mutex_destroy(&whep2->rtp_fifo_lock);

    if (whep2->session_url) {
        ff_whip_whep_delete_session(s, whep2->token, whep2->session_url);
        av_freep(&whep2->session_url);
    }

    return 0;
}

#define OFFSET(x) offsetof(WHEP2Context, x)
static const AVOption whep2_options[] = {
    { "token", "set token to send in the Authorization header as \"Bearer <token>\"",
        OFFSET(token), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};

static const AVClass whep2_class = {
    .class_name = "WHEP2 demuxer",
    .item_name  = av_default_item_name,
    .option     = whep2_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_whep2_demuxer = {
    .p.name         = "whep2",
    .p.long_name    = NULL_IF_CONFIG_SMALL("WHEP2 (WebRTC-HTTP Egress Protocol 2)"),
    .p.flags        = AVFMT_NOFILE,
    .p.priv_class   = &whep2_class,
    .priv_data_size = sizeof(WHEP2Context),
    .read_header    = whep2_read_header,
    .read_packet    = whep2_read_packet,
    .read_close     = whep2_read_close,
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
};

