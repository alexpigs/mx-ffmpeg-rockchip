#ifndef __MXCAMENC_PIPE_H__
#define __MXCAMENC_PIPE_H__

#include "mxcamenc_common.h"


#ifdef __cplusplus
extern "C" {
#endif
int mxcam_open_pipes(MxContext *mx);
int mxcam_close_pipes(MxContext *mx);

#ifdef __cplusplus
}
#endif


#endif /* __MXCAMENC_PIPE_H__ */