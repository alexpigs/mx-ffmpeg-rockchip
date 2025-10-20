
#include <rtc/rtc.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <inttypes.h>
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

// SDP_MAX_SIZE 已在 whip_whep.h 中定义
/*
暂时不处理音频，只处理视频
*/

// 获取当前时间戳（毫秒）
static inline int64_t get_timestamp_ms(void) {
    return av_gettime() / 1000;
}

// 日志宏，自动添加毫秒时间戳
#define WHEP_LOG(ctx, level, fmt, ...) \
    av_log(ctx, level, "[%lld ms] " fmt, (long long)get_timestamp_ms(), ##__VA_ARGS__)
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
    {99, "H265", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_HEVC, 90000, -1},
  {111, "OPUS", AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_OPUS, 48000, 2},
  {-1, "", AVMEDIA_TYPE_UNKNOWN, AV_CODEC_ID_NONE, -1, -1}
};

// 已移除音频支持,只处理视频
// static const char *audio_mline = ...

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

typedef struct WHEPContext {
    AVClass *class;
    char *token;
    char *session_url;
    int64_t pli_period;
    int64_t last_pli_time;

    // libdatachannel state
    int pc;
    int video_track;
    
    // Payload type to RTP context mapping (256 entries for all possible PT values)
    RTPDemuxContext *payload_map[256];
    
    // Video packet queue (produced by callback, consumed by read_packet)
    AVFifo *video_queue;
    pthread_mutex_t video_queue_mutex;
    pthread_cond_t video_queue_cond;
    
    // RTP contexts for video processing
    AVPacket *video_pkt;
    
    // Format context pointer for logging in callback
    AVFormatContext *fmt_ctx;
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

// Forward declaration
static RTPDemuxContext* whep_new_rtp_context(AVFormatContext *s, int payload_type);

static int whep_parse_sdp_and_create_contexts(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;
    char answer[SDP_MAX_SIZE];
    
    // Get the remote SDP answer
    if (rtcGetRemoteDescription(whep->pc, answer, sizeof(answer)) < 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to get remote description\n");
        return AVERROR_EXTERNAL;
    }
    
    WHEP_LOG(s, AV_LOG_DEBUG, "Parsing SDP answer: %s\n", answer);
    
    // Parse SDP and create RTP contexts for each media line
    char *line, *next_line;
    line = answer;
    
    while (line && *line) {
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        
        // Skip carriage return if present
        if (line[strlen(line) - 1] == '\r') {
            line[strlen(line) - 1] = '\0';
        }
        
        // Check for media line (m=)
        if (strncmp(line, "m=", 2) == 0) {
            char media_type[16];
            int port;
            char protocol[16];
            char fmt[256];
            
            if (sscanf(line, "m=%15s %d %15s %255s", media_type, &port, protocol, fmt) == 4) {
                WHEP_LOG(s, AV_LOG_DEBUG, "Found media line: %s %d %s %s\n", 
                       media_type, port, protocol, fmt);
                
                // Parse payload types - note that sscanf only reads first space-delimited token into fmt
                // We need to manually parse all payload types from the line
                char *pt_start = strstr(line, protocol);
                if (pt_start) {
                    pt_start = strchr(pt_start, ' '); // Skip protocol
                    if (pt_start) {
                        pt_start++; // Move past space
                        
                        // Now parse all payload types
                        char *payload_str = pt_start;
                        char *payload_end;
                        
                        while (payload_str && *payload_str) {
                            // Skip whitespace
                            while (*payload_str == ' ') payload_str++;
                            if (*payload_str == '\0') break;
                            
                            int payload_type = atoi(payload_str);
                            WHEP_LOG(s, AV_LOG_DEBUG, "Parsed payload type: %d from string '%s'\n", payload_type, payload_str);
                            
                            if (payload_type >= 0) {  // Changed from > 0 to >= 0 since PT 0 is valid
                                WHEP_LOG(s, AV_LOG_DEBUG, "Creating RTP context for payload type %d\n", payload_type);
                                
                                RTPDemuxContext *rtp_ctx = whep_new_rtp_context(s, payload_type);
                                if (!rtp_ctx) {
                                    WHEP_LOG(s, AV_LOG_ERROR, "Failed to create RTP context for payload type %d\n", payload_type);
                                    return AVERROR(ENOMEM);
                                }
                            }
                            
                            // Move to next payload type
                            payload_end = strchr(payload_str, ' ');
                            if (payload_end) {
                                payload_str = payload_end + 1;
                            } else {
                                break;
                            }
                        }
                    }
                }
            }
        }
        
        line = next_line;
    }
    
    return 0;
}

static RTPDemuxContext* whep_new_rtp_context(AVFormatContext *s, int payload_type)
{
    WHEPContext *whep = s->priv_data;
    RTPDemuxContext *rtp_ctx = NULL;
    AVStream *st = NULL;
    const RTPDynamicProtocolHandler *handler = NULL;
    PayloadContext *dynamic_protocol_context = NULL;

    st = avformat_new_stream(s, NULL);    
    if (!st) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to allocate stream\n");
        goto fail;
    }
    if (ff_rtp_get_codec_info(st->codecpar, payload_type) < 0) {
        for (int i = 0; dynamic_payload_types[i].pt != -1; i++) {
            if (dynamic_payload_types[i].pt == payload_type) {
                // 只处理视频codec
                if (dynamic_payload_types[i].codec_type != AVMEDIA_TYPE_VIDEO) {
                    WHEP_LOG(s, AV_LOG_WARNING, "Skipping non-video payload type %d\n", payload_type);
                    goto fail;
                }
                
                st->codecpar->codec_id   = dynamic_payload_types[i].codec_id;
                st->codecpar->codec_type = dynamic_payload_types[i].codec_type;

                if (dynamic_payload_types[i].clock_rate > 0)
                    st->codecpar->sample_rate = dynamic_payload_types[i].clock_rate;
                handler = ff_rtp_handler_find_by_name(dynamic_payload_types[i].enc_name,
                                                     dynamic_payload_types[i].codec_type);
                break;
            }
        }
    }
    
    // 设置时间基准和帧率
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        // 对于视频流：
        // - sample_rate 保留为 90000 (RTP 时钟频率，用于 RTP 解析)
        // - time_base 设置为 1/90000 (用于 PTS 计算)
        // - r_frame_rate 设置为合理的初始值 (用于显示和后续处理)
        if (st->codecpar->sample_rate > 0) {
            st->time_base = (AVRational){1, st->codecpar->sample_rate};
        }
        
        // 设置时间基准为 1/90000 (RTP 时钟频率)
        avpriv_set_pts_info(st, 33, 1, 90000);  // 33 bits PTS, timebase 1/90000
        
        // 推流源固定为 30fps，直接写死
        st->r_frame_rate = (AVRational){30, 1};
        st->avg_frame_rate = (AVRational){30, 1};
        
        WHEP_LOG(s, AV_LOG_INFO, "Video stream: timebase=%d/%d, r_frame_rate=%d/%d\n",
               st->time_base.num, st->time_base.den,
               st->r_frame_rate.num, st->r_frame_rate.den);
    } else if (st->codecpar->sample_rate > 0) {
        // 音频流（虽然当前不支持，但保留代码结构）
        st->time_base = (AVRational){1, st->codecpar->sample_rate};
    }

    // 使用更大的 jitter buffer (默认500太小，改为4096)
    rtp_ctx = ff_rtp_parse_open(s, st, payload_type, 4096);
    if (!rtp_ctx) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to open RTP context\n");
        goto fail;
    }
    if (handler) {
        ffstream(st)->need_parsing = handler->need_parsing;
        dynamic_protocol_context = av_mallocz(handler->priv_data_size);
        if (!dynamic_protocol_context) {
            WHEP_LOG(s, AV_LOG_ERROR, "Failed to allocate dynamic protocol context\n");
            goto fail;
        }
        if (handler->init && handler->init(s, st->index, dynamic_protocol_context) < 0) {
            WHEP_LOG(s, AV_LOG_ERROR, "Failed to initialize dynamic protocol context\n");
            goto fail;
        }
        ff_rtp_parse_set_dynamic_protocol(rtp_ctx, dynamic_protocol_context, handler);

        if (handler->parse_sdp_a_line) {
            char line[SDP_MAX_SIZE];
            // 只处理视频track
            int track_id = whep->video_track;
            if (whep_get_sdp_a_line(track_id, line, sizeof(line), payload_type) < 0) {
                WHEP_LOG(s, AV_LOG_WARNING, "No SDP a-line for payload type %d\n", payload_type);
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
            WHEP_LOG(s, AV_LOG_INFO, "Enabled AVSTREAM_PARSE_FULL for %s to merge NAL units\n",
                   st->codecpar->codec_id == AV_CODEC_ID_H264 ? "H.264" : "HEVC");
        }
    }
    
    // Add to payload type mapping table
    if (payload_type >= 0 && payload_type < 256) {
        whep->payload_map[payload_type] = rtp_ctx;
        WHEP_LOG(s, AV_LOG_DEBUG, "Added RTP context to payload_map[%d]\n", payload_type);
    }
    
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
    AVFormatContext *s = whep->fmt_ctx;
    int ret;
    uint8_t *data;
    static int warn_count = 0;

    if (size < 2) {
        WHEP_LOG(s, AV_LOG_DEBUG, "[WHEP] message_callback: size too small (%d)\n", size);
        return;
    }

    if (((RTP_PT_IS_RTCP(message[1]) && size < 8)) || size < 12) {
        WHEP_LOG(s, AV_LOG_DEBUG, "[WHEP] message_callback: skipping RTCP or invalid packet\n");
        return;
    }

    // 只处理视频track
    if (id != whep->video_track) {
        if (warn_count++ % 100 == 0) {
            WHEP_LOG(s, AV_LOG_WARNING, "[WHEP] Unknown track (track=%d, video_track=%d)\n", 
                   id, whep->video_track);
        }
        return;
    }
    
    // Extract payload type from RTP packet header (message[1] & 0x7F)
    int pt = ((uint8_t)message[1]) & 0x7F;
    RTPDemuxContext *rtp_ctx = whep->payload_map[pt];
    
    if (!rtp_ctx) {
        if (warn_count++ % 100 == 0) {
            WHEP_LOG(s, AV_LOG_WARNING, "[WHEP] No RTP context for payload type %d\n", pt);
        }
        return;
    }

    // 复制数据供 ff_rtp_parse_packet 使用
    data = av_memdup(message, size);
    if (!data) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to duplicate RTP packet\n");
        return;
    }

    // Allocate packet for this RTP context
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to allocate packet for PT %d\n", pt);
        av_free(data);
        return;
    }

    // 直接调用 ff_rtp_parse_packet
    ret = ff_rtp_parse_packet(rtp_ctx, pkt, &data, size);
    
    
    // 处理所有返回的包：ret=0(单包) 或 ret=1(还有更多缓冲包)
    while (ret >= 0) {
        // Got a complete frame, add to video queue
        WHEP_LOG(s, AV_LOG_DEBUG, "[WHEP] ff_rtp_parse_packet returned %d for PT %d\n", ret, pt);
        
        pthread_mutex_lock(&whep->video_queue_mutex);
        
        // 尝试写入 FIFO
        int write_ret = av_fifo_write(whep->video_queue, &pkt, 1);
        if (write_ret < 0) {
            // 队列满，丢弃最旧的包
            AVPacket *old_pkt = NULL;
            if (av_fifo_can_read(whep->video_queue) > 0) {
                av_fifo_read(whep->video_queue, &old_pkt, 1);
                if (old_pkt) {
                    WHEP_LOG(s, AV_LOG_WARNING, "[WHEP] Video queue full, dropping oldest frame (pts=%" PRId64 ")\n", old_pkt->pts);
                    av_packet_free(&old_pkt);
                }
            }
            // 再次尝试写入新包
            write_ret = av_fifo_write(whep->video_queue, &pkt, 1);
            if (write_ret < 0) {
                WHEP_LOG(s, AV_LOG_ERROR, "[WHEP] Failed to write to video queue after dropping old frame\n");
                av_packet_free(&pkt);
            }
        }
        
        if (write_ret >= 0) {
            size_t queue_size = av_fifo_can_read(whep->video_queue);
            pthread_cond_signal(&whep->video_queue_cond);
         WHEP_LOG(s, AV_LOG_DEBUG, "[WHEP] Got complete video frame, added to queue (size=%d, pts=%" PRId64 ", queue_size=%zu, PT=%d)\n", 
             pkt->size, pkt->pts, queue_size, pt);
        }
        
        pthread_mutex_unlock(&whep->video_queue_mutex);
        
        // 如果 ret == 1，说明还有更多缓冲的包，继续读取
        if (ret == 1) {
            pkt = av_packet_alloc();
            if (!pkt) {
                WHEP_LOG(s, AV_LOG_ERROR, "Failed to allocate packet for buffered frame\n");
                break;
            }
            ret = ff_rtp_parse_packet(rtp_ctx, pkt, NULL, 0);
            WHEP_LOG(s, AV_LOG_DEBUG, "[WHEP] ff_rtp_parse_packet (buffered) returned %d\n", ret);
        } else {
            // ret == 0，没有更多缓冲包了
            break;
        }
    }
    
    // If the loop ended with ret < 0, we need to free the last allocated packet
    if (ret < 0 && pkt) {
        av_packet_free(&pkt);
    }
    
    av_free(data);
}

static int whep_read_header(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;
    WHEP_LOG(s, AV_LOG_INFO, "[WHEP] whep_read_header called\n");
    rtcConfiguration config = {0};

    ff_whip_whep_init_rtc_logger();
    s->ctx_flags |= AVFMTCTX_NOHEADER;

    // 保存格式上下文指针供 callback 使用
    whep->fmt_ctx = s;
    
    // Initialize payload_map to NULL
    memset(whep->payload_map, 0, sizeof(whep->payload_map));
    
    // Initialize video packet queue using AVFifo
    whep->video_queue = av_fifo_alloc2(128, sizeof(AVPacket*), AV_FIFO_FLAG_AUTO_GROW);
    if (!whep->video_queue) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to allocate video queue\n");
        return AVERROR(ENOMEM);
    }
    av_fifo_auto_grow_limit(whep->video_queue, 1024);  // 最多允许增长到 1024 个包
    
    if (pthread_mutex_init(&whep->video_queue_mutex, NULL) != 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to initialize video queue mutex\n");
        av_fifo_freep2(&whep->video_queue);
        return AVERROR(ENOMEM);
    }
    
    if (pthread_cond_init(&whep->video_queue_cond, NULL) != 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to initialize video queue condition\n");
        pthread_mutex_destroy(&whep->video_queue_mutex);
        av_fifo_freep2(&whep->video_queue);
        return AVERROR(ENOMEM);
    }
    
    // Allocate video packet for RTP processing
    whep->video_pkt = av_packet_alloc();
    if (!whep->video_pkt) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to allocate video packet\n");
        return AVERROR(ENOMEM);
    }
    
    // 已移除音频支持
    
    // Initialize WebRTC peer connection
    whep->pc = rtcCreatePeerConnection(&config);
    if (whep->pc <= 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to create peer connection\n");
        return AVERROR_EXTERNAL;
    }
    rtcSetUserPointer(whep->pc, whep);

    // 只添加视频track
    whep->video_track = rtcAddTrack(whep->pc, video_mline);
    if (whep->video_track <= 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to add video track\n");
        return AVERROR_EXTERNAL;
    }

    if (rtcSetMessageCallback(whep->video_track, message_callback) < 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to set video track message callback\n");
        return AVERROR_EXTERNAL;
    }

    int ret = ff_whip_whep_exchange_and_set_sdp(s, whep->pc, whep->token, &whep->session_url);
    if (ret < 0) {
        return ret;
    }
    
    // Parse SDP and create RTP contexts
    ret = whep_parse_sdp_and_create_contexts(s);
    if (ret < 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to parse SDP and create RTP contexts\n");
        return ret;
    }
    
    WHEP_LOG(s, AV_LOG_INFO, "[WHEP] Created %d streams\n", s->nb_streams);
    for (int i = 0; i < s->nb_streams; i++) {
        WHEP_LOG(s, AV_LOG_INFO, "[WHEP] Stream %d: codec_type=%d (%s), codec_id=%d\n",
               i, s->streams[i]->codecpar->codec_type,
               av_get_media_type_string(s->streams[i]->codecpar->codec_type),
               s->streams[i]->codecpar->codec_id);
    }
    
    return 0;
}

static int whep_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    WHEPContext *whep = s->priv_data;
    AVPacket *video_pkt = NULL;
    
    if (rtcIsClosed(whep->video_track)) {
        WHEP_LOG(s, AV_LOG_ERROR, "Connection closed\n");
        return AVERROR_EOF;
    }
    
    // Get packet from video queue
    pthread_mutex_lock(&whep->video_queue_mutex);
    
    // Check if queue is empty
    if (av_fifo_can_read(whep->video_queue) == 0) {
        pthread_mutex_unlock(&whep->video_queue_mutex);
        // WHEP_LOG(s, AV_LOG_DEBUG, "[WHEP] No packet available, returning EAGAIN\n");
        return AVERROR(EAGAIN);
    }
    
    // Read packet from FIFO
    av_fifo_read(whep->video_queue, &video_pkt, 1);
    
    pthread_mutex_unlock(&whep->video_queue_mutex);
    
    if (!video_pkt) {
        WHEP_LOG(s, AV_LOG_ERROR, "[WHEP] Got NULL packet from queue\n");
        return AVERROR(EAGAIN);
    }
    
    // Move packet data to output
    av_packet_move_ref(pkt, video_pkt);
    av_packet_free(&video_pkt);
    
    // Verify stream index is valid
    if (pkt->stream_index < 0 || pkt->stream_index >= s->nb_streams) {
        WHEP_LOG(s, AV_LOG_ERROR, "[WHEP] Invalid stream_index %d (nb_streams=%d)\n", 
               pkt->stream_index, s->nb_streams);
        av_packet_unref(pkt);
        return AVERROR(EINVAL);
    }
    
    WHEP_LOG(s, AV_LOG_DEBUG, "[WHEP] Returning packet: stream=%d, pts=%" PRId64 ", size=%d, keyframe=%d\n",
           pkt->stream_index, pkt->pts, pkt->size, !!(pkt->flags & AV_PKT_FLAG_KEY));
    
    return 0;
}

static int whep_read_close(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;

    if (whep->video_track > 0) {
        rtcDeleteTrack(whep->video_track);
        whep->video_track = 0;
    }
    if (whep->pc > 0) {
        rtcDeletePeerConnection(whep->pc);
        whep->pc = 0;
    }

    // Clean up RTP contexts from payload_map
    for (int i = 0; i < 256; i++) {
        if (whep->payload_map[i]) {
            PayloadContext *payload_ctx = whep->payload_map[i]->dynamic_protocol_context;
            ff_rtp_parse_close(whep->payload_map[i]);
            av_freep(&payload_ctx);
            whep->payload_map[i] = NULL;
        }
    }

    // Clean up video queue
    if (whep->video_queue) {
        // 释放队列中剩余的所有包
        AVPacket *pkt;
        while (av_fifo_can_read(whep->video_queue) > 0) {
            av_fifo_read(whep->video_queue, &pkt, 1);
            if (pkt) {
                av_packet_free(&pkt);
            }
        }
        av_fifo_freep2(&whep->video_queue);
    }
    
    pthread_mutex_destroy(&whep->video_queue_mutex);
    pthread_cond_destroy(&whep->video_queue_cond);

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