#ifndef MXCAMENC_COMMON_H
#define MXCAMENC_COMMON_H

#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
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
#include <libavutil/hwcontext.h>
#ifdef __cplusplus
}
#endif

static inline void __log_print__x123(int level, const char *fmt, const char *f,
                                     const char *func, int line, va_list args) {
  char buffer[2048] = {0};
  long msec = 0, usec = 0;
  static struct timeval last_time = {0, 0};
  struct timeval cur;
  gettimeofday(&cur, NULL);
  if (last_time.tv_sec == 0 && last_time.tv_usec == 0) {
    last_time = cur;
  }
  msec = (cur.tv_sec - last_time.tv_sec) * 1000 +
         (cur.tv_usec - last_time.tv_usec) / 1000;
  usec = (cur.tv_sec - last_time.tv_sec) * 1000000 +
         (cur.tv_usec - last_time.tv_usec);
  last_time.tv_sec = cur.tv_sec;
  last_time.tv_usec = cur.tv_usec;

  vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);

  if (level == 4) {
    printf("\033[31m%s\033[0m:%ldus \033[32m%s:%d \033[33m%s\033[0m\n", "ERROR",
           usec, func, line, buffer);
  } else {
    printf("\033[34m%s\033[0m:%ldus \033[32m%s:%d \033[33m%s\033[0m\n", "DEBUG",
           usec, func, line, buffer);
  }
}

static inline void __log_print__(int level, const char *fmt, const char *f,
                                 const char *func, int line, ...) {
  va_list args;
  va_start(args, line);
  __log_print__x123(level, fmt, f, func, line, args);
  va_end(args);
}

#define ALOGD(fmt, ...)                                                        \
  { __log_print__(1, fmt, __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__); }

#define ALOGE(fmt, ...)                                                        \
  { __log_print__(4, fmt, __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__); }

typedef struct {
  const AVClass *cclass;
  void *ctx;
  /*options*/
  int phone;
  int listen_port;
  char *listen_ip;

  /* context */
  void *mx_sockserver;
  void *mx_video_pipeserver;
  int is_stop;
  int audio_stream_idx;
  int video_stream_idx;

  int video_width;
  int video_height;
  int video_format;
  int video_fps;
  int video_bitrate;

  pthread_t io_worker;

} MxContext;

#endif