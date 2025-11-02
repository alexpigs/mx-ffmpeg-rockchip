/*
 * WHEP (WebRTC-HTTP Egress Protocol) C++ 实现
 * 
 * 使用 metaRTC 实现 WHEP 客户端
 * 参考 metaRTC/demo/metaplayer7/yangplayer/YangRtcReceive.cpp
 */

#include "whep_wrapper.h"

extern "C" {
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "libavutil/frame.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/packet.h"
}

#include <yangutil/yangavinfotype.h>
#include <yangutil/yangtype.h>
#include <yangutil/sys/YangLog.h>
#include <yangrtc/YangPeerConnection.h>
#include <yangrtc/YangPeerInfo.h>
#include <yangrtc/YangWhip.h>

#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <cstring>

/* 帧队列 */
struct FrameQueue {
    std::queue<AVFrame*> queue;
    std::mutex mutex;
    std::condition_variable cond;
    int max_size;
    
    FrameQueue() : max_size(100) {}
    
    ~FrameQueue() {
        clear();
    }
    
    void push(AVFrame *frame) {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.size() >= (size_t)max_size) {
            AVFrame *old = queue.front();
            queue.pop();
            av_frame_free(&old);
        }
        queue.push(av_frame_clone(frame));
        cond.notify_one();
    }
    
    AVFrame* pop(int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex);
        if (timeout_ms > 0) {
            cond.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this]{ return !queue.empty(); });
        } else {
            cond.wait(lock, [this]{ return !queue.empty(); });
        }
        
        if (queue.empty()) {
            return nullptr;
        }
        
        AVFrame *frame = queue.front();
        queue.pop();
        return frame;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) {
            AVFrame *frame = queue.front();
            queue.pop();
            av_frame_free(&frame);
        }
    }
};

/* WHEP 客户端实现 */
struct WHEPClient {
    AVFormatContext *avctx;
    
    /* 配置 */
    std::string url;
    int timeout;
    std::string ice_servers;
    int audio_enabled;
    int video_enabled;
    
    /* metaRTC 组件 */
    YangContext *context;
    YangPeerConnection *peer_conn;
    
    /* 帧队列 */
    FrameQueue audio_queue;
    FrameQueue video_queue;
    
    /* 回调 */
    whep_audio_callback audio_cb;
    whep_video_callback video_cb;
    void *cb_opaque;
    
    /* 状态 */
    int connected;
    int audio_stream_index;
    int video_stream_index;
    
    WHEPClient(AVFormatContext *s) 
        : avctx(s), timeout(10), audio_enabled(1), video_enabled(1),
          context(nullptr), peer_conn(nullptr),
          audio_cb(nullptr), video_cb(nullptr), cb_opaque(nullptr),
          connected(0), audio_stream_index(-1), video_stream_index(-1) {
    }
    
    ~WHEPClient() {
        disconnect();
    }
    
    int connect();
    int disconnect();
    int read_frame(AVPacket *pkt, int timeout_ms);
    
private:
    static void receive_audio_callback(void* user, YangFrame *frame);
    static void receive_video_callback(void* user, YangFrame *frame);
    void handle_audio_frame(YangFrame *frame);
    void handle_video_frame(YangFrame *frame);
};

/* 音频回调 */
void WHEPClient::receive_audio_callback(void* user, YangFrame *frame) {
    if (!user) return;
    WHEPClient *client = (WHEPClient*)user;
    client->handle_audio_frame(frame);
}

void WHEPClient::handle_audio_frame(YangFrame *frame) {
    if (!audio_enabled || !frame || !frame->payload) return;
    
    AVFrame *avframe = av_frame_alloc();
    if (!avframe) return;
    
    /* 填充音频帧 */
    avframe->format = AV_SAMPLE_FMT_S16;
    avframe->sample_rate = context->avinfo.audio.sample;
    avframe->ch_layout.nb_channels = context->avinfo.audio.channel;
    avframe->nb_samples = frame->nb;
    avframe->pts = frame->pts;
    
    if (av_frame_get_buffer(avframe, 0) < 0) {
        av_frame_free(&avframe);
        return;
    }
    
    memcpy(avframe->data[0], frame->payload, frame->nb * avframe->ch_layout.nb_channels * 2);
    
    audio_queue.push(avframe);
    av_frame_free(&avframe);
    
    if (audio_cb) {
        audio_cb(cb_opaque, avframe);
    }
}

/* 视频回调 */
void WHEPClient::receive_video_callback(void* user, YangFrame *frame) {
    if (!user) return;
    WHEPClient *client = (WHEPClient*)user;
    client->handle_video_frame(frame);
}

void WHEPClient::handle_video_frame(YangFrame *frame) {
    if (!video_enabled || !frame || !frame->payload) return;
    
    AVFrame *avframe = av_frame_alloc();
    if (!avframe) return;
    
    /* 填充视频帧 */
    avframe->format = AV_PIX_FMT_YUV420P;
    avframe->width = context->avinfo.video.width;
    avframe->height = context->avinfo.video.height;
    avframe->pts = frame->pts;
    
    if (av_frame_get_buffer(avframe, 32) < 0) {
        av_frame_free(&avframe);
        return;
    }
    
    /* 复制 YUV 数据 */
    int y_size = avframe->width * avframe->height;
    int uv_size = y_size / 4;
    
    memcpy(avframe->data[0], frame->payload, y_size);
    memcpy(avframe->data[1], frame->payload + y_size, uv_size);
    memcpy(avframe->data[2], frame->payload + y_size + uv_size, uv_size);
    
    video_queue.push(avframe);
    av_frame_free(&avframe);
    
    if (video_cb) {
        video_cb(cb_opaque, avframe);
    }
}

/* 连接 */
int WHEPClient::connect() {
    if (connected) {
        return 0;
    }
    
    /* 创建 YangContext */
    context = new YangContext();
    if (!context) {
        av_log(avctx, AV_LOG_ERROR, "无法分配 YangContext\n");
        return AVERROR(ENOMEM);
    }
    
    context->init();
    
    /* 配置 RTC 参数 - 参考 mainwindow.cpp */
    context->avinfo.sys.mediaServer = Yang_Server_Srs;  // 连接 SRS
    context->avinfo.rtc.rtcSocketProtocol = Yang_Socket_Protocol_Udp;
    context->avinfo.rtc.rtcLocalPort = 10000 + (rand() % 15000);
    context->avinfo.rtc.iceCandidateType = YangIceHost;
    context->avinfo.rtc.turnSocketProtocol = Yang_Socket_Protocol_Udp;
    
    /* 音频配置 - 参考 YangPlayerHandleImpl::playRtc */
    context->avinfo.audio.sample = 48000;
    context->avinfo.audio.channel = 2;
    context->avinfo.audio.audioDecoderType = Yang_AED_OPUS;
    context->avinfo.audio.enableMono = yangfalse;
    context->avinfo.audio.enableAudioFec = yangfalse;  // SRS 不使用 audio FEC
    context->avinfo.audio.aIndex = -1;
    
    /* 视频配置 */
    context->avinfo.video.videoDecHwType = 0;
    
    av_log(avctx, AV_LOG_INFO, "连接 SRS URL: %s\n", url.c_str());
    
    /* 创建 PeerConnection - 参考 YangRtcReceive::init */
    peer_conn = (YangPeerConnection*)calloc(sizeof(YangPeerConnection), 1);
    if (!peer_conn) {
        av_log(avctx, AV_LOG_ERROR, "无法分配 PeerConnection\n");
        delete context;
        context = nullptr;
        return AVERROR(ENOMEM);
    }
    
    /* 初始化 PeerInfo */
    yang_avinfo_initPeerInfo(&peer_conn->peer.peerInfo, &context->avinfo);
    peer_conn->peer.peerInfo.rtc.rtcLocalPort = context->avinfo.rtc.rtcLocalPort++;
    peer_conn->peer.peerInfo.direction = YangRecvonly;
    peer_conn->peer.peerInfo.uid = 0;
    
    /* 设置接收回调 */
    peer_conn->peer.peerCallback.recvCallback.context = this;
    peer_conn->peer.peerCallback.recvCallback.receiveAudio = receive_audio_callback;
    peer_conn->peer.peerCallback.recvCallback.receiveVideo = receive_video_callback;
    peer_conn->peer.peerCallback.recvCallback.receiveMsg = nullptr;
    
    /* 复制 RTC 回调 */
    memcpy(&peer_conn->peer.peerCallback.rtcCallback, &context->rtcCallback, sizeof(YangRtcCallback));
    
    /* 创建 PeerConnection */
    yang_create_peerConnection(peer_conn);
    
    /* 添加音视频轨道和收发器 - 参考 YangRtcReceive::init */
    if (audio_enabled) {
        peer_conn->addAudioTrack(&peer_conn->peer, (YangAudioCodec)context->avinfo.audio.audioDecoderType);
        peer_conn->addTransceiver(&peer_conn->peer, YangMediaAudio, YangRecvonly);
    }
    
    if (video_enabled) {
        peer_conn->addVideoTrack(&peer_conn->peer, Yang_VED_H264);
        peer_conn->addTransceiver(&peer_conn->peer, YangMediaVideo, YangRecvonly);
    }
    
    /* 连接 SRS 服务器 - 参考 YangRtcReceive::startLoop */
    char *url_copy = strdup(url.c_str());
    int32_t err = yang_whip_connectSfuServer(&peer_conn->peer, url_copy, context->avinfo.sys.mediaServer);
    free(url_copy);
    
    if (err != Yang_Ok) {
        av_log(avctx, AV_LOG_ERROR, "连接 SRS 服务器失败: %d\n", err);
        yang_destroy_peerConnection(peer_conn);
        free(peer_conn);
        peer_conn = nullptr;
        delete context;
        context = nullptr;
        return AVERROR(EIO);
    }
    
    av_log(avctx, AV_LOG_INFO, "SRS 连接成功\n");
    connected = 1;
    
    return 0;
}

/* 断开连接 */
int WHEPClient::disconnect() {
    if (!connected) {
        return 0;
    }
    
    if (peer_conn) {
        if (peer_conn->close) {
            peer_conn->close(&peer_conn->peer);
        }
        yang_destroy_peerConnection(peer_conn);
        free(peer_conn);
        peer_conn = nullptr;
    }
    
    if (context) {
        delete context;
        context = nullptr;
    }
    
    connected = 0;
    av_log(avctx, AV_LOG_INFO, "WHEP 断开连接\n");
    
    return 0;
}

/* 读取帧 */
int WHEPClient::read_frame(AVPacket *pkt, int timeout_ms) {
    /* 优先读取视频帧 */
    AVFrame *frame = nullptr;
    
    if (video_enabled && !video_queue.queue.empty()) {
        frame = video_queue.pop(0);
        if (frame) {
            /* 转换为 AVPacket */
            av_new_packet(pkt, frame->width * frame->height * 3 / 2);
            memcpy(pkt->data, frame->data[0], frame->width * frame->height);
            memcpy(pkt->data + frame->width * frame->height, frame->data[1], frame->width * frame->height / 4);
            memcpy(pkt->data + frame->width * frame->height * 5 / 4, frame->data[2], frame->width * frame->height / 4);
            
            pkt->pts = frame->pts;
            pkt->dts = frame->pts;
            pkt->stream_index = video_stream_index;
            
            av_frame_free(&frame);
            return 0;
        }
    }
    
    if (audio_enabled && !audio_queue.queue.empty()) {
        frame = audio_queue.pop(0);
        if (frame) {
            /* 转换为 AVPacket */
            int data_size = frame->nb_samples * frame->ch_layout.nb_channels * 2;
            av_new_packet(pkt, data_size);
            memcpy(pkt->data, frame->data[0], data_size);
            
            pkt->pts = frame->pts;
            pkt->dts = frame->pts;
            pkt->stream_index = audio_stream_index;
            
            av_frame_free(&frame);
            return 0;
        }
    }
    
    /* 等待新帧 */
    if (video_enabled) {
        frame = video_queue.pop(timeout_ms);
        if (frame) {
            av_new_packet(pkt, frame->width * frame->height * 3 / 2);
            memcpy(pkt->data, frame->data[0], frame->width * frame->height);
            memcpy(pkt->data + frame->width * frame->height, frame->data[1], frame->width * frame->height / 4);
            memcpy(pkt->data + frame->width * frame->height * 5 / 4, frame->data[2], frame->width * frame->height / 4);
            
            pkt->pts = frame->pts;
            pkt->dts = frame->pts;
            pkt->stream_index = video_stream_index;
            
            av_frame_free(&frame);
            return 0;
        }
    }
    
    if (audio_enabled) {
        frame = audio_queue.pop(timeout_ms);
        if (frame) {
            int data_size = frame->nb_samples * frame->ch_layout.nb_channels * 2;
            av_new_packet(pkt, data_size);
            memcpy(pkt->data, frame->data[0], data_size);
            
            pkt->pts = frame->pts;
            pkt->dts = frame->pts;
            pkt->stream_index = audio_stream_index;
            
            av_frame_free(&frame);
            return 0;
        }
    }
    
    return AVERROR(EAGAIN);
}

/* ==================== C 接口实现 ==================== */

WHEPClient* whep_client_create(AVFormatContext *s) {
    return new WHEPClient(s);
}

int whep_client_set_url(WHEPClient *client, const char *url) {
    if (!client || !url) return AVERROR(EINVAL);
    client->url = url;
    return 0;
}

int whep_client_set_timeout(WHEPClient *client, int timeout_sec) {
    if (!client) return AVERROR(EINVAL);
    client->timeout = timeout_sec;
    return 0;
}

int whep_client_set_ice_servers(WHEPClient *client, const char *ice_servers) {
    if (!client) return AVERROR(EINVAL);
    if (ice_servers) {
        client->ice_servers = ice_servers;
    }
    return 0;
}

int whep_client_enable_audio(WHEPClient *client, int enable) {
    if (!client) return AVERROR(EINVAL);
    client->audio_enabled = enable;
    return 0;
}

int whep_client_enable_video(WHEPClient *client, int enable) {
    if (!client) return AVERROR(EINVAL);
    client->video_enabled = enable;
    return 0;
}

void whep_client_set_callbacks(WHEPClient *client,
                               whep_audio_callback audio_cb,
                               whep_video_callback video_cb,
                               void *opaque) {
    if (!client) return;
    client->audio_cb = audio_cb;
    client->video_cb = video_cb;
    client->cb_opaque = opaque;
}

int whep_client_connect(WHEPClient *client) {
    if (!client) return AVERROR(EINVAL);
    return client->connect();
}

int whep_client_disconnect(WHEPClient *client) {
    if (!client) return AVERROR(EINVAL);
    return client->disconnect();
}

int whep_client_read_frame(WHEPClient *client, AVPacket *pkt, int timeout_ms) {
    if (!client || !pkt) return AVERROR(EINVAL);
    return client->read_frame(pkt, timeout_ms);
}

void whep_client_destroy(WHEPClient *client) {
    if (client) {
        delete client;
    }
}
