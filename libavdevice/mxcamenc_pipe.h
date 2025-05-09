#ifndef __MXCAMENC_PIPE_H__
#define __MXCAMENC_PIPE_H__

#include "mxcamenc_common.h"


#ifdef __cplusplus
extern "C" {
#endif
int mxcam_start_server_socket(MxContext *mx);
int mxcam_handle_packet(AVFormatContext *s1, AVPacket *pkt);

#ifdef __cplusplus
}
#endif


#endif /* __MXCAMENC_PIPE_H__ */