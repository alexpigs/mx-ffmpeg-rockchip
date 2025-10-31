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
} WHEPContext;


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
 * Track 打开回调
 */
static void on_track_open(int tr, void *user_ptr)
{
    av_log(NULL, AV_LOG_INFO, "Track 已打开 (ID: %d)\n", tr);
}

/**
 * 音频 Track 消息回调 - 接收 RTP 数据
 */
static void on_audio_message(int tr, const char *data, int size, void *user_ptr)
{
    av_log(NULL, AV_LOG_INFO, "收到音频数据: Track ID=%d, 大小=%d bytes\n", tr, size);
    // TODO: 将数据解析并放入队列
}

/**
 * 视频 Track 消息回调 - 接收 RTP 数据
 */
static void on_video_message(int tr, const char *data, int size, void *user_ptr)
{
    av_log(NULL, AV_LOG_INFO, "收到视频数据: Track ID=%d, 大小=%d bytes\n", tr, size);
    // TODO: 将数据解析并放入队列
}


/**
 * 初始化 libdatachannel PeerConnection 并添加 tracks
 */
static int whep_init_peer_connection(AVFormatContext *avctx)
{
    WHEPContext *whep = avctx->priv_data;
    rtcConfiguration config;
    int ret;

    av_log(avctx, AV_LOG_INFO, "初始化 libdatachannel...\n");

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
        av_log(avctx, AV_LOG_ERROR, "创建 PeerConnection 失败\n");
        return AVERROR_EXTERNAL;
    }

    av_log(avctx, AV_LOG_INFO, "PeerConnection 创建成功 (ID: %d)\n", whep->peer_connection);

    // 设置回调
    rtcSetStateChangeCallback(whep->peer_connection, on_state_change);
    rtcSetGatheringStateChangeCallback(whep->peer_connection, on_gathering_state_change);
    rtcSetUserPointer(whep->peer_connection, whep);

    // === 添加音频 track (Opus) - 使用 SRS 兼容的 SDP 字符串 ===
    // SRS 服务器期望标准的 SDP 格式，包含必要的属性
    const char *audio_sdp = 
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=recvonly\r\n"
        "a=rtcp-mux\r\n"
        "a=rtcp-rsize\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:111 minptime=10;useinbandfec=1\r\n";

    whep->audio_track = rtcAddTrack(whep->peer_connection, audio_sdp);
    if (whep->audio_track < 0) {
        av_log(avctx, AV_LOG_ERROR, "添加音频 track 失败: %d\n", whep->audio_track);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(avctx, AV_LOG_INFO, "音频 track 添加成功 (ID: %d)\n", whep->audio_track);

    // 设置音频 track 回调
    rtcSetOpenCallback(whep->audio_track, on_track_open);
    rtcSetMessageCallback(whep->audio_track, on_audio_message);
    rtcSetUserPointer(whep->audio_track, whep);

    // === 添加视频 track (H264) - 使用 SRS 兼容的 SDP 字符串 ===
    // SRS 通常使用 H.264 Baseline/Constrained Baseline Profile
    // profile-level-id=42e01f 表示 Baseline Level 3.1
    const char *video_sdp = 
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:1\r\n"
        "a=recvonly\r\n"
        "a=rtcp-mux\r\n"
        "a=rtcp-rsize\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=rtpmap:97 H264/90000\r\n"
        "a=fmtp:97 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f\r\n"
        "a=rtpmap:98 H264/90000\r\n"
        "a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f\r\n"
        "a=rtcp-fb:96 goog-remb\r\n"
        "a=rtcp-fb:96 transport-cc\r\n"
        "a=rtcp-fb:96 ccm fir\r\n"
        "a=rtcp-fb:96 nack\r\n"
        "a=rtcp-fb:96 nack pli\r\n";

    whep->video_track = rtcAddTrack(whep->peer_connection, video_sdp);
    if (whep->video_track < 0) {
        av_log(avctx, AV_LOG_ERROR, "添加视频 track 失败: %d\n", whep->video_track);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(avctx, AV_LOG_INFO, "视频 track 添加成功 (ID: %d)\n", whep->video_track);

    // 设置视频 track 回调
    rtcSetOpenCallback(whep->video_track, on_track_open);
    rtcSetMessageCallback(whep->video_track, on_video_message);
    rtcSetUserPointer(whep->video_track, whep);

    av_log(avctx, AV_LOG_INFO, "PeerConnection 初始化完成，回调已设置\n");
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
    whep->audio_track = -1;
    whep->video_track = -1;

    // === WHEP 流程：初始化 WebRTC 并交换 SDP ===
    
    // 1. 初始化 PeerConnection 并添加 tracks
    ret = whep_init_peer_connection(avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "初始化 PeerConnection 失败\n");
        goto fail;
    }

    // 2. 使用共享函数交换 SDP 并设置远端描述
    ret = ff_whip_whep_exchange_and_set_sdp(avctx, whep->peer_connection, whep->token, &whep->resource_url);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "SDP 交换失败\n");
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

    // 使用共享函数删除 WHEP 会话
    if (whep->resource_url) {
        av_log(avctx, AV_LOG_INFO, "删除 WHEP 会话...\n");
        ff_whip_whep_delete_session(avctx, whep->token, whep->resource_url);
    }

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
    av_freep(&whep->token);
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

