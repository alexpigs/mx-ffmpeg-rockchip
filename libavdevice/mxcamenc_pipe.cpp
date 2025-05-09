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


static const char* VCTRL_FILE = "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/camera.ctl";
static const char* VREPLY_FILE = "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/camera.reply";
static const char* ACRTL_FILE = "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/audio.ctl";
static const char* AREPLY_FILE = "/mnt/data/mxdroid/containers/%d/phone/data/misc/.mxdroid/device/camera/audio.reply";

static int open_ctl_fd(const char *fmt, int phone)
{
    char full_path[256] = {0};
    snprintf(full_path, sizeof(full_path) -1, fmt, phone);

    if (access(full_path, F_OK)){
        mkfifo(full_path, 0666);
    }

    chmod(full_path, 0666);
    int fd = open(full_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        ALOGE("MXCamEnc: open fifo %s failed", full_path);
        return -1;
    }
    
    return fd;
}

static int open_data_fd(const char *fmt, int phone)
{
    char full_path[256] = {0};
    snprintf(full_path, sizeof(full_path) -1, fmt, phone);

    if (access(full_path, F_OK)){
        mkfifo(full_path, 0666);
    }

    chmod(full_path, 0666);
    int fd = open(full_path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        ALOGE("MXCamEnc: open fifo %s failed", full_path);
        return -1;
    }
    
    fcntl(fd, F_SETPIPE_SZ, 1024*1024);
    return fd;
}

static void* audio_io_threadfunc(void *arg)
{
    MxContext *mx = (MxContext*)arg;
    ALOGD("MXCamEnc: audio_io_threadfunc %d\n", mx->phone);

    return 0;
}


int mxcam_open_pipes(MxContext *mx)
{
    ALOGD("MXCamEnc: open fifo phone=%d", mx->phone);

    mx->video_cmd_fd = open_ctl_fd(VCTRL_FILE, mx->phone);
    mx->video_data_fd = open_data_fd(VREPLY_FILE, mx->phone);
    mx->audio_cmd_fd = open_ctl_fd(ACRTL_FILE, mx->phone);
    mx->audio_data_fd = open_data_fd(AREPLY_FILE, mx->phone);
    if (mx->video_cmd_fd < 0 || mx->video_data_fd < 0 ||
        mx->audio_cmd_fd < 0 || mx->audio_data_fd < 0) {
            ALOGE("MXCamEnc: open fifo failed");
        return AVERROR(EIO);
    }

    ALOGD("MXCamEnc: open fifo ok");

    return 0;
}

int mxcam_handle_packet(AVFormatContext *s1, AVPacket *pkt){

    MxContext *mx = (MxContext*)s1->priv_data;
    ALOGD("MXCamEnc: write_packet %d\n", mx->phone);

    // 将packet放到对应的list
    if (pkt->stream_index == mx->audio_stream_idx) {
        pthread_mutex_lock(&mx->al_mutex);
        avpriv_packet_list_put(&mx->audio_list, pkt, NULL, 0);
        pthread_cond_signal(&mx->al_cond);
        pthread_mutex_unlock(&mx->al_mutex);

    } else if (pkt->stream_index == mx->video_stream_idx) {
        pthread_mutex_lock(&mx->vl_mutex);
        avpriv_packet_list_put(&mx->video_list, pkt, NULL, 0);
        pthread_cond_signal(&mx->vl_cond);
        pthread_mutex_unlock(&mx->vl_mutex);
    } else {
        ALOGE("MXCamEnc: unknown stream index %d\n", pkt->stream_index);
        return AVERROR(EINVAL);
    }
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