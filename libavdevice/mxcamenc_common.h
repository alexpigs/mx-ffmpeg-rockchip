#ifndef MXCAMENC_COMMON_H
#define MXCAMENC_COMMON_H

#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "avdevice.h"
#include "libavcodec/packet_internal.h"
#include "libavformat/avformat.h"
#include "libavformat/mux.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/thread.h"

#ifdef __cplusplus
}
#endif

static inline void __log_print__x123(int level, const char *fmt, va_list args) {
  char buffer[1024] = {0};
  vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
  switch (level) {
  case 0:
    av_log(NULL, AV_LOG_VERBOSE, "%s\n", buffer);
    break;
  case 1:
    av_log(NULL, AV_LOG_WARNING, "%s\n", (buffer));
    break;
  case 2:
    av_log(NULL, AV_LOG_WARNING, "%s\n", (buffer));
    break;
  case 3:
    av_log(NULL, AV_LOG_WARNING, "%s\n", (buffer));
    break;
  case 4:
    av_log(NULL, AV_LOG_ERROR, "%s\n", (buffer));
    break;
  default:
    av_log(NULL, AV_LOG_VERBOSE, "%s\n", (buffer));
    break;
  }
}

static inline void __log_print__(int level, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  __log_print__x123(level, fmt, args);
  va_end(args);
}

#define ALOGV(fmt, ...)                                                        \
  {                                                                            \
    __log_print__(0, "%s:%s:%d " #fmt, __FILE__, __FUNCTION__, __LINE__,       \
                  ##__VA_ARGS__);                                              \
  }
#define ALOGD(fmt, ...)                                                        \
  {                                                                            \
    __log_print__(1, "%s:%s:%d " #fmt, __FILE__, __FUNCTION__, __LINE__,       \
                  ##__VA_ARGS__);                                              \
  }
#define ALOGI(fmt, ...)                                                        \
  {                                                                            \
    __log_print__(2, "%s:%s:%d " #fmt, __FILE__, __FUNCTION__, __LINE__,       \
                  ##__VA_ARGS__);                                              \
  }
#define ALOGW(fmt, ...)                                                        \
  {                                                                            \
    __log_print__(3, "%s:%s:%d " #fmt, __FILE__, __FUNCTION__, __LINE__,       \
                  ##__VA_ARGS__);                                              \
  }
#define ALOGE(fmt, ...)                                                        \
  {                                                                            \
    __log_print__(4, "%s:%s:%d " #fmt, __FILE__, __FUNCTION__, __LINE__,       \
                  ##__VA_ARGS__);                                              \
  }

typedef struct {
  const AVClass *cclass;
  void *ctx;
  /*options*/
  int phone;
  int listen_port;
  char *listen_ip;

  /* context */
  void *mx_server;
  int is_stop;
  int audio_stream_idx;
  int video_stream_idx;

  pthread_t io_worker;

  pthread_mutex_t vl_mutex;
  pthread_cond_t vl_cond;
  PacketList video_list;
  int video_packet_cnt;

  pthread_mutex_t al_mutex;
  pthread_cond_t al_cond;
  PacketList audio_list;
  int audio_packet_cnt;
} MxContext;

#endif