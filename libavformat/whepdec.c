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

typedef struct WHEPContext {
    AVClass *class;
    char *whep_url;              ///< WHEP server URL
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
    char *local_sdp;             ///< 本地 SDP offer
    char *remote_sdp;            ///< 远端 SDP answer
    char *resource_url;          ///< WHEP 资源 URL (用于DELETE)
} WHEPContext;

/**
 * libdatachannel 本地描述回调
 */
static void on_local_description(int pc, const char *sdp, const char *type, void *user_ptr)
{
    WHEPContext *whep = (WHEPContext *)user_ptr;
    
    av_log(NULL, AV_LOG_INFO, "本地 SDP %s 生成:\n%s\n", type, sdp);
    
    // 保存本地 SDP
    av_freep(&whep->local_sdp);
    whep->local_sdp = av_strdup(sdp);
}

/**
 * libdatachannel 状态改变回调
 */
static void on_state_change(int pc, rtcState state, void *user_ptr)
{
    const char *state_str[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
    av_log(NULL, AV_LOG_INFO, "PeerConnection 状态变更: %s\n", 
           state < 6 ? state_str[state] : "Unknown");
}

/**
 * libdatachannel gathering 状态回调
 */
static void on_gathering_state_change(int pc, rtcGatheringState state, void *user_ptr)
{
    const char *state_str[] = {"New", "InProgress", "Complete"};
    av_log(NULL, AV_LOG_INFO, "ICE Gathering 状态: %s\n", 
           state < 3 ? state_str[state] : "Unknown");
}

/**
 * 通过 HTTP POST 发送 SDP offer 到 WHEP 服务器
 * @return 0表示成功，负值表示错误
 */
static int whep_exchange_sdp(AVFormatContext *avctx)
{
    WHEPContext *whep = avctx->priv_data;
    AVIOContext *avio_ctx = NULL;
    AVDictionary *options = NULL;
    AVBPrint response;
    char *headers = NULL;
    uint8_t buf[4096];
    int ret = 0;
    int read_size;

    if (!whep->local_sdp) {
        av_log(avctx, AV_LOG_ERROR, "本地 SDP 未生成\n");
        return AVERROR(EINVAL);
    }

    av_log(avctx, AV_LOG_INFO, "发送 WHEP POST 请求到: %s\n", whep->whep_url);

    // 构建 HTTP headers
    headers = av_asprintf(
        "Content-Type: application/sdp\r\n"
        "Content-Length: %zu\r\n",
        strlen(whep->local_sdp)
    );
    if (!headers)
        return AVERROR(ENOMEM);

    av_dict_set(&options, "method", "POST", 0);
    av_dict_set(&options, "headers", headers, 0);
    av_dict_set_int(&options, "timeout", whep->timeout * 1000, 0);

    // 打开 HTTP 连接
    ret = avio_open2(&avio_ctx, whep->whep_url, AVIO_FLAG_READ_WRITE, 
                     &avctx->interrupt_callback, &options);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "无法连接到 WHEP 服务器: %s\n", av_err2str(ret));
        goto cleanup;
    }

    // 发送 SDP offer
    avio_write(avio_ctx, (const unsigned char *)whep->local_sdp, strlen(whep->local_sdp));
    avio_flush(avio_ctx);

    av_log(avctx, AV_LOG_INFO, "SDP offer 已发送，等待服务器响应...\n");

    // 读取响应
    av_bprint_init(&response, 0, AV_BPRINT_SIZE_UNLIMITED);
    while ((read_size = avio_read(avio_ctx, buf, sizeof(buf))) > 0) {
        av_bprint_append_data(&response, (const char *)buf, read_size);
    }

    if (!av_bprint_is_complete(&response)) {
        av_log(avctx, AV_LOG_ERROR, "响应数据过大\n");
        ret = AVERROR(ENOMEM);
        av_bprint_finalize(&response, NULL);
        goto cleanup;
    }

    // 检查 HTTP 状态码
    int http_code = 0;
    if (avio_ctx->av_class) {
        AVIOContext *h = avio_ctx;
        av_opt_get_int(h, "http_code", AV_OPT_SEARCH_CHILDREN, (int64_t *)&http_code);
    }

    if (http_code != 200 && http_code != 201) {
        av_log(avctx, AV_LOG_ERROR, "WHEP 服务器返回错误: HTTP %d\n", http_code);
        ret = AVERROR_HTTP_BAD_REQUEST;
        av_bprint_finalize(&response, NULL);
        goto cleanup;
    }

    // 获取 Location 头（WHEP 资源 URL）
    char *location = NULL;
    if (avio_ctx->av_class) {
        av_opt_get(avio_ctx, "location", AV_OPT_SEARCH_CHILDREN, (uint8_t **)&location);
        if (location) {
            whep->resource_url = av_strdup(location);
            av_log(avctx, AV_LOG_INFO, "WHEP 资源 URL: %s\n", whep->resource_url);
            av_free(location);
        }
    }

    // 保存 SDP answer
    av_bprint_finalize(&response, &whep->remote_sdp);
    if (whep->remote_sdp && strlen(whep->remote_sdp) > 0) {
        av_log(avctx, AV_LOG_INFO, "收到 SDP answer:\n%s\n", whep->remote_sdp);
        ret = 0;
    } else {
        av_log(avctx, AV_LOG_ERROR, "服务器返回空的 SDP answer\n");
        ret = AVERROR_INVALIDDATA;
    }

cleanup:
    if (avio_ctx)
        avio_close(avio_ctx);
    av_dict_free(&options);
    av_freep(&headers);
    return ret;
}

/**
 * 初始化 libdatachannel PeerConnection 并生成 offer
 */
static int whep_init_peer_connection(AVFormatContext *avctx)
{
    WHEPContext *whep = avctx->priv_data;
    rtcConfiguration config;
    int audio_track = -1;
    int video_track = -1;
    int ret;

    av_log(avctx, AV_LOG_INFO, "初始化 libdatachannel...\n");

    // 初始化 libdatachannel 日志
    rtcInitLogger(RTC_LOG_INFO, NULL);

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
        av_log(avctx, AV_LOG_ERROR, "创建 PeerConnection 失败\n");
        return AVERROR_EXTERNAL;
    }

    av_log(avctx, AV_LOG_INFO, "PeerConnection 创建成功 (ID: %d)\n", whep->peer_connection);

    // 设置回调
    rtcSetLocalDescriptionCallback(whep->peer_connection, on_local_description);
    rtcSetStateChangeCallback(whep->peer_connection, on_state_change);
    rtcSetGatheringStateChangeCallback(whep->peer_connection, on_gathering_state_change);
    rtcSetUserPointer(whep->peer_connection, whep);

    // === 添加音频 track (Opus) - 使用 SDP 字符串 ===
    const char *audio_sdp = 
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=recvonly\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:111 minptime=10;useinbandfec=1\r\n";

    audio_track = rtcAddTrack(whep->peer_connection, audio_sdp);
    if (audio_track < 0) {
        av_log(avctx, AV_LOG_ERROR, "添加音频 track 失败: %d\n", audio_track);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(avctx, AV_LOG_INFO, "音频 track 添加成功 (ID: %d)\n", audio_track);

    // === 添加视频 track (H264) - 使用 SDP 字符串 ===
    const char *video_sdp = 
        "m=video 9 UDP/TLS/RTP/SAVPF 103 107 109 115\r\n"
        "a=mid:1\r\n"
        "a=recvonly\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:103 H264/90000\r\n"
        "a=fmtp:103 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f\r\n"
        "a=rtpmap:107 H264/90000\r\n"
        "a=fmtp:107 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42001f\r\n"
        "a=rtpmap:109 H264/90000\r\n"
        "a=fmtp:109 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=rtpmap:115 H264/90000\r\n"
        "a=fmtp:115 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f\r\n";

    video_track = rtcAddTrack(whep->peer_connection, video_sdp);
    if (video_track < 0) {
        av_log(avctx, AV_LOG_ERROR, "添加视频 track 失败: %d\n", video_track);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(avctx, AV_LOG_INFO, "视频 track 添加成功 (ID: %d)\n", video_track);

    // 设置为接收模式（WHEP 是接收端）
    rtcSetLocalDescription(whep->peer_connection, "offer");

    av_log(avctx, AV_LOG_INFO, "等待本地 SDP 生成...\n");

    // 等待本地 SDP 生成（最多等待5秒）
    int64_t start_time = av_gettime_relative();
    while (!whep->local_sdp) {
        if (av_gettime_relative() - start_time > 5000000) { // 5秒超时
            av_log(avctx, AV_LOG_ERROR, "等待本地 SDP 超时\n");
            ret = AVERROR(ETIMEDOUT);
            goto fail;
        }
        av_usleep(10000); // 10ms
    }

    av_log(avctx, AV_LOG_INFO, "本地 SDP 生成完成\n");
    return 0;

fail:
    if (whep->peer_connection >= 0) {
        rtcDeletePeerConnection(whep->peer_connection);
        whep->peer_connection = -1;
    }
    return ret;
}

/**
 * 设置远端 SDP answer
 */
static int whep_set_remote_description(AVFormatContext *avctx)
{
    WHEPContext *whep = avctx->priv_data;
    int ret;

    if (!whep->remote_sdp) {
        av_log(avctx, AV_LOG_ERROR, "远端 SDP 为空\n");
        return AVERROR(EINVAL);
    }

    av_log(avctx, AV_LOG_INFO, "设置远端描述...\n");

    ret = rtcSetRemoteDescription(whep->peer_connection, whep->remote_sdp, "answer");
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "设置远端描述失败: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    av_log(avctx, AV_LOG_INFO, "远端描述设置成功\n");
    return 0;
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
    AVStream *st;
    int ret = 0;

    av_log(avctx, AV_LOG_INFO, "WHEP demuxer initializing...\n");
    
    // 检查URL
    if (!avctx->url || !strlen(avctx->url)) {
        av_log(avctx, AV_LOG_ERROR, "WHEP URL not specified\n");
        return AVERROR(EINVAL);
    }

    whep->whep_url = av_strdup(avctx->url);
    if (!whep->whep_url)
        return AVERROR(ENOMEM);

    av_log(avctx, AV_LOG_INFO, "WHEP URL: %s\n", whep->whep_url);
    av_log(avctx, AV_LOG_INFO, "Timeout: %d ms\n", whep->timeout);
    av_log(avctx, AV_LOG_INFO, "Buffer size: %d\n", whep->buffer_size);
    av_log(avctx, AV_LOG_INFO, "Max retry: %d\n", whep->max_retry);

    // 初始化队列 (每个packet指针大小)
    whep->audio_queue = av_fifo_alloc2(100, sizeof(AVPacket*), 0);
    whep->video_queue = av_fifo_alloc2(100, sizeof(AVPacket*), 0);
    if (!whep->audio_queue || !whep->video_queue) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate packet queues\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    // 初始化互斥锁和条件变量
    ret = pthread_mutex_init(&whep->mutex, NULL);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize mutex\n");
        ret = AVERROR(ret);
        goto fail;
    }

    ret = pthread_cond_init(&whep->cond, NULL);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize condition variable\n");
        pthread_mutex_destroy(&whep->mutex);
        ret = AVERROR(ret);
        goto fail;
    }

    whep->abort_request = 0;
    whep->eof_reached = 0;
    whep->peer_connection = -1;

    // === WHEP 流程：初始化 WebRTC 并交换 SDP ===
    
    // 1. 初始化 PeerConnection 并生成 SDP offer
    ret = whep_init_peer_connection(avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "初始化 PeerConnection 失败\n");
        goto fail;
    }

    // 2. 通过 HTTP POST 交换 SDP
    ret = whep_exchange_sdp(avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "SDP 交换失败\n");
        goto fail;
    }

    // 3. 设置远端 SDP answer
    ret = whep_set_remote_description(avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "设置远端描述失败\n");
        goto fail;
    }

    av_log(avctx, AV_LOG_INFO, "WHEP 信令交互完成，等待 WebRTC 连接建立...\n");

    // 创建视频流（占位）
    st = avformat_new_stream(avctx, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_H264;  // 默认，后续会根据实际情况更新
    avpriv_set_pts_info(st, 64, 1, 1000000);    // 使用微秒作为时间基

    // 创建音频流（占位）
    st = avformat_new_stream(avctx, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = AV_CODEC_ID_OPUS;  // 默认，后续会根据实际情况更新
    avpriv_set_pts_info(st, 64, 1, 1000000);

    whep->initialized = 1;
    whep->start_time = av_gettime_relative();

    av_log(avctx, AV_LOG_INFO, "WHEP demuxer initialized successfully\n");
    
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
        av_log(avctx, AV_LOG_ERROR, "WHEP not initialized\n");
        return AVERROR(EINVAL);
    }

    pthread_mutex_lock(&whep->mutex);

    // 等待队列中有数据或者收到终止/EOF信号
    while (!whep->abort_request && !whep->eof_reached &&
           av_fifo_can_read(whep->audio_queue) == 0 && 
           av_fifo_can_read(whep->video_queue) == 0) {
        av_log(avctx, AV_LOG_DEBUG, "Waiting for packet data...\n");
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
        av_log(avctx, AV_LOG_DEBUG, "Read video packet from queue\n");
    } else if (av_fifo_can_read(whep->audio_queue) > 0) {
        av_fifo_read(whep->audio_queue, &queued_pkt, 1);
        av_log(avctx, AV_LOG_DEBUG, "Read audio packet from queue\n");
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

    av_log(avctx, AV_LOG_INFO, "WHEP demuxer closing...\n");

    // 设置终止标志并唤醒可能在等待的线程
    pthread_mutex_lock(&whep->mutex);
    whep->abort_request = 1;
    pthread_cond_broadcast(&whep->cond);
    pthread_mutex_unlock(&whep->mutex);

    // 关闭 PeerConnection
    if (whep->peer_connection >= 0) {
        av_log(avctx, AV_LOG_INFO, "关闭 PeerConnection...\n");
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
    av_freep(&whep->local_sdp);
    av_freep(&whep->remote_sdp);
    av_freep(&whep->resource_url);

    whep->initialized = 0;

    av_log(avctx, AV_LOG_INFO, "WHEP demuxer closed\n");
    
    return 0;
}

static int whep_read_seek(AVFormatContext *avctx, int stream_index,
                          int64_t timestamp, int flags)
{
    av_log(avctx, AV_LOG_WARNING, "WHEP does not support seeking\n");
    return AVERROR(ENOSYS);
}

static int whep_read_pause(AVFormatContext *avctx)
{
    av_log(avctx, AV_LOG_INFO, "WHEP pause requested\n");
    // 暂停逻辑占位符
    return 0;
}

static int whep_read_play(AVFormatContext *avctx)
{
    av_log(avctx, AV_LOG_INFO, "WHEP play requested\n");
    // 播放逻辑占位符
    return 0;
}

#define OFFSET(x) offsetof(WHEPContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption whep_options[] = {
    { "timeout", "Connection timeout in milliseconds", OFFSET(timeout), AV_OPT_TYPE_INT, {.i64 = 5000}, 0, INT_MAX, DEC },
    { "buffer_size", "Buffer size for incoming packets", OFFSET(buffer_size), AV_OPT_TYPE_INT, {.i64 = 1024*1024}, 0, INT_MAX, DEC },
    { "max_retry", "Maximum number of reconnection attempts", OFFSET(max_retry), AV_OPT_TYPE_INT, {.i64 = 3}, 0, 100, DEC },
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

