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
#include <stdio.h>
#include <stdlib.h>

#include "mxcamenc_common.h"
#include "mxcamenc_pipe.h"

static int write_header(AVFormatContext *s1)
{
    MxContext *mx = s1->priv_data;

    ALOGD("MXCamEnc: write_header url=%s,phone=%d\n", 
        s1->url,
        mx->phone);

    if (mxcam_open_pipes(mx) < 0) {
        ALOGE("MXCamEnc: open fifo failed\n");
        return AVERROR(EIO);
    }

    mx->audio_stream_idx = -1;
    mx->video_stream_idx = -1;

    for (int i = 0; i < s1->nb_streams; i++) {
        AVStream *st = s1->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            mx->audio_stream_idx = i;
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            mx->video_stream_idx = i;
        }
    }

    if (mx->audio_stream_idx < 0) {
        ALOGE("MXCamEnc: no audio stream present\n");
    }

    if (mx->video_stream_idx < 0) {
        ALOGE("MXCamEnc: no video stream present\n");
    }

    return 0;
}

static int write_packet(AVFormatContext *s1, AVPacket *pkt)
{
    return mxcam_handle_packet(s1, pkt);
}

static int write_trailer(AVFormatContext *s1)
{
    ALOGD("MXCamEnc: write_trailer\n");
    return 0;
}

#define OFFSET(x) offsetof(MxContext, x)
static const AVOption options[] = {
    { "phone", 
        "set phone id",       
        OFFSET(phone), 
        AV_OPT_TYPE_INT,  
        {.i64 = 0 }, INT_MIN, INT_MAX,
        AV_OPT_FLAG_ENCODING_PARAM 
    },
    { NULL }

};

static const AVClass mxcam_class = {
    .class_name = "Maxia Camera outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

const FFOutputFormat ff_mxcam_muxer = {
    .p.name         = "mxcam",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Maxia Camera output device"),
    .p.audio_codec  = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE),
    .p.video_codec  = AV_CODEC_ID_WRAPPED_AVFRAME,
    .p.flags        = AVFMT_NOFILE,
    .p.priv_class    = &mxcam_class,
    .priv_data_size = sizeof(MxContext),
    .write_header   = write_header,
    .write_packet   = write_packet,
    .write_trailer  = write_trailer,
};
