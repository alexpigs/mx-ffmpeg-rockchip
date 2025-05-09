/*
 * Copyright (c) 2013 Clément Bœsch
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
#include "mxcamenc_pipe.h"


#define VCTRL_FILE "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/camera.ctl"
#define VREPLY_FILE "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/camera.reply"
#define ACRTL_FILE "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/audio.ctl"
#define AREPLY_FILE "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/audio.reply"

static int open_fd(const char *fmt, int phone)
{
    char full_path[256] = {0};
    snprintf(full_path, sizeof(full_path) -1, fmt, phone);

    if (access(full_path, F_OK)){
        mkfifo(full_path, 0666);
    }

    chmod(full_path, 0666);
    int fd = open(full_path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        av_log(NULL, AV_LOG_ERROR, "MXCamEnc: open fifo %s failed\n", full_path);
        return -1;
    }
    
    fcntl(fd, F_SETPIPE_SZ, 1024*1024);
    return fd;
}


int mxcam_open_pipes(MxContext *mx)
{
    char path[256];

    av_log(NULL, AV_LOG_INFO, "MXCamEnc: mxcam_open_pipes");
    ALOGD("MXCamEnc: open fifo phone=%d", mx->phone);

    mx->video_cmd_fd = open_fd(VCTRL_FILE, mx->phone);
    mx->video_data_fd = open_fd(VREPLY_FILE, mx->phone);
    mx->audio_cmd_fd = open_fd(ACRTL_FILE, mx->phone);
    mx->audio_data_fd = open_fd(AREPLY_FILE, mx->phone);
    if (mx->video_cmd_fd < 0 || mx->video_data_fd < 0 ||
        mx->audio_cmd_fd < 0 || mx->audio_data_fd < 0) {
            ALOGE("MXCamEnc: open fifo failed");
        return AVERROR(EIO);
    }

    ALOGD("MXCamEnc: open fifo ok");

    return 0;
}

int mxcam_close_pipes(MxContext *mx)
{
    close(mx->video_cmd_fd);
    close(mx->video_data_fd);
    close(mx->audio_cmd_fd);
    close(mx->audio_data_fd);

    return 0;
}