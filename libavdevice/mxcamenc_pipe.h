#ifndef __MXCAMENC_PIPE_H__
#define __MXCAMENC_PIPE_H__

#include "mxcamenc_common.h"

#define VCTRL_FILE                                                             \
  "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/"    \
  "camera.ctl"
#define VREPLY_FILE                                                            \
  "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/"    \
  "camera.reply"
#define ACRTL_FILE                                                             \
  "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/"    \
  "audio.ctl"
#define AREPLY_FILE                                                            \
  "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/"    \
  "audio.reply"

#ifdef __cplusplus
extern "C" {
#endif
int mxcam_start_server(MxContext *mx);
int mxcam_handle_packet(AVFormatContext *s1, AVPacket *pkt);
int mxcam_stop_server(MxContext *mx);

#ifdef __cplusplus
}
#endif

#endif /* __MXCAMENC_PIPE_H__ */