/*
 * WHEP (WebRTC-HTTP Egress Protocol) C 包装接口
 * 
 * 这个头文件定义了纯 C 接口，实际实现在 whep_wrapper.cpp 中使用 C++ 和 metaRTC
 */

#ifndef AVFORMAT_WHEP_WRAPPER_H
#define AVFORMAT_WHEP_WRAPPER_H

#include "libavutil/frame.h"
#include "libavformat/avformat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄，隐藏 C++ 实现细节 */
typedef struct WHEPClient WHEPClient;

/* 音视频帧回调 */
typedef void (*whep_audio_callback)(void *opaque, AVFrame *frame);
typedef void (*whep_video_callback)(void *opaque, AVFrame *frame);

/* 创建 WHEP 客户端 */
WHEPClient* whep_client_create(AVFormatContext *s);

/* 配置参数 */
int whep_client_set_url(WHEPClient *client, const char *url);
int whep_client_set_timeout(WHEPClient *client, int timeout_sec);
int whep_client_set_ice_servers(WHEPClient *client, const char *ice_servers);
int whep_client_enable_audio(WHEPClient *client, int enable);
int whep_client_enable_video(WHEPClient *client, int enable);

/* 设置回调 */
void whep_client_set_callbacks(WHEPClient *client,
                               whep_audio_callback audio_cb,
                               whep_video_callback video_cb,
                               void *opaque);

/* 连接和断开 */
int whep_client_connect(WHEPClient *client);
int whep_client_disconnect(WHEPClient *client);

/* 读取帧（阻塞，直到有帧或超时） */
int whep_client_read_frame(WHEPClient *client, AVPacket *pkt, int timeout_ms);

/* 销毁客户端 */
void whep_client_destroy(WHEPClient *client);

#ifdef __cplusplus
}
#endif

#endif /* AVFORMAT_WHEP_WRAPPER_H */

