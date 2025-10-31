/*
 * WHEP (WebRTC HTTP Egress Protocol) demuxer
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

/**
 * @file
 * WHEP (WebRTC HTTP Egress Protocol) demuxer
 */

#include <rtc/rtc.h>

#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/avstring.h"
#include "libavutil/thread.h"
#include "libavutil/fifo.h"
#include "libavutil/mem.h"
#include "libavutil/bprint.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "url.h"
#include "http.h"
#include "avio_internal.h"
#include "whip_whep.h"
#include "rtpdec.h"

typedef struct WHEPContext {
    AVClass *class;
    char *whep_url;              ///< WHEP server URL
    char *token;                 ///< Bearer token for authentication
    int timeout;                 ///< Connection timeout in milliseconds
    int buffer_size;             ///< Buffer size for incoming packets
    int max_retry;               ///< Maximum number of reconnection attempts
    
    // 内部状态
    int initialized;
    int64_t start_time;
    
    // Packet 队列
    AVFifo *audio_queue;         ///< 音频packet队列
    AVFifo *video_queue;         ///< 视频packet队列
    
    // 同步机制
    pthread_mutex_t mutex;       ///< 保护队列的互斥锁
    pthread_cond_t cond;         ///< 用于等待数据的条件变量
    
    int abort_request;           ///< 请求终止标志
    int eof_reached;             ///< 是否到达EOF
    
    // WebRTC 相关
    int peer_connection;         ///< libdatachannel PeerConnection ID
    int audio_track;             ///< 音频 track ID
    int video_track;             ///< 视频 track ID
    char *resource_url;          ///< WHEP 资源 URL (用于DELETE)
    
    // RTP demuxer 数组，按 Payload Type 索引 (0-127)
    // 一个 track 可能有多个 PT (例如主流+RTX)
    RTPDemuxContext *rtp_demux[128];
    
    // SSRC 信息 (从接收的 RTP 包中提取，用于日志)
    uint32_t audio_ssrc;          ///< 音频 SSRC
    uint32_t video_ssrc;          ///< 视频 SSRC
} WHEPContext;


/**
 * 获取当前时间戳字符串 (毫秒精度)
 * 返回格式: [HH:MM:SS.mmm]
 */
static const char* get_timestamp(void)
{
    static __thread char buf[32];
    int64_t now = av_gettime_relative();
    int64_t ms = now / 1000;
    int h = (ms / 3600000) % 24;
    int m = (ms / 60000) % 60;
    int s = (ms / 1000) % 60;
    int msec = ms % 1000;
    snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d]", h, m, s, msec);
    return buf;
}

/**
 * PLI (Picture Loss Indication) 回调
 * 当 libdatachannel 检测到需要关键帧时触发
 */
static void RTC_API on_pli_request(int tr, void *ptr)
{
    av_log(NULL, AV_LOG_INFO, "%s 🔑 收到 PLI 请求，libdatachannel 将自动请求关键帧\n", get_timestamp());
}


/**
 * 解析 SDP answer，动态创建对应的流，并初始化 RTPDemuxContext
 * @param avctx AVFormatContext
 * @param whep WHEP 上下文
 * @param sdp_answer SDP answer 字符串
 * @return 0表示成功，负值表示错误
 */
static int whep_parse_sdp_and_init_rtp(AVFormatContext *avctx, WHEPContext *whep, const char *sdp_answer)
{
    const char *line = sdp_answer;
    const char *next_line;
    int pt, clock_rate, channels;
    char codec_name[64];
    AVStream *st = NULL;
    AVStream *video_stream = NULL;  // 视频流指针
    AVStream *audio_stream = NULL;  // 音频流指针
    enum AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
    
    av_log(avctx, AV_LOG_INFO, "%s 开始解析 SDP answer，动态创建流并初始化 RTP demuxer...\n", get_timestamp());
    
    // 初始化数组
    memset(whep->rtp_demux, 0, sizeof(whep->rtp_demux));
    
    // 逐行解析 SDP
    while (line && *line) {
        // 查找下一行
        next_line = strchr(line, '\n');
        int line_len = next_line ? (next_line - line) : strlen(line);
        
        // 跳过 \r
        if (line_len > 0 && line[line_len - 1] == '\r')
            line_len--;
        
        // 解析 m= 行以确定媒体类型，并动态创建对应的流
        if (line_len > 2 && line[0] == 'm' && line[1] == '=') {
            if (av_strstart(line, "m=audio", NULL)) {
                media_type = AVMEDIA_TYPE_AUDIO;
                
                // 如果音频流还未创建，则创建它
                if (!audio_stream) {
                    audio_stream = avformat_new_stream(avctx, NULL);
                    if (!audio_stream) {
                        av_log(avctx, AV_LOG_ERROR, "%s 创建音频流失败\n", get_timestamp());
                        return AVERROR(ENOMEM);
                    }
                    audio_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                    avpriv_set_pts_info(audio_stream, 64, 1, 1000000);
                    av_log(avctx, AV_LOG_INFO, "%s 创建音频流 (index=%d)\n", get_timestamp(), audio_stream->index);
                }
                st = audio_stream;
                
            } else if (av_strstart(line, "m=video", NULL)) {
                media_type = AVMEDIA_TYPE_VIDEO;
                
                // 如果视频流还未创建，则创建它
                if (!video_stream) {
                    video_stream = avformat_new_stream(avctx, NULL);
                    if (!video_stream) {
                        av_log(avctx, AV_LOG_ERROR, "%s 创建视频流失败\n", get_timestamp());
                        return AVERROR(ENOMEM);
                    }
                    video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                    avpriv_set_pts_info(video_stream, 64, 1, 1000000);
                    av_log(avctx, AV_LOG_INFO, "%s 创建视频流 (index=%d)\n", get_timestamp(), video_stream->index);
                }
                st = video_stream;
            }
        }
        
        // 解析 a=rtpmap: 行
        // 格式: a=rtpmap:<payload type> <encoding name>/<clock rate>[/<channels>]
        if (line_len > 9 && av_strstart(line, "a=rtpmap:", NULL)) {
            const char *p = line + 9;
            
            // 解析 payload type
            pt = atoi(p);
            
            // 跳过数字到空格
            while (*p && *p != ' ')
                p++;
            while (*p == ' ')
                p++;
            
            // 解析 codec name
            int i = 0;
            while (*p && *p != '/' && i < sizeof(codec_name) - 1) {
                codec_name[i++] = *p++;
            }
            codec_name[i] = '\0';
            
            // 解析 clock rate
            if (*p == '/') {
                p++;
                clock_rate = atoi(p);
                
                // 解析 channels (音频)
                while (*p && *p != '/')
                    p++;
                if (*p == '/') {
                    p++;
                    channels = atoi(p);
                } else {
                    channels = (media_type == AVMEDIA_TYPE_AUDIO) ? 2 : 0;
                }
            } else {
                clock_rate = 90000;  // 视频默认
                channels = 0;
            }
            
            av_log(avctx, AV_LOG_INFO, "%s 解析 rtpmap: PT=%d, codec=%s, clock_rate=%d, channels=%d, media_type=%d\n",
                   get_timestamp(), pt, codec_name, clock_rate, channels, media_type);
            
            // 只为主流 codec 创建 RTPDemuxContext (跳过 rtx/red 等辅助流)
            if (st && media_type != AVMEDIA_TYPE_UNKNOWN &&
                av_strcasecmp(codec_name, "rtx") != 0 &&
                av_strcasecmp(codec_name, "red") != 0 &&
                av_strcasecmp(codec_name, "ulpfec") != 0) {
                
                // 映射 codec name 到 AVCodecID
                enum AVCodecID codec_id = AV_CODEC_ID_NONE;
                if (av_strcasecmp(codec_name, "H264") == 0) {
                    codec_id = AV_CODEC_ID_H264;
                } else if (av_strcasecmp(codec_name, "H265") == 0 || av_strcasecmp(codec_name, "HEVC") == 0) {
                    codec_id = AV_CODEC_ID_HEVC;
                } else if (av_strcasecmp(codec_name, "VP8") == 0) {
                    codec_id = AV_CODEC_ID_VP8;
                } else if (av_strcasecmp(codec_name, "VP9") == 0) {
                    codec_id = AV_CODEC_ID_VP9;
                } else if (av_strcasecmp(codec_name, "AV1") == 0) {
                    codec_id = AV_CODEC_ID_AV1;
                } else if (av_strcasecmp(codec_name, "opus") == 0) {
                    codec_id = AV_CODEC_ID_OPUS;
                } else if (av_strcasecmp(codec_name, "PCMU") == 0) {
                    codec_id = AV_CODEC_ID_PCM_MULAW;
                } else if (av_strcasecmp(codec_name, "PCMA") == 0) {
                    codec_id = AV_CODEC_ID_PCM_ALAW;
                } else if (av_strcasecmp(codec_name, "G722") == 0) {
                    codec_id = AV_CODEC_ID_ADPCM_G722;
                } else {
                    av_log(avctx, AV_LOG_WARNING, "%s 未识别的 codec: %s, 跳过\n", get_timestamp(), codec_name);
                }
                
                if (codec_id != AV_CODEC_ID_NONE) {
                    // 更新 AVStream 的 codec_id
                    st->codecpar->codec_id = codec_id;
                    
                    // 更新采样率/时钟频率
                    if (media_type == AVMEDIA_TYPE_AUDIO) {
                        st->codecpar->sample_rate = clock_rate;
                        st->codecpar->ch_layout.nb_channels = channels;
                    }
                    
                    av_log(avctx, AV_LOG_INFO, "%s 设置流参数: index=%d, codec_id=%d (%s), clock_rate=%d\n", 
                           get_timestamp(), st->index, codec_id, codec_name, clock_rate);
                    
                    // 创建 RTPDemuxContext (使用较小的 jitter buffer 以降低延迟)
                    // 10 个包的缓冲区足够处理网络抖动，同时保持低延迟
                    whep->rtp_demux[pt] = ff_rtp_parse_open(avctx, st, pt, 10);
                    if (!whep->rtp_demux[pt]) {
                        av_log(avctx, AV_LOG_ERROR, "%s 创建 RTPDemuxContext 失败 (PT=%d)\n", get_timestamp(), pt);
                        return AVERROR(ENOMEM);
                    }
                    
                    av_log(avctx, AV_LOG_INFO, "%s 成功创建 RTPDemuxContext: PT=%d → stream[%d] (jitter_buffer=10)\n", get_timestamp(), pt, st->index);
                }
            }
        }
        
        // 解析 a=ssrc: 行，提取 SSRC 用于后续验证
        // 格式: a=ssrc:<ssrc> <attribute>:<value>
        if (line_len > 7 && av_strstart(line, "a=ssrc:", NULL)) {
            const char *p = line + 7;
            uint32_t ssrc = (uint32_t)strtoul(p, NULL, 10);
            
            if (ssrc > 0) {
                if (media_type == AVMEDIA_TYPE_AUDIO) {
                    whep->audio_ssrc = ssrc;
                    av_log(avctx, AV_LOG_INFO, "%s 从 SDP 提取音频 SSRC: 0x%08x (%u)\n", 
                           get_timestamp(), ssrc, ssrc);
                } else if (media_type == AVMEDIA_TYPE_VIDEO) {
                    whep->video_ssrc = ssrc;
                    av_log(avctx, AV_LOG_INFO, "%s 从 SDP 提取视频 SSRC: 0x%08x (%u)\n", 
                           get_timestamp(), ssrc, ssrc);
                }
            }
        }
        
        // 移动到下一行
        if (next_line) {
            line = next_line + 1;
        } else {
            break;
        }
    }
    
    // 总结创建的流
    av_log(avctx, AV_LOG_INFO, "%s SDP 解析完成，共创建 %d 个流:\n", get_timestamp(), avctx->nb_streams);
    if (video_stream) {
        av_log(avctx, AV_LOG_INFO, "%s   - 视频流: index=%d, codec_id=%d, SSRC=0x%08x\n", 
               get_timestamp(), video_stream->index, video_stream->codecpar->codec_id, whep->video_ssrc);
    }
    if (audio_stream) {
        av_log(avctx, AV_LOG_INFO, "%s   - 音频流: index=%d, codec_id=%d, sample_rate=%d, SSRC=0x%08x\n", 
               get_timestamp(), audio_stream->index, audio_stream->codecpar->codec_id,
               audio_stream->codecpar->sample_rate, whep->audio_ssrc);
    }
    
    if (!video_stream && !audio_stream) {
        av_log(avctx, AV_LOG_ERROR, "%s SDP 中未找到任何可用的媒体流\n", get_timestamp());
        return AVERROR_INVALIDDATA;
    }
    
    return 0;
}

/**
 * libdatachannel 状态改变回调
 */
static void on_state_change(int pc, rtcState state, void *user_ptr)
{
    const char *state_str[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
    av_log(NULL, AV_LOG_INFO, "%s PeerConnection 状态变更: %s\n", 
           get_timestamp(), state < 6 ? state_str[state] : "Unknown");
}

/**
 * libdatachannel gathering 状态回调
 */
static void on_gathering_state_change(int pc, rtcGatheringState state, void *user_ptr)
{
    const char *state_str[] = {"New", "InProgress", "Complete"};
    av_log(NULL, AV_LOG_INFO, "%s ICE Gathering 状态: %s\n", 
           get_timestamp(), state < 3 ? state_str[state] : "Unknown");
}

/**
 * Track 打开回调
 */
static void on_track_open(int tr, void *user_ptr)
{
    av_log(NULL, AV_LOG_INFO, "%s Track 已打开 (ID: %d)\n", get_timestamp(), tr);
}

/**
 * 音频 Track 消息回调 - 接收 RTP 数据
 */
static void on_audio_message(int tr, const char *data, int size, void *user_ptr)
{
    WHEPContext *whep = (WHEPContext *)user_ptr;
    
    // 验证是否是我们的音频 track
    if (tr != whep->audio_track) {
        av_log(NULL, AV_LOG_WARNING, "%s 收到未知 track 的音频数据: %d (expected %d)\n", get_timestamp(), tr, whep->audio_track);
        return;
    }
    
    // RTP头部至少12字节
    if (size < 12) {
        av_log(NULL, AV_LOG_WARNING, "%s 收到的RTP包太小: %d bytes\n", get_timestamp(), size);
        return;
    }
    
    // 解析 RTP 头部获取 Payload Type 和 SSRC
    uint8_t payload_type = (uint8_t)data[1] & 0x7F;
    
    // 提取 SSRC (字节 8-11, 大端序)
    uint32_t ssrc = ((uint32_t)(uint8_t)data[8] << 24) |
                    ((uint32_t)(uint8_t)data[9] << 16) |
                    ((uint32_t)(uint8_t)data[10] << 8) |
                    ((uint32_t)(uint8_t)data[11]);
    
    // SSRC 过滤：只接受 SDP Answer 中声明的 SSRC
    if (whep->audio_ssrc != 0 && ssrc != whep->audio_ssrc) {
        av_log(NULL, AV_LOG_WARNING, "%s 拒绝不匹配的音频 SSRC: 0x%08x (期望 0x%08x, track=%d)\n", 
               get_timestamp(), ssrc, whep->audio_ssrc, tr);
        return;  // 丢弃这个包
    }
    
    // 首次接收时保存 SSRC（用于向后兼容，如果 SDP 中没有声明）
    if (whep->audio_ssrc == 0) {
        whep->audio_ssrc = ssrc;
        av_log(NULL, AV_LOG_INFO, "%s 锁定音频 SSRC: 0x%08x (track=%d)\n", get_timestamp(), ssrc, tr);
    }
    
    // 查找对应的 RTPDemuxContext
    RTPDemuxContext *rtp_demux = whep->rtp_demux[payload_type];
    if (!rtp_demux) {
        av_log(NULL, AV_LOG_WARNING, "%s 未找到 PT=%u 的 RTP demuxer\n", get_timestamp(), payload_type);
        return;
    }
    
    // av_log(NULL, AV_LOG_DEBUG, "%s 收到音频 RTP: track=%d, PT=%u, size=%d bytes, ssrc=0x%08x\n", 
    //        get_timestamp(), tr, payload_type, size, ssrc);
    
    // 重要：拷贝一份 RTP 数据，因为 ff_rtp_parse_packet 可能接管指针所有权
    // 当 RTP 包需要重排序时，enqueue_packet 会保存 buf 指针并稍后释放
    uint8_t *buf_ptr = av_memdup(data, size);
    if (!buf_ptr) {
        av_log(NULL, AV_LOG_ERROR, "%s 拷贝 RTP 缓冲区失败\n", get_timestamp());
        return;
    }
    
    // 调用 ff_rtp_parse_packet 解析 RTP 数据
    // 返回值: 0 = 完整包, 1 = 完整包且还有更多, -1 = 无包/错误
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_ERROR, "%s 分配 AVPacket 失败\n", get_timestamp());
        av_free(buf_ptr);
        return;
    }
    
    int ret = ff_rtp_parse_packet(rtp_demux, pkt, &buf_ptr, size);
    
    // 循环读取所有可用的完整包（返回值为1表示还有更多）
    while (ret >= 0) {
        // 成功组包完成
        av_log(NULL, AV_LOG_INFO, "%s 🎵 音频组包完成: stream=%d, size=%d bytes, pts=%ld, dts=%ld, flags=%d\n",
               get_timestamp(), pkt->stream_index,
               pkt->size,
               pkt->pts,
               pkt->dts,
               pkt->flags);
        
        // TODO: 这里应该将 packet 放入队列供 read_packet 读取
        // 目前只打印信息，稍后实现队列
        
        if (ret == 0) {
            // 没有更多数据了
            break;
        }
        
        // ret == 1，还有更多数据，用 NULL/0 继续读取内部队列
        av_packet_unref(pkt);
        uint8_t *null_ptr = NULL;
        ret = ff_rtp_parse_packet(rtp_demux, pkt, &null_ptr, 0);
    }
    
    if (ret < 0) {
        // 没有完整包（需要更多 RTP 数据，或错误）
        // av_log(NULL, AV_LOG_DEBUG, "%s 音频 RTP 等待更多数据组包 (ret=%d)\n", get_timestamp(), ret);
    }
    
    // 如果 buf_ptr 不为 NULL，说明 ff_rtp_parse_packet 没有接管所有权，需要我们释放
    if (buf_ptr) {
        av_free(buf_ptr);
    }
    
    av_packet_free(&pkt);
}

/**
 * 视频 Track 消息回调 - 接收 RTP 数据
 */
static void on_video_message(int tr, const char *data, int size, void *user_ptr)
{
    WHEPContext *whep = (WHEPContext *)user_ptr;
    static int consecutive_errors = 0;  // 连续解码错误计数
    
    // 验证是否是我们的视频 track
    if (tr != whep->video_track) {
        av_log(NULL, AV_LOG_WARNING, "%s 收到未知 track 的视频数据: %d (expected %d)\n", get_timestamp(), tr, whep->video_track);
        return;
    }
    
    // RTP头部至少12字节
    if (size < 12) {
        av_log(NULL, AV_LOG_WARNING, "%s 收到的RTP包太小: %d bytes\n", get_timestamp(), size);
        return;
    }
    
    // 解析 RTP 头部获取 Payload Type 和 SSRC
    uint8_t payload_type = (uint8_t)data[1] & 0x7F;
    
    // 提取 SSRC (字节 8-11, 大端序)
    uint32_t ssrc = ((uint32_t)(uint8_t)data[8] << 24) |
                    ((uint32_t)(uint8_t)data[9] << 16) |
                    ((uint32_t)(uint8_t)data[10] << 8) |
                    ((uint32_t)(uint8_t)data[11]);
    
    // SSRC 过滤：只接受 SDP Answer 中声明的 SSRC
    if (whep->video_ssrc != 0 && ssrc != whep->video_ssrc) {
        av_log(NULL, AV_LOG_WARNING, "%s 拒绝不匹配的视频 SSRC: 0x%08x (期望 0x%08x, track=%d)\n", 
               get_timestamp(), ssrc, whep->video_ssrc, tr);
        return;  // 丢弃这个包
    }
    
    // 首次接收时保存 SSRC（用于向后兼容，如果 SDP 中没有声明）
    if (whep->video_ssrc == 0) {
        whep->video_ssrc = ssrc;
        av_log(NULL, AV_LOG_INFO, "%s 锁定视频 SSRC: 0x%08x (track=%d)\n", get_timestamp(), ssrc, tr);
    }
    
    // 查找对应的 RTPDemuxContext
    RTPDemuxContext *rtp_demux = whep->rtp_demux[payload_type];
    if (!rtp_demux) {
        av_log(NULL, AV_LOG_WARNING, "%s 未找到 PT=%u 的 RTP demuxer\n", get_timestamp(), payload_type);
        return;
    }
    
    // ========== 详细解析 RTP 头部 ==========
    uint8_t version = (data[0] >> 6) & 0x03;
    uint8_t padding = (data[0] >> 5) & 0x01;
    uint8_t extension = (data[0] >> 4) & 0x01;
    uint8_t cc = data[0] & 0x0F;  // CSRC count
    uint8_t marker = (data[1] >> 7) & 0x01;
    uint16_t seq = ((uint8_t)data[2] << 8) | (uint8_t)data[3];
    uint32_t timestamp = ((uint32_t)(uint8_t)data[4] << 24) |
                         ((uint32_t)(uint8_t)data[5] << 16) |
                         ((uint32_t)(uint8_t)data[6] << 8) |
                         ((uint32_t)(uint8_t)data[7]);
    
    int rtp_header_size = 12 + (cc * 4);  // 基础头 + CSRC
    if (extension) {
        // 如果有扩展头，跳过它
        if (size >= rtp_header_size + 4) {
            uint16_t ext_len = ((uint8_t)data[rtp_header_size + 2] << 8) | 
                               (uint8_t)data[rtp_header_size + 3];
            rtp_header_size += 4 + (ext_len * 4);
        }
    }
    
    int payload_size = size - rtp_header_size;
    
    // av_log(NULL, AV_LOG_DEBUG, "%s 收到视频 RTP: track=%d, PT=%u, size=%d, seq=%u, ts=%u, marker=%d, ssrc=0x%08x\n", 
    //        get_timestamp(), tr, payload_type, size, seq, timestamp, marker, ssrc);
    
    // ========== 详细解析 H.264 RTP Payload ==========
    if (payload_size > 0 && rtp_header_size < size) {
        const uint8_t *payload = (const uint8_t *)data + rtp_header_size;
        uint8_t nal_header = payload[0];
        uint8_t nal_type = nal_header & 0x1F;
        uint8_t nri = (nal_header >> 5) & 0x03;
        
        const char *nal_type_str = "Unknown";
        const char *packet_type_str = "Unknown";
        
        // NAL 类型名称
        switch (nal_type) {
            case 0:  nal_type_str = "Unspecified"; break;
            case 1:  nal_type_str = "Non-IDR Slice"; packet_type_str = "Single NAL"; break;
            case 2:  nal_type_str = "Slice DPA"; packet_type_str = "Single NAL"; break;
            case 3:  nal_type_str = "Slice DPB"; packet_type_str = "Single NAL"; break;
            case 4:  nal_type_str = "Slice DPC"; packet_type_str = "Single NAL"; break;
            case 5:  nal_type_str = "IDR Slice"; packet_type_str = "Single NAL (KEY)"; break;
            case 6:  nal_type_str = "SEI"; packet_type_str = "Single NAL"; break;
            case 7:  nal_type_str = "SPS"; packet_type_str = "Single NAL"; break;
            case 8:  nal_type_str = "PPS"; packet_type_str = "Single NAL"; break;
            case 9:  nal_type_str = "AUD"; packet_type_str = "Single NAL"; break;
            case 24: nal_type_str = "STAP-A"; packet_type_str = "Aggregation"; break;
            case 25: nal_type_str = "STAP-B"; packet_type_str = "Aggregation"; break;
            case 26: nal_type_str = "MTAP16"; packet_type_str = "Aggregation"; break;
            case 27: nal_type_str = "MTAP24"; packet_type_str = "Aggregation"; break;
            case 28: nal_type_str = "FU-A"; packet_type_str = "Fragmentation"; break;
            case 29: nal_type_str = "FU-B"; packet_type_str = "Fragmentation"; break;
            default: nal_type_str = "Reserved"; break;
        }
        
        // 详细信息
        if (nal_type == 28) {  // FU-A (分片单元)
            if (payload_size > 1) {
                uint8_t fu_header = payload[1];
                uint8_t fu_start = (fu_header >> 7) & 0x01;
                uint8_t fu_end = (fu_header >> 6) & 0x01;
                uint8_t fu_nal_type = fu_header & 0x1F;
                
                const char *fu_nal_type_str = "Unknown";
                switch (fu_nal_type) {
                    case 1: fu_nal_type_str = "Non-IDR"; break;
                    case 5: fu_nal_type_str = "IDR (KEY)"; break;
                    case 6: fu_nal_type_str = "SEI"; break;
                    case 7: fu_nal_type_str = "SPS"; break;
                    case 8: fu_nal_type_str = "PPS"; break;
                    default: fu_nal_type_str = "Other"; break;
                }
                
                av_log(NULL, AV_LOG_INFO, "%s   📦 H.264 FU-A: NAL=%d (%s), NRI=%d, Start=%d, End=%d, Marker=%d, Payload=%d bytes\n",
                       get_timestamp(), fu_nal_type, fu_nal_type_str, nri, fu_start, fu_end, marker, payload_size - 2);
            }
        } else if (nal_type >= 1 && nal_type <= 23) {  // Single NAL Unit
            av_log(NULL, AV_LOG_INFO, "%s   📦 H.264 Single NAL: Type=%d (%s), NRI=%d, Marker=%d, Payload=%d bytes\n",
                   get_timestamp(), nal_type, nal_type_str, nri, marker, payload_size);
        } else if (nal_type == 24) {  // STAP-A (聚合)
            av_log(NULL, AV_LOG_INFO, "%s   📦 H.264 STAP-A (Aggregation): Marker=%d, Payload=%d bytes\n",
                   get_timestamp(), marker, payload_size);
        } else {
            av_log(NULL, AV_LOG_INFO, "%s   📦 H.264 %s: Type=%d (%s), NRI=%d, Marker=%d, Payload=%d bytes\n",
                   get_timestamp(), packet_type_str, nal_type, nal_type_str, nri, marker, payload_size);
        }
    }
    
    // 重要：拷贝一份 RTP 数据，因为 ff_rtp_parse_packet 可能接管指针所有权
    // 当 RTP 包需要重排序时，enqueue_packet 会保存 buf 指针并稍后释放
    uint8_t *buf_ptr = av_memdup(data, size);
    if (!buf_ptr) {
        av_log(NULL, AV_LOG_ERROR, "%s 拷贝 RTP 缓冲区失败\n", get_timestamp());
        return;
    }
    
    // 调用 ff_rtp_parse_packet 解析 RTP 数据
    // 返回值: 0 = 完整包, 1 = 完整包且还有更多, -1 = 无包/错误
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_ERROR, "%s 分配 AVPacket 失败\n", get_timestamp());
        av_free(buf_ptr);
        return;
    }
    
    int ret = ff_rtp_parse_packet(rtp_demux, pkt, &buf_ptr, size);
    
    // 循环读取所有可用的完整包（返回值为1表示还有更多）
    while (ret >= 0) {
        // 成功组包完成
        av_log(NULL, AV_LOG_INFO, "%s 🎬 视频组包完成: stream=%d, size=%d bytes, pts=%ld, dts=%ld, flags=0x%x%s\n",
               get_timestamp(), pkt->stream_index,
               pkt->size,
               pkt->pts,
               pkt->dts,
               pkt->flags,
               (pkt->flags & AV_PKT_FLAG_KEY) ? " [关键帧]" : "");
        
        // 重置错误计数器（成功组包）
        consecutive_errors = 0;
        
        // TODO: 这里应该将 packet 放入队列供 read_packet 读取
        // 目前只打印信息，稍后实现队列
        
        if (ret == 0) {
            // 没有更多数据了
            break;
        }
        
        // ret == 1，还有更多数据，用 NULL/0 继续读取内部队列
        av_packet_unref(pkt);
        uint8_t *null_ptr = NULL;
        ret = ff_rtp_parse_packet(rtp_demux, pkt, &null_ptr, 0);
    }
    
    if (ret < 0) {
        // 没有完整包（需要更多 RTP 数据，或错误）
        // av_log(NULL, AV_LOG_DEBUG, "%s 视频 RTP 等待更多数据组包 (ret=%d, seq=%u, marker=%d)\n", 
        //        get_timestamp(), ret, seq, marker);
        
        // 连续失败多次（可能丢包太严重）
        consecutive_errors++;
        if (consecutive_errors >= 50) {
            av_log(NULL, AV_LOG_WARNING, "%s ⚠️  视频组包连续失败 %d 次（可能丢包或 RTP demuxer 无法识别分片格式）\n", 
                   get_timestamp(), consecutive_errors);
            av_log(NULL, AV_LOG_WARNING, "%s    最后收到: seq=%u, marker=%d, payload_size=%d, PT=%u\n",
                   get_timestamp(), seq, marker, payload_size, payload_type);
            consecutive_errors = 0;  // 重置计数器
            // 注意：RTCP PLI 由 libdatachannel 自动处理，无需手动发送
        }
    }
    
    // 如果 buf_ptr 不为 NULL，说明 ff_rtp_parse_packet 没有接管所有权，需要我们释放
    if (buf_ptr) {
        av_free(buf_ptr);
    }
    
    av_packet_free(&pkt);
}


/**
 * 初始化 libdatachannel PeerConnection 并使用 Transceiver API 添加 tracks
 */
static int whep_init_peer_connection(AVFormatContext *avctx)
{
    WHEPContext *whep = avctx->priv_data;
    rtcConfiguration config;
    int ret;

    av_log(avctx, AV_LOG_INFO, "%s 初始化 libdatachannel (使用 Transceiver API)...\n", get_timestamp());

    // 使用共享的 RTC logger 初始化函数
    ff_whip_whep_init_rtc_logger();

    // 配置 PeerConnection
    memset(&config, 0, sizeof(config));
    
    // 设置 STUN 服务器（可选）
    const char *ice_servers[] = {
        "stun:stun.l.google.com:19302",
        NULL
    };
    config.iceServers = ice_servers;
    config.iceServersCount = 1;

    // 创建 PeerConnection
    whep->peer_connection = rtcCreatePeerConnection(&config);
    if (whep->peer_connection < 0) {
        av_log(avctx, AV_LOG_ERROR, "%s 创建 PeerConnection 失败\n", get_timestamp());
        return AVERROR_EXTERNAL;
    }

    av_log(avctx, AV_LOG_INFO, "%s PeerConnection 创建成功 (ID: %d)\n", get_timestamp(), whep->peer_connection);

    // 设置回调
    rtcSetStateChangeCallback(whep->peer_connection, on_state_change);
    rtcSetGatheringStateChangeCallback(whep->peer_connection, on_gathering_state_change);
    rtcSetUserPointer(whep->peer_connection, whep);

    // === 使用 rtcAddTrackEx 添加音频 track (只声明接收方向，参数由 SDP 协商) ===
    rtcTrackInit audio_init;
    memset(&audio_init, 0, sizeof(audio_init));
    audio_init.direction = RTC_DIRECTION_RECVONLY;  // 只接收
    audio_init.codec = RTC_CODEC_OPUS;              // 期望 Opus 编码
    audio_init.payloadType = 111;                   // 建议的 PT（可被 Answer 覆盖）
    audio_init.ssrc = 0;                            // SSRC 由服务器指定
    audio_init.mid = "0";                           // Media ID
    audio_init.name = "audio";                      // Track 名称
    
    whep->audio_track = rtcAddTrackEx(whep->peer_connection, &audio_init);
    if (whep->audio_track < 0) {
        av_log(avctx, AV_LOG_ERROR, "%s 添加音频 track 失败: %d\n", get_timestamp(), whep->audio_track);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(avctx, AV_LOG_INFO, "%s 音频 track 添加成功 (ID: %d, direction: recvonly, codec: Opus)\n", 
           get_timestamp(), whep->audio_track);

    // 设置音频 track 回调
    rtcSetOpenCallback(whep->audio_track, on_track_open);
    rtcSetMessageCallback(whep->audio_track, on_audio_message);
    rtcSetUserPointer(whep->audio_track, whep);
    
    // 启用自动 RTCP 接收会话处理
    if (rtcChainRtcpReceivingSession(whep->audio_track) < 0) {
        av_log(avctx, AV_LOG_WARNING, "%s 启用音频 RTCP 接收会话失败\n", get_timestamp());
    } else {
        av_log(avctx, AV_LOG_INFO, "%s 音频 track RTCP 自动处理已启用\n", get_timestamp());
    }

    // === 使用 rtcAddTrackEx 添加视频 track (只声明接收方向，参数由 SDP 协商) ===
    rtcTrackInit video_init;
    memset(&video_init, 0, sizeof(video_init));
    video_init.direction = RTC_DIRECTION_RECVONLY;  // 只接收
    video_init.codec = RTC_CODEC_H264;              // 期望 H.264 编码
    video_init.payloadType = 96;                    // 建议的 PT（可被 Answer 覆盖）
    video_init.ssrc = 0;                            // SSRC 由服务器指定
    video_init.mid = "1";                           // Media ID
    video_init.name = "video";                      // Track 名称
    video_init.profile = "42e01f";                  // H.264 Baseline Level 3.1（建议）
    
    whep->video_track = rtcAddTrackEx(whep->peer_connection, &video_init);
    if (whep->video_track < 0) {
        av_log(avctx, AV_LOG_ERROR, "%s 添加视频 track 失败: %d\n", get_timestamp(), whep->video_track);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(avctx, AV_LOG_INFO, "%s 视频 track 添加成功 (ID: %d, direction: recvonly, codec: H264, profile: %s)\n", 
           get_timestamp(), whep->video_track, video_init.profile);

    // 设置视频 track 回调
    rtcSetOpenCallback(whep->video_track, on_track_open);
    rtcSetMessageCallback(whep->video_track, on_video_message);
    rtcSetUserPointer(whep->video_track, whep);
    
    // 启用自动 RTCP 接收会话处理
    if (rtcChainRtcpReceivingSession(whep->video_track) < 0) {
        av_log(avctx, AV_LOG_WARNING, "%s 启用视频 RTCP 接收会话失败\n", get_timestamp());
    } else {
        av_log(avctx, AV_LOG_INFO, "%s 视频 track RTCP 自动处理已启用\n", get_timestamp());
    }
    
    // 链接 PLI 处理器（当需要关键帧时触发回调）
    if (rtcChainPliHandler(whep->video_track, on_pli_request) < 0) {
        av_log(avctx, AV_LOG_WARNING, "%s 启用视频 PLI 处理器失败\n", get_timestamp());
    } else {
        av_log(avctx, AV_LOG_INFO, "%s 视频 track PLI 处理器已启用\n", get_timestamp());
    }

    av_log(avctx, AV_LOG_INFO, "%s PeerConnection 初始化完成，tracks 已添加，等待 SDP 协商...\n", get_timestamp());
    return 0;

fail:
    if (whep->peer_connection >= 0) {
        rtcDeletePeerConnection(whep->peer_connection);
        whep->peer_connection = -1;
    }
    return ret;
}


/**
 * 向队列中添加packet的辅助函数
 * @param whep WHEP上下文
 * @param pkt 要添加的packet
 * @param is_audio 是否是音频packet（1=音频，0=视频）
 * @return 0表示成功，负值表示错误
 */
static av_unused int whep_queue_packet(WHEPContext *whep, AVPacket *pkt, int is_audio)
{
    AVFifo *queue = is_audio ? whep->audio_queue : whep->video_queue;
    AVPacket *queued_pkt;
    int ret = 0;

    // 分配新的packet
    queued_pkt = av_packet_alloc();
    if (!queued_pkt)
        return AVERROR(ENOMEM);

    // 复制packet数据
    ret = av_packet_ref(queued_pkt, pkt);
    if (ret < 0) {
        av_packet_free(&queued_pkt);
        return ret;
    }

    pthread_mutex_lock(&whep->mutex);

    // 检查队列是否有空间（如果满了可以考虑丢弃或等待）
    if (av_fifo_can_write(queue) == 0) {
        // 队列满了，尝试扩展
        ret = av_fifo_grow2(queue, 1);
        if (ret < 0) {
            pthread_mutex_unlock(&whep->mutex);
            av_packet_free(&queued_pkt);
            return ret;
        }
    }

    // 将packet添加到队列
    av_fifo_write(queue, &queued_pkt, 1);

    // 唤醒等待的读取线程
    pthread_cond_signal(&whep->cond);

    pthread_mutex_unlock(&whep->mutex);

    return 0;
}

static av_cold int whep_read_header(AVFormatContext *avctx)
{
    WHEPContext *whep = avctx->priv_data;
    int ret = 0;

    av_log(avctx, AV_LOG_INFO, "%s WHEP demuxer initializing...\n", get_timestamp());
    
    // 检查URL
    if (!avctx->url || !strlen(avctx->url)) {
        av_log(avctx, AV_LOG_ERROR, "%s WHEP URL not specified\n", get_timestamp());
        return AVERROR(EINVAL);
    }

    whep->whep_url = av_strdup(avctx->url);
    if (!whep->whep_url)
        return AVERROR(ENOMEM);

    av_log(avctx, AV_LOG_INFO, "%s WHEP URL: %s\n", get_timestamp(), whep->whep_url);
    av_log(avctx, AV_LOG_INFO, "%s Timeout: %d ms\n", get_timestamp(), whep->timeout);
    av_log(avctx, AV_LOG_INFO, "%s Buffer size: %d\n", get_timestamp(), whep->buffer_size);
    av_log(avctx, AV_LOG_INFO, "%s Max retry: %d\n", get_timestamp(), whep->max_retry);

    // 初始化队列 (每个packet指针大小)
    whep->audio_queue = av_fifo_alloc2(100, sizeof(AVPacket*), 0);
    whep->video_queue = av_fifo_alloc2(100, sizeof(AVPacket*), 0);
    if (!whep->audio_queue || !whep->video_queue) {
        av_log(avctx, AV_LOG_ERROR, "%s Failed to allocate packet queues\n", get_timestamp());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    // 初始化互斥锁和条件变量
    ret = pthread_mutex_init(&whep->mutex, NULL);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "%s Failed to initialize mutex\n", get_timestamp());
        ret = AVERROR(ret);
        goto fail;
    }

    ret = pthread_cond_init(&whep->cond, NULL);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "%s Failed to initialize condition variable\n", get_timestamp());
        pthread_mutex_destroy(&whep->mutex);
        ret = AVERROR(ret);
        goto fail;
    }

    whep->abort_request = 0;
    whep->eof_reached = 0;
    whep->peer_connection = -1;
    whep->audio_track = -1;
    whep->video_track = -1;
    
    // 初始化 SSRC 字段
    whep->audio_ssrc = 0;
    whep->video_ssrc = 0;

    // === WHEP 流程：初始化 WebRTC 并交换 SDP ===
    
    // 1. 初始化 PeerConnection 并添加 tracks
    ret = whep_init_peer_connection(avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "%s 初始化 PeerConnection 失败\n", get_timestamp());
        goto fail;
    }

    // 2. 使用共享函数交换 SDP 并设置远端描述
    ret = ff_whip_whep_exchange_and_set_sdp(avctx, whep->peer_connection, whep->token, &whep->resource_url);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "%s SDP 交换失败\n", get_timestamp());
        goto fail;
    }

    av_log(avctx, AV_LOG_INFO, "%s WHEP 信令交互完成，等待 WebRTC 连接建立...\n", get_timestamp());

    // 3. 获取 remote description (SDP answer)，解析并动态创建流，初始化 RTP demuxer
    char sdp_answer[8192];
    int sdp_len = rtcGetRemoteDescription(whep->peer_connection, sdp_answer, sizeof(sdp_answer));
    if (sdp_len > 0) {
        sdp_answer[sdp_len] = '\0';
        av_log(avctx, AV_LOG_DEBUG, "%s 获取到 SDP answer (%d bytes)\n", get_timestamp(), sdp_len);
        
        ret = whep_parse_sdp_and_init_rtp(avctx, whep, sdp_answer);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "%s 解析 SDP 并初始化 RTP demuxer 失败\n", get_timestamp());
            goto fail;
        }
    } else {
        av_log(avctx, AV_LOG_WARNING, "%s 无法获取 remote description\n", get_timestamp());
    }

    whep->initialized = 1;
    whep->start_time = av_gettime_relative();

    av_log(avctx, AV_LOG_INFO, "%s WHEP demuxer initialized successfully\n", get_timestamp());
    
    return 0;

fail:
    if (whep->audio_queue)
        av_fifo_freep2(&whep->audio_queue);
    if (whep->video_queue)
        av_fifo_freep2(&whep->video_queue);
    av_freep(&whep->whep_url);
    return ret;
}

static int whep_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    WHEPContext *whep = avctx->priv_data;
    AVPacket *queued_pkt = NULL;
    int ret = 0;

    if (!whep->initialized) {
        av_log(avctx, AV_LOG_ERROR, "%s WHEP not initialized\n", get_timestamp());
        return AVERROR(EINVAL);
    }

    pthread_mutex_lock(&whep->mutex);

    // 等待队列中有数据或者收到终止/EOF信号
    while (!whep->abort_request && !whep->eof_reached &&
           av_fifo_can_read(whep->audio_queue) == 0 && 
           av_fifo_can_read(whep->video_queue) == 0) {
        av_log(avctx, AV_LOG_DEBUG, "%s Waiting for packet data...\n", get_timestamp());
        pthread_cond_wait(&whep->cond, &whep->mutex);
    }

    // 检查是否需要终止
    if (whep->abort_request) {
        pthread_mutex_unlock(&whep->mutex);
        return AVERROR_EXIT;
    }

    // 检查是否到达EOF且队列为空
    if (whep->eof_reached && 
        av_fifo_can_read(whep->audio_queue) == 0 && 
        av_fifo_can_read(whep->video_queue) == 0) {
        pthread_mutex_unlock(&whep->mutex);
        return AVERROR_EOF;
    }

    // 优先从视频队列读取（可以根据策略调整）
    // 这里实现简单的交错读取策略
    if (av_fifo_can_read(whep->video_queue) > 0) {
        av_fifo_read(whep->video_queue, &queued_pkt, 1);
        av_log(avctx, AV_LOG_DEBUG, "%s Read video packet from queue\n", get_timestamp());
    } else if (av_fifo_can_read(whep->audio_queue) > 0) {
        av_fifo_read(whep->audio_queue, &queued_pkt, 1);
        av_log(avctx, AV_LOG_DEBUG, "%s Read audio packet from queue\n", get_timestamp());
    }

    pthread_mutex_unlock(&whep->mutex);

    if (queued_pkt) {
        // 将队列中的packet移动到输出packet
        av_packet_move_ref(pkt, queued_pkt);
        av_packet_free(&queued_pkt);
        ret = 0;
    } else {
        // 不应该到这里，但为了安全性
        ret = AVERROR(EAGAIN);
    }

    return ret;
}

static av_cold int whep_read_close(AVFormatContext *avctx)
{
    WHEPContext *whep = avctx->priv_data;
    AVPacket *pkt;

    av_log(avctx, AV_LOG_INFO, "%s WHEP demuxer closing...\n", get_timestamp());

    // 设置终止标志并唤醒可能在等待的线程
    pthread_mutex_lock(&whep->mutex);
    whep->abort_request = 1;
    pthread_cond_broadcast(&whep->cond);
    pthread_mutex_unlock(&whep->mutex);

    // 清理所有 RTP demuxer
    for (int i = 0; i < 128; i++) {
        if (whep->rtp_demux[i]) {
            ff_rtp_parse_close(whep->rtp_demux[i]);
            whep->rtp_demux[i] = NULL;
        }
    }
    
    // 使用共享函数删除 WHEP 会话
    if (whep->resource_url) {
        av_log(avctx, AV_LOG_INFO, "%s 删除 WHEP 会话...\n", get_timestamp());
        ff_whip_whep_delete_session(avctx, whep->token, whep->resource_url);
    }

    // 关闭 PeerConnection
    if (whep->peer_connection >= 0) {
        av_log(avctx, AV_LOG_INFO, "%s 关闭 PeerConnection...\n", get_timestamp());
        rtcDeletePeerConnection(whep->peer_connection);
        whep->peer_connection = -1;
    }

    // 清理 libdatachannel
    rtcCleanup();

    // 清空音频队列
    if (whep->audio_queue) {
        while (av_fifo_can_read(whep->audio_queue) > 0) {
            av_fifo_read(whep->audio_queue, &pkt, 1);
            av_packet_free(&pkt);
        }
        av_fifo_freep2(&whep->audio_queue);
    }

    // 清空视频队列
    if (whep->video_queue) {
        while (av_fifo_can_read(whep->video_queue) > 0) {
            av_fifo_read(whep->video_queue, &pkt, 1);
            av_packet_free(&pkt);
        }
        av_fifo_freep2(&whep->video_queue);
    }

    // 销毁同步机制
    pthread_cond_destroy(&whep->cond);
    pthread_mutex_destroy(&whep->mutex);

    // 清理内存
    av_freep(&whep->whep_url);
    av_freep(&whep->token);
    av_freep(&whep->resource_url);

    whep->initialized = 0;

    av_log(avctx, AV_LOG_INFO, "%s WHEP demuxer closed\n", get_timestamp());
    
    return 0;
}

static int whep_read_seek(AVFormatContext *avctx, int stream_index,
                          int64_t timestamp, int flags)
{
    av_log(avctx, AV_LOG_WARNING, "%s WHEP does not support seeking\n", get_timestamp());
    return AVERROR(ENOSYS);
}

static int whep_read_pause(AVFormatContext *avctx)
{
    av_log(avctx, AV_LOG_INFO, "%s WHEP pause requested\n", get_timestamp());
    // 暂停逻辑占位符
    return 0;
}

static int whep_read_play(AVFormatContext *avctx)
{
    av_log(avctx, AV_LOG_INFO, "%s WHEP play requested\n", get_timestamp());
    // 播放逻辑占位符
    return 0;
}

#define OFFSET(x) offsetof(WHEPContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption whep_options[] = {
    { "timeout", "Connection timeout in milliseconds", OFFSET(timeout), AV_OPT_TYPE_INT, {.i64 = 5000}, 0, INT_MAX, DEC },
    { "buffer_size", "Buffer size for incoming packets", OFFSET(buffer_size), AV_OPT_TYPE_INT, {.i64 = 1024*1024}, 0, INT_MAX, DEC },
    { "max_retry", "Maximum number of reconnection attempts", OFFSET(max_retry), AV_OPT_TYPE_INT, {.i64 = 3}, 0, 100, DEC },
    { "token", "Bearer token for authentication", OFFSET(token), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, DEC },
    { NULL },
};

static const AVClass whep_class = {
    .class_name = "whep demuxer",
    .item_name  = av_default_item_name,
    .option     = whep_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEMUXER,
};

const FFInputFormat ff_whep_demuxer = {
    .p.name         = "whep",
    .p.long_name    = NULL_IF_CONFIG_SMALL("WHEP (WebRTC HTTP Egress Protocol)"),
    .p.flags        = AVFMT_NOFILE,
    .p.priv_class   = &whep_class,
    .priv_data_size = sizeof(WHEPContext),
    .read_header    = whep_read_header,
    .read_packet    = whep_read_packet,
    .read_close     = whep_read_close,
    .read_seek      = whep_read_seek,
    .read_pause     = whep_read_pause,
    .read_play      = whep_read_play,
};

