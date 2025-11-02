/*
 * WHEP (WebRTC HTTP Egress Protocol) Demuxer
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

/**
 * @file
 * WHEP (WebRTC HTTP Egress Protocol) demuxer
 * Based on metaRTC libmetartccore7
 * 
 * 使用 SRS 专用 WebRTC 协议从 SRS 流媒体服务器拉取流
 * 支持的 URL 格式:
 *   - webrtc://host:port/app/stream  (SRS 专用格式，推荐)
 *   - whep://host:port/endpoint      (标准 WHEP 协议)
 *   - http://host:port/rtc/v1/play/  (SRS HTTP API)
 */

#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "libavcodec/avcodec.h"

#include <pthread.h>
#include <time.h>

/* metaRTC headers */
#include <yangrtc/YangPeerConnection.h>
#include <yangrtc/YangPeerInfo.h>
#include <yangrtc/YangWhip.h>
#include <yangutil/yangavinfo.h>
#include <yangssl/YangOpenssl.h>
#include <yangutil/yangtype.h>
#include <yangutil/sys/YangLog.h>

#define MAX_QUEUE_SIZE 100

/* 音视频帧队列 */
typedef struct FrameQueue {
    YangFrame *frames[MAX_QUEUE_SIZE];
    int read_idx;
    int write_idx;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} FrameQueue;

/* WHEP 上下文 */
typedef struct WHEPContext {
    const AVClass *class;
    
    /* 用户配置选项 */
    char *url;                      /* WHEP 服务器 URL */
    int timeout;                    /* 连接超时（秒） */
    char *ice_servers;              /* ICE 服务器配置 */
    int audio_enabled;              /* 是否启用音频 */
    int video_enabled;              /* 是否启用视频 */
    
    /* metaRTC 组件 */
    YangPeerConnection peer_conn;
    YangAVInfo av_info;
    
    /* 状态管理 */
    int connected;
    int eof;
    int64_t start_time;
    
    /* 音视频流 */
    AVStream *audio_stream;
    AVStream *video_stream;
    
    /* 帧队列 */
    FrameQueue audio_queue;
    FrameQueue video_queue;
    
    /* 编解码器信息 */
    YangAudioCodec audio_codec;
    YangVideoCodec video_codec;
    int audio_sample_rate;
    int audio_channels;
    int video_width;
    int video_height;
    int video_fps;
    
} WHEPContext;

/* ==================== 帧队列管理 ==================== */

static void frame_queue_init(FrameQueue *q)
{
    memset(q, 0, sizeof(FrameQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void frame_queue_destroy(FrameQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    for (int i = 0; i < q->size; i++) {
        int idx = (q->read_idx + i) % MAX_QUEUE_SIZE;
        if (q->frames[idx]) {
            yang_free(q->frames[idx]->payload);
            yang_free(q->frames[idx]);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int frame_queue_push(FrameQueue *q, YangFrame *frame)
{
    pthread_mutex_lock(&q->mutex);
    
    if (q->size >= MAX_QUEUE_SIZE) {
        pthread_mutex_unlock(&q->mutex);
        return -1; /* 队列满 */
    }
    
    /* 复制帧数据 */
    YangFrame *new_frame = (YangFrame *)malloc(sizeof(YangFrame));
    if (!new_frame) {
        pthread_mutex_unlock(&q->mutex);
        return AVERROR(ENOMEM);
    }
    
    memcpy(new_frame, frame, sizeof(YangFrame));
    
    /* 复制 payload */
    new_frame->payload = (uint8_t *)malloc(frame->nb);
    if (!new_frame->payload) {
        free(new_frame);
        pthread_mutex_unlock(&q->mutex);
        return AVERROR(ENOMEM);
    }
    memcpy(new_frame->payload, frame->payload, frame->nb);
    
    q->frames[q->write_idx] = new_frame;
    q->write_idx = (q->write_idx + 1) % MAX_QUEUE_SIZE;
    q->size++;
    
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    
    return 0;
}

static YangFrame *frame_queue_pop(FrameQueue *q, int timeout_ms)
{
    pthread_mutex_lock(&q->mutex);
    
    if (q->size == 0 && timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&q->cond, &q->mutex, &ts);
    }
    
    if (q->size == 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    
    YangFrame *frame = q->frames[q->read_idx];
    q->read_idx = (q->read_idx + 1) % MAX_QUEUE_SIZE;
    q->size--;
    
    pthread_mutex_unlock(&q->mutex);
    
    return frame;
}

/* ==================== metaRTC 回调函数 ==================== */

/* 音频接收回调（YangReceiveCallback） */
static void on_audio_frame(void *context, YangFrame *audioFrame)
{
    if (!context || !audioFrame)
        return;
    
    WHEPContext *whep = (WHEPContext *)context;
    frame_queue_push(&whep->audio_queue, audioFrame);
}

/* 视频接收回调（YangReceiveCallback） */
static void on_video_frame(void *context, YangFrame *videoFrame)
{
    if (!context || !videoFrame)
        return;
    
    WHEPContext *whep = (WHEPContext *)context;
    frame_queue_push(&whep->video_queue, videoFrame);
}

static void on_connection_state_change(void *context, int32_t uid, YangRtcConnectionState state)
{
    WHEPContext *whep = (WHEPContext *)context;
    
    switch (state) {
    case Yang_Conn_State_Connected:
        av_log(whep, AV_LOG_INFO, "WHEP 连接成功\n");
        whep->connected = 1;
        break;
    case Yang_Conn_State_Disconnected:
    case Yang_Conn_State_Failed:
    case Yang_Conn_State_Closed:
        av_log(whep, AV_LOG_WARNING, "WHEP 连接断开 (state=%d)\n", state);
        whep->connected = 0;
        whep->eof = 1;
        break;
    default:
        break;
    }
}

static void on_media_config(void *context, int32_t puid, YangAudioParam *audio, YangVideoParam *video)
{
    WHEPContext *whep = (WHEPContext *)context;
    
    if (audio && whep->audio_enabled) {
        whep->audio_codec = audio->encode;
        whep->audio_sample_rate = audio->sample;
        whep->audio_channels = audio->channel;
        av_log(whep, AV_LOG_INFO, "音频配置: codec=%d, sample_rate=%d, channels=%d\n",
               audio->encode, audio->sample, audio->channel);
    }
    
    if (video && whep->video_enabled) {
        whep->video_codec = video->encode;
        av_log(whep, AV_LOG_INFO, "视频配置: codec=%d\n", video->encode);
    }
}

/* ==================== FFmpeg 解复用器实现 ==================== */

static int whep_probe(const AVProbeData *p)
{
    /* WHEP URL 格式: http(s)://host:port/whep/stream_id */
    if (av_stristart(p->filename, "whep://", NULL) ||
        av_stristart(p->filename, "wheps://", NULL) ||
        (av_stristart(p->filename, "http://", NULL) && strstr(p->filename, "/whep/")) ||
        (av_stristart(p->filename, "https://", NULL) && strstr(p->filename, "/whep/")))
        return AVPROBE_SCORE_MAX;

    return 0;
}

static enum AVCodecID yang_audio_codec_to_ffmpeg(YangAudioCodec codec)
{
    switch (codec) {
    case Yang_AED_OPUS:
        return AV_CODEC_ID_OPUS;
    case Yang_AED_AAC:
        return AV_CODEC_ID_AAC;
    case Yang_AED_PCMA:
        return AV_CODEC_ID_PCM_ALAW;
    case Yang_AED_PCMU:
        return AV_CODEC_ID_PCM_MULAW;
    default:
        return AV_CODEC_ID_NONE;
    }
}

static enum AVCodecID yang_video_codec_to_ffmpeg(YangVideoCodec codec)
{
    switch (codec) {
    case Yang_VED_H264:
        return AV_CODEC_ID_H264;
    case Yang_VED_H265:
        return AV_CODEC_ID_HEVC;
    case Yang_VED_VP8:
        return AV_CODEC_ID_VP8;
    case Yang_VED_VP9:
        return AV_CODEC_ID_VP9;
    case Yang_VED_AV1:
        return AV_CODEC_ID_AV1;
    default:
        return AV_CODEC_ID_NONE;
    }
}

static int whep_read_header(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;
    int ret;
    
    av_log(s, AV_LOG_INFO, "WHEP 解复用器初始化: %s\n", s->url);
    
    /* 设置 metaRTC 日志级别 */
    if (av_log_get_level() >= AV_LOG_DEBUG) {
        yang_setLogLevel(YANG_LOG_DEBUG);
    } else if (av_log_get_level() >= AV_LOG_INFO) {
        yang_setLogLevel(YANG_LOG_INFO);
    } else {
        yang_setLogLevel(YANG_LOG_ERROR);
    }
    
    /* 初始化帧队列 */
    frame_queue_init(&whep->audio_queue);
    frame_queue_init(&whep->video_queue);
    
    /* 初始化 YangAVInfo */
    memset(&whep->av_info, 0, sizeof(YangAVInfo));
    
    /* 设置基本系统参数 */
    whep->av_info.sys.familyType = Yang_IpFamilyType_IPV4; /* 默认 IPv4 */
    whep->av_info.sys.mediaServer = Yang_Server_Srs; /* 使用 SRS 专用协议 */
    
    /* 配置 URL (支持多种格式) */
    if (s->url) {
        /* SRS WebRTC 协议格式: webrtc://host:port/app/stream */
        if (av_stristart(s->url, "webrtc://", NULL)) {
            /* SRS 专用格式，直接使用 */
            av_strlcpy(whep->av_info.sys.whepUrl, s->url, sizeof(whep->av_info.sys.whepUrl));
        }
        /* HTTP WHEP URL 格式: http://host:port/rtc/v1/whep/?app=xxx&stream=yyy */
        else if (av_stristart(s->url, "http://", NULL) || av_stristart(s->url, "https://", NULL)) {
            /* 检查是否包含 app 和 stream 参数 */
            const char *app_param = strstr(s->url, "app=");
            const char *stream_param = strstr(s->url, "stream=");
            
            if (app_param && stream_param) {
                char hostname[256] = {0};
                char app[128] = {0};
                char stream[128] = {0};
                int port = 1985; /* 默认 SRS 端口 */
                
                /* 解析主机名和端口 */
                const char *url_start = strchr(s->url, ':') + 3; /* 跳过 "http://" */
                const char *port_sep = strchr(url_start, ':');
                const char *path_sep = strchr(url_start, '/');
                
                if (port_sep && path_sep && port_sep < path_sep) {
                    /* 有端口号 */
                    size_t host_len = port_sep - url_start;
                    if (host_len > sizeof(hostname) - 1) host_len = sizeof(hostname) - 1;
                    memcpy(hostname, url_start, host_len);
                    hostname[host_len] = '\0';
                    port = atoi(port_sep + 1);
                } else if (path_sep) {
                    /* 没有端口号 */
                    size_t host_len = path_sep - url_start;
                    if (host_len > sizeof(hostname) - 1) host_len = sizeof(hostname) - 1;
                    memcpy(hostname, url_start, host_len);
                    hostname[host_len] = '\0';
                }
                
                /* 解析 app 参数 */
                const char *app_start = app_param + 4; /* 跳过 "app=" */
                const char *app_end = strchr(app_start, '&');
                if (!app_end) app_end = app_start + strlen(app_start);
                size_t app_len = app_end - app_start;
                if (app_len > sizeof(app) - 1) app_len = sizeof(app) - 1;
                memcpy(app, app_start, app_len);
                app[app_len] = '\0';
                
                /* 解析 stream 参数 */
                const char *stream_start = stream_param + 7; /* 跳过 "stream=" */
                const char *stream_end = strchr(stream_start, '&');
                if (!stream_end) stream_end = stream_start + strlen(stream_start);
                size_t stream_len = stream_end - stream_start;
                if (stream_len > sizeof(stream) - 1) stream_len = sizeof(stream) - 1;
                memcpy(stream, stream_start, stream_len);
                stream[stream_len] = '\0';
                
                /* 构造 webrtc:// URL */
                snprintf(whep->av_info.sys.whepUrl, sizeof(whep->av_info.sys.whepUrl),
                         "webrtc://%s:%d/%s/%s", hostname, port, app, stream);
                
                av_log(s, AV_LOG_INFO, "从 HTTP URL 转换为 SRS WebRTC URL: %s\n", whep->av_info.sys.whepUrl);
            } else {
                /* 不是 SRS 格式的 WHEP URL，直接使用 */
                av_strlcpy(whep->av_info.sys.whepUrl, s->url, sizeof(whep->av_info.sys.whepUrl));
            }
        }
        /* 标准 WHEP 格式: whep://host:port/path -> http://host:port/path */
        else if (av_stristart(s->url, "whep://", NULL)) {
            snprintf(whep->av_info.sys.whepUrl, sizeof(whep->av_info.sys.whepUrl),
                     "http://%s", s->url + 7);
        } 
        /* 安全 WHEP: wheps://host:port/path -> https://host:port/path */
        else if (av_stristart(s->url, "wheps://", NULL)) {
            snprintf(whep->av_info.sys.whepUrl, sizeof(whep->av_info.sys.whepUrl),
                     "https://%s", s->url + 8);
        } 
        /* 其他格式，直接使用 */
        else {
            av_strlcpy(whep->av_info.sys.whepUrl, s->url, sizeof(whep->av_info.sys.whepUrl));
        }
    }
    
    /* 配置 RTC 参数 */
    whep->av_info.rtc.sessionTimeout = whep->timeout * 1000; /* 转换为毫秒 */
    whep->av_info.rtc.iceCandidateType = YangIceHost; /* 默认 host 候选 */
    whep->av_info.rtc.rtcLocalPort = 16000; /* 本地 RTC 端口 */
    whep->av_info.rtc.rtcSocketProtocol = Yang_Socket_Protocol_Udp; /* UDP 传输 */
    
    /* 初始化整个 PeerConnection 结构体 */
    memset(&whep->peer_conn, 0, sizeof(YangPeerConnection));
    
    /* 使用 yang_avinfo_initPeerInfo 初始化 PeerInfo（按官方示例） */
    yang_avinfo_initPeerInfo(&whep->peer_conn.peer.peerInfo, &whep->av_info);
    whep->peer_conn.peer.peerInfo.direction = YangRecvonly;
    
    /* 设置接收回调（按官方示例使用 recvCallback） */
    whep->peer_conn.peer.peerCallback.recvCallback.context = whep;
    whep->peer_conn.peer.peerCallback.recvCallback.receiveAudio = on_audio_frame;
    whep->peer_conn.peer.peerCallback.recvCallback.receiveVideo = on_video_frame;
    
    /* 设置状态回调 */
    whep->peer_conn.peer.peerCallback.iceCallback.context = whep;
    whep->peer_conn.peer.peerCallback.iceCallback.onConnectionStateChange = on_connection_state_change;
    
    whep->peer_conn.peer.peerCallback.rtcCallback.context = whep;
    whep->peer_conn.peer.peerCallback.rtcCallback.setMediaConfig = on_media_config;
    
    /* 手动初始化全局 SRTP（确保 libsrtp 库已初始化） */
    g_yang_create_srtp();
    av_log(s, AV_LOG_INFO, "全局 SRTP 已初始化\n");
    
    /* 创建 PeerConnection（会自动初始化 Peer、RTC context、DTLS、SRTP 等） */
    yang_create_peerConnection(&whep->peer_conn);
    
    /* 添加音频轨道（接收） */
    if (whep->audio_enabled) {
        ret = whep->peer_conn.addAudioTrack(&whep->peer_conn.peer, Yang_AED_OPUS);
    if (ret < 0) {
            av_log(s, AV_LOG_WARNING, "添加音频轨道失败: %d\n", ret);
        } else {
            whep->peer_conn.addTransceiver(&whep->peer_conn.peer, YangMediaAudio, YangRecvonly);
        }
    }
    
    /* 添加视频轨道（接收） */
    if (whep->video_enabled) {
        ret = whep->peer_conn.addVideoTrack(&whep->peer_conn.peer, Yang_VED_H264);
    if (ret < 0) {
            av_log(s, AV_LOG_WARNING, "添加视频轨道失败: %d\n", ret);
        } else {
            whep->peer_conn.addTransceiver(&whep->peer_conn.peer, YangMediaVideo, YangRecvonly);
        }
    }
    
    /* 连接 SRS 服务器 (使用专用协议) */
    av_log(s, AV_LOG_INFO, "连接 SRS WebRTC 服务器: %s\n", whep->av_info.sys.whepUrl);
    
    /* yang_whip_connectSfuServer 使用 SRS 专用 API 连接
     * 内部会创建 Offer、设置本地描述、
     * 通过 SRS 专用 HTTP API (/rtc/v1/play/) 发送、
     * 接收 Answer 并设置远端描述 */
    ret = yang_whip_connectSfuServer(&whep->peer_conn.peer, 
                                      whep->av_info.sys.whepUrl, 
                                      Yang_Server_Srs);
    
    av_log(s, AV_LOG_DEBUG, "yang_whip_connectSfuServer 返回值: %d (Yang_Ok=%d)\n", ret, Yang_Ok);
    
    if (ret != Yang_Ok) {
        av_log(s, AV_LOG_ERROR, "SRS WebRTC 信令交换失败: %d\n", ret);
        return AVERROR(EIO);
    }
    
    av_log(s, AV_LOG_INFO, "SRS WebRTC 信令交换成功，等待连接建立（ICE+DTLS）\n");
    
    /* 等待连接建立（最多 timeout 秒） */
    int wait_time = 0;
    YangRtcConnectionState conn_state = Yang_Conn_State_New;
    
    while (wait_time < whep->timeout * 10) {
        av_usleep(100000); /* 100ms */
        wait_time++;
        
        /* 查询连接状态 */
        conn_state = whep->peer_conn.getConnectionState(&whep->peer_conn.peer);
        
        av_log(s, AV_LOG_DEBUG, "连接状态: %d (0=New, 1=Connecting, 2=Connected, 3=Disconnected, 4=Failed, 5=Closed)\n", conn_state);
        
        if (conn_state == Yang_Conn_State_Connected) {
            av_log(s, AV_LOG_INFO, "✅ SRS WebRTC 连接已建立 (DTLS 握手完成，SRTP 已就绪)\n");
            whep->connected = 1;
            break;
        }
        
        if (conn_state == Yang_Conn_State_Failed || conn_state == Yang_Conn_State_Closed) {
            av_log(s, AV_LOG_ERROR, "❌ SRS WebRTC 连接失败，状态: %d\n", conn_state);
            return AVERROR(EIO);
        }
        
        if (whep->eof) {
            av_log(s, AV_LOG_ERROR, "连接失败\n");
            return AVERROR(EIO);
        }
    }
    
    if (!whep->connected) {
        av_log(s, AV_LOG_ERROR, "连接超时，最终状态: %d\n", conn_state);
        return AVERROR(ETIMEDOUT);
    }
    
    /* 创建音频流 */
    if (whep->audio_enabled) {
        whep->audio_stream = avformat_new_stream(s, NULL);
        if (!whep->audio_stream)
            return AVERROR(ENOMEM);
        
        whep->audio_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        whep->audio_stream->codecpar->codec_id = yang_audio_codec_to_ffmpeg(whep->audio_codec);
        whep->audio_stream->codecpar->sample_rate = whep->audio_sample_rate ? whep->audio_sample_rate : 48000;
        whep->audio_stream->codecpar->ch_layout = whep->audio_channels == 2 ? 
                                                   (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO :
                                                   (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        
        avpriv_set_pts_info(whep->audio_stream, 64, 1, whep->audio_stream->codecpar->sample_rate);
    }
    
    /* 创建视频流 */
    if (whep->video_enabled) {
        whep->video_stream = avformat_new_stream(s, NULL);
        if (!whep->video_stream)
            return AVERROR(ENOMEM);
        
        whep->video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        whep->video_stream->codecpar->codec_id = yang_video_codec_to_ffmpeg(whep->video_codec);
        
        if (whep->video_width > 0 && whep->video_height > 0) {
            whep->video_stream->codecpar->width = whep->video_width;
            whep->video_stream->codecpar->height = whep->video_height;
        }
        
        avpriv_set_pts_info(whep->video_stream, 64, 1, 90000); /* RTP 视频时间戳 90kHz */
    }
    
    whep->start_time = av_gettime();
    
    av_log(s, AV_LOG_INFO, "WHEP 连接成功，开始接收流\n");
    
    return 0;
}

static int whep_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    WHEPContext *whep = s->priv_data;
    YangFrame *frame = NULL;
    int ret;
    
    if (whep->eof)
        return AVERROR_EOF;
    
    /* 优先读取视频帧 */
    if (whep->video_enabled) {
        frame = frame_queue_pop(&whep->video_queue, 10); /* 10ms 超时 */
        if (frame) {
            ret = av_new_packet(pkt, frame->nb);
            if (ret < 0) {
                yang_free(frame->payload);
                yang_free(frame);
    return ret;
}

            memcpy(pkt->data, frame->payload, frame->nb);
            pkt->stream_index = whep->video_stream->index;
            pkt->pts = frame->pts;
            pkt->dts = frame->dts;
            
            if (frame->frametype == YANG_Frametype_I || frame->frametype == YANG_Frametype_Spspps) {
                pkt->flags |= AV_PKT_FLAG_KEY;
            }
            
            yang_free(frame->payload);
            yang_free(frame);
            
            return 0;
        }
    }
    
    /* 读取音频帧 */
    if (whep->audio_enabled) {
        frame = frame_queue_pop(&whep->audio_queue, 100); /* 100ms 超时 */
        if (frame) {
            ret = av_new_packet(pkt, frame->nb);
            if (ret < 0) {
                yang_free(frame->payload);
                yang_free(frame);
                return ret;
            }
            
            memcpy(pkt->data, frame->payload, frame->nb);
            pkt->stream_index = whep->audio_stream->index;
            pkt->pts = frame->pts;
            pkt->dts = frame->pts;
            pkt->flags |= AV_PKT_FLAG_KEY; /* 音频帧都是关键帧 */
            
            yang_free(frame->payload);
            yang_free(frame);
            
            return 0;
        }
    }
    
    /* 检查连接状态 */
    if (!whep->peer_conn.isConnected(&whep->peer_conn.peer)) {
        av_log(s, AV_LOG_WARNING, "连接已断开\n");
        whep->eof = 1;
        return AVERROR_EOF;
    }
    
    /* 没有数据可读 */
    return AVERROR(EAGAIN);
}

static int whep_read_close(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;
    
    av_log(s, AV_LOG_INFO, "关闭 WHEP 连接\n");
    
    /* 关闭 PeerConnection */
    if (whep->peer_conn.close) {
        whep->peer_conn.close(&whep->peer_conn.peer);
    }
    
    /* 销毁 PeerConnection */
    yang_destroy_peerConnection(&whep->peer_conn);
    
    /* 清理帧队列 */
    frame_queue_destroy(&whep->audio_queue);
    frame_queue_destroy(&whep->video_queue);
    
    /* 销毁全局 SRTP */
    g_yang_destroy_srtp();
    av_log(s, AV_LOG_DEBUG, "全局 SRTP 已清理\n");
    
    return 0;
}

/* ==================== AVOptions ==================== */

#define OFFSET(x) offsetof(WHEPContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption whep_options[] = {
    { "timeout", "连接超时（秒）", OFFSET(timeout), AV_OPT_TYPE_INT, {.i64 = 10}, 1, 60, DEC },
    { "ice_servers", "ICE 服务器配置", OFFSET(ice_servers), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, DEC },
    { "audio", "启用音频", OFFSET(audio_enabled), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, DEC },
    { "video", "启用视频", OFFSET(video_enabled), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, DEC },
    { NULL }
};

static const AVClass whep_demuxer_class = {
    .class_name = "WHEP demuxer",
    .item_name  = av_default_item_name,
    .option     = whep_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEMUXER,
};

/* ==================== 注册解复用器 ==================== */

const FFInputFormat ff_whep_demuxer = {
    .p.name         = "whep",
    .p.long_name    = NULL_IF_CONFIG_SMALL("SRS WebRTC / WHEP Protocol (via metaRTC)"),
    .p.flags        = AVFMT_NOFILE | AVFMT_NOGENSEARCH,
    .p.priv_class   = &whep_demuxer_class,
    .priv_data_size = sizeof(WHEPContext),
    .read_probe     = whep_probe,
    .read_header    = whep_read_header,
    .read_packet    = whep_read_packet,
    .read_close     = whep_read_close,
};

