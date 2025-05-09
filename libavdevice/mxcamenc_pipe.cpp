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
#define BOOST_ERROR_CODE_HEADER_ONLY
#include "mxcamenc_pipe.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <boost/asio.hpp>

static inline int _parse_query(const char *query,
                               char *query_name,
                               int query_name_size,
                               const char **query_param)
{
    /* Extract query name. */
    const char *qend = strchr(query, ' ');
    if (qend == NULL)
    {
        qend = query + strlen(query);
    }
    if ((qend - query) >= query_name_size)
    {
        return qend - query + 1;
    }
    memcpy(query_name, query, qend - query);
    query_name[qend - query] = '\0';
    /* Calculate query parameters pointer (if needed) */
    if (query_param != NULL)
    {
        if (*qend == ' ')
        {
            qend++;
        }
        *query_param = (*qend == '\0') ? NULL : qend;
    }
    return 0;
}

int send_reply(int fd, const char *reply, int size)
{
    int ret = 0;
    int bytes_send = 0;
    while (bytes_send < size)
    {
        ret = write(fd, reply + bytes_send, size - bytes_send);
        if (ret == -1)
        {
            if (errno == EAGAIN)
            {
                ALOGD("write reply EAGAIN");
                usleep(1);
                continue;
            }
            ALOGE("write reply failed %d %s", ret, strerror(errno));
            break;
        }
        else if (ret == 0)
        {
            ALOGE("write reply failed %d %s", ret, strerror(errno));
            break;
        }

        bytes_send += ret;
    }

    return bytes_send;
}

int reply_success(int fd, const char *result, int result_size)
{
    if (result == nullptr)
    {
        static const char *ok_null_msg = "00000003ok\0";
        static int ok_null_msg_size = 11;
        return send_reply(fd, ok_null_msg, ok_null_msg_size);
    }

    char size_str[16] = {0};
    snprintf(size_str, sizeof(size_str), "%08xok:", result_size + 3);

    int ret = send_reply(fd, size_str, 11);

    if (ret != 11)
    {
        ALOGE("send reply failed %d %s", ret, strerror(errno));
        return ret;
    }
    return send_reply(fd, result, result_size);
}

int reply_failed(int fd)
{
    static const char *failed_msg = "00000003ko\0";
    static int failed_msg_size = 11;
    return send_reply(fd, failed_msg, failed_msg_size);
}

#define MAX_CMD_SIZE 1024

static void *video_io_threadfunc(void *arg)
{
    MxContext *mx = (MxContext *)arg;
    char buf[MAX_CMD_SIZE] = {0};
    ALOGD("MXCamEnc: video_io_threadfunc %d\n", mx->phone);

    while (!mx->is_stop)
    {
        int client = accept(mx->video_server_socket, NULL, NULL);
        if (client < 0)
        {
            break;
        }
    }
    ALOGD("MXCamEnc: video_io_threadfunc exit\n");

    return 0;
}

int mxcam_start_server_socket(MxContext *mx){
    
    ALOGD("MXCamEnc: mxcam_start_server_socket=%d", mx->phone);
    // listen and create server socket
    mx->video_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (mx->video_server_socket < 0)
    {
        ALOGE("MXCamEnc: create video server socket failed");
        return -1;
    }

    in_addr_t addr = inet_addr(mx->listen_ip);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(addr);
    server_addr.sin_port = htons(mx->video_port);
    if (bind(mx->video_server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        ALOGE("MXCamEnc: bind video server socket failed");
        close(mx->video_server_socket);
        return -1;
    }
    if (listen(mx->video_server_socket, 1) < 0)
    {
        ALOGE("MXCamEnc: listen video server socket failed");
        close(mx->video_server_socket);
        return -1;
    }

    pthread_create(&mx->video_io_worker, NULL, video_io_threadfunc, mx);
    ALOGD("MXCamEnc: video_io_threadfunc create %d\n", mx->phone);

    return 0;
}

int mxcam_handle_packet(AVFormatContext *s1, AVPacket *pkt)
{

    MxContext *mx = (MxContext *)s1->priv_data;
    ALOGD("MXCamEnc: write_packet %d\n", mx->phone);

    // 将packet放到对应的list
    if (pkt->stream_index == mx->audio_stream_idx)
    {
        pthread_mutex_lock(&mx->al_mutex);
        avpriv_packet_list_put(&mx->audio_list, pkt, NULL, 0);
        pthread_cond_signal(&mx->al_cond);
        pthread_mutex_unlock(&mx->al_mutex);
    }
    else if (pkt->stream_index == mx->video_stream_idx)
    {
        pthread_mutex_lock(&mx->vl_mutex);
        avpriv_packet_list_put(&mx->video_list, pkt, NULL, 0);
        pthread_cond_signal(&mx->vl_cond);
        pthread_mutex_unlock(&mx->vl_mutex);
    }
    else
    {
        ALOGE("MXCamEnc: unknown stream index %d\n", pkt->stream_index);
        return AVERROR(EINVAL);
    }
    return 0;
}

int mxcam_close_pipes(MxContext *mx)
{
    return 0;
}