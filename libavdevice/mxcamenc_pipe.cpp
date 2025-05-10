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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define BOOST_ERROR_CODE_HEADER_ONLY
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <boost/noncopyable.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/thread/thread.hpp>
#include <list>
#include <string.h>
#include <vector>

#include <chrono>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <vector>

using namespace std::chrono;
using boost::asio::ip::tcp;

typedef boost::shared_ptr<tcp::socket> socket_ptr;

static inline int _parse_query(const char *query, char *query_name,
                               int query_name_size, const char **query_param) {
  /* Extract query name. */
  const char *qend = strchr(query, ' ');
  if (qend == NULL) {
    qend = query + strlen(query);
  }
  if ((qend - query) >= query_name_size) {
    return qend - query + 1;
  }
  memcpy(query_name, query, qend - query);
  query_name[qend - query] = '\0';
  /* Calculate query parameters pointer (if needed) */
  if (query_param != NULL) {
    if (*qend == ' ') {
      qend++;
    }
    *query_param = (*qend == '\0') ? NULL : qend;
  }
  return 0;
}

#define MAX_CMD_SIZE 1024

static void mxcam_add_video_packet(MxContext *mx, AVPacket *pkt) {
  pthread_mutex_lock(&mx->vl_mutex);
  avpriv_packet_list_put(&mx->video_list, pkt, NULL, 0);
  mx->video_packet_cnt++;

  if (mx->video_packet_cnt >= 30) {
    AVPacket pkt1;
    if (avpriv_packet_list_get(&mx->video_list, &pkt1) == 0) {
      av_packet_unref(&pkt1);
    }
  }
  pthread_cond_signal(&mx->vl_cond);
  pthread_mutex_unlock(&mx->vl_mutex);
}

static AVPacket *mxcam_get_video_packet(MxContext *mx) {
  static AVPacket *last_pkt = NULL;
  int ret = 0;
  AVPacket *pkt = NULL;

  pthread_mutex_lock(&mx->vl_mutex);
  if (mx->video_packet_cnt == 1) {
    pkt = av_packet_clone(&mx->video_list.head->pkt);
  } else if (mx->video_packet_cnt > 1) {
    mx->video_packet_cnt--;
    pkt = av_packet_alloc();
    avpriv_packet_list_get(&mx->video_list, pkt);
  }
  pthread_mutex_unlock(&mx->vl_mutex);
  return pkt;
}

class MxCamClient : public boost::enable_shared_from_this<MxCamClient>,
                    private boost::noncopyable {
private:
  boost::asio::io_context &mIoContext;
  boost::asio::ip::tcp::socket mClientSocket;
  // timer
  boost::asio::steady_timer mTimer;
  char mRecvBuf[MAX_CMD_SIZE] = {0};
  int mRecvBufPos = 0;
  MxContext *mMxCtx;

  std::chrono::steady_clock::time_point mLastActionTime;
  char *mReplyBuf = NULL;
  int mReplyBufSize = 0;
  int mReplyBufPos = 0;

public:
  MxCamClient(MxContext *mx, boost::asio::io_context &ioc)
      : mMxCtx(mx), mIoContext(ioc), mClientSocket(ioc), mTimer(ioc) {
    mLastActionTime = std::chrono::steady_clock::now();
    mReplyBuf = (char *)malloc(32 * 1024 * 1024);
    mReplyBufSize = 32 * 1024 * 1024;
  }

  ~MxCamClient() {
    if (mReplyBuf) {
      free(mReplyBuf);
      mReplyBuf = NULL;
    }
  }

  tcp::socket &get_socket() { return mClientSocket; }

  void start() {
    start_timer();
    start_read();
  }

  void stop() {
    mTimer.cancel();
    if (mClientSocket.is_open())
      mClientSocket.close();
    if (mReplyBuf) {
      free(mReplyBuf);
      mReplyBuf = NULL;
    }
  }

private:
  void start_timer() {
    auto self(shared_from_this());
    mTimer.expires_after(std::chrono::seconds(5));
    mTimer.async_wait(boost::bind(&MxCamClient::on_timer, self,
                                  boost::asio::placeholders::error));
  }

  void start_read() {
    mClientSocket.async_read_some(
        boost::asio::buffer(mRecvBuf, MAX_CMD_SIZE),
        boost::bind(&MxCamClient::handle_read, shared_from_this(),
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred));
  }

  void on_timer(const boost::system::error_code &error) {
    if (!error) {
      auto now = std::chrono::steady_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::seconds>(
          now - mLastActionTime);
      if (duration.count() > 5) {
        ALOGE("MXCamEnc: client timeout");
        stop();
      } else {
        start_timer();
      }
    }
  }

  void handle_read(const boost::system::error_code &error,
                   std::size_t bytes_transferred) {
    if (!error) {
      mLastActionTime = std::chrono::steady_clock::now();
      ALOGD("MXCamEnc: read %d bytes from client: %s\n", bytes_transferred,
            mRecvBuf);
      char *reply = NULL;
      int reply_size = 0;
      if (handle_command(mRecvBuf, &reply, &reply_size) < 0) {
        ALOGE("MXCamEnc: handle command failed");
        stop();
        return;
      }

      static char reply_header[16] = {0};
      if (reply_size > 0) {
        snprintf(reply_header, sizeof(reply_header), "%08xok:", reply_size + 3);
      } else {
        snprintf(reply_header, sizeof(reply_header), "00000003ok\0");
      }

      std::vector<boost::asio::const_buffer> reply_buffers;
      reply_buffers.push_back(boost::asio::buffer(reply_header, 11));
      if (reply_size > 0) {
        reply_buffers.push_back(boost::asio::buffer(reply, reply_size));
      }

      auto self(shared_from_this());
      boost::asio::async_write(
          mClientSocket, reply_buffers,
          [self, this, reply,
           reply_size](const boost::system::error_code &error,
                       std::size_t bytes_transferred) {
            if (!error) {
              ALOGD("MxCamClient: handle_command reply %d bytes to client: "
                    "%s, reading next command",
                    bytes_transferred, mRecvBuf);
              start_read();
            } else {
              ALOGE("MXCamEnc: write failed %s", error.message().c_str());
              stop();
            }
          });
    } else {
      ALOGE("MXCamEnc: read failed %s", error.message().c_str());
      stop();
    }
  }

  int handle_command(const char *command, char **reply, int *reply_size) {
    static const char _cmd_start[] = "start";
    static const char _cmd_stop[] = "stop";
    static const char _cmd_query_vframe[] = "vframe";
    char query_name[64];
    const char *query_param = NULL;

    *reply = NULL;
    *reply_size = 0;

    if (_parse_query((const char *)command, query_name, sizeof(query_name),
                     &query_param)) {
      ALOGE("parse query failed");
      return -1;
    }

    if (strcmp(query_name, _cmd_start) == 0) {
      return 0;
    } else if (strcmp(query_name, _cmd_stop) == 0) {
      return 0;
    } else if (strcmp(query_name, _cmd_query_vframe) == 0) {

      AVPacket *packet = mxcam_get_video_packet(mMxCtx);
      if (!packet) {
        ALOGE("MXCamEnc: no packet exist. waitting");
        return 0;
      }
      AVFrame *frame = (AVFrame *)packet->data;
      if (!frame) {
        ALOGE("MxCamClient: get frame from packet failed");
        av_packet_free(&packet);
        return -1;
      }

      int frameSize = av_image_get_buffer_size((AVPixelFormat)frame->format,
                                               frame->width, frame->height, 1);

      if (frameSize > mReplyBufSize) {
        if (mReplyBuf) {
          free(mReplyBuf);
        }
        mReplyBuf = (char *)malloc(frameSize);
        mReplyBufSize = frameSize;
      }

      int bytesCopied = av_image_copy_to_buffer(
          (uint8_t *)mReplyBuf, frameSize, frame->data, frame->linesize,
          (AVPixelFormat)frame->format, frame->width, frame->height, 1);

      av_packet_free(&packet);

      if (bytesCopied != frameSize) {
        ALOGE("MXCamEnc: copy video frame failed");
        return -1;
      }

      *reply = mReplyBuf;
      *reply_size = frameSize;
      return 0;
    }

    ALOGE("unknow cmd:%s", command);
    return -1;
  }
};

class MxCamServer {
private:
  MxContext *mMxCtx;
  boost::asio::io_context mIoContext;
  boost::asio::ip::tcp::acceptor mAcceptor;

public:
  MxCamServer(MxContext *mx, const char *listen_ip, int port)
      : mMxCtx(mx),
        mAcceptor(mIoContext,
                  boost::asio::ip::tcp::endpoint(
                      boost::asio::ip::make_address(listen_ip), port)) {}

  void run() {
    start_accept();
    mIoContext.run();
  }
  void stop() { mAcceptor.close(); }

private:
  void start_accept() {
    boost::shared_ptr<MxCamClient> client(new MxCamClient(mMxCtx, mIoContext));
    mAcceptor.async_accept(
        client->get_socket(),
        [this, client](const boost::system::error_code &error) {
          if (error) {
            ALOGE("MXCamEnc: accept failed %s", error.message().c_str());
            return;
          }
          client->start();
          start_accept();
          ALOGD("MxCamServer: client incomed!!");
        });
  }
};

static void *io_threadfunc(void *arg) {
  MxContext *mx = (MxContext *)arg;

  MxCamServer *server = new MxCamServer(mx, mx->listen_ip, mx->listen_port);
  mx->mx_server = (void *)server;
  server->run();

  return 0;
}

int mxcam_start_server_socket(MxContext *mx) {

  ALOGD("MXCamEnc: mxcam_start_server_socket=%d", mx->phone);
  pthread_create(&mx->io_worker, NULL, io_threadfunc, mx);
  ALOGD("MXCamEnc: video_io_threadfunc create %d\n", mx->phone);

  return 0;
}

int mxcam_handle_packet(AVFormatContext *s1, AVPacket *pkt) {

  MxContext *mx = (MxContext *)s1->priv_data;

  // 将packet放到对应的list
  if (pkt->stream_index == mx->audio_stream_idx) {
  } else if (pkt->stream_index == mx->video_stream_idx) {
    AVCodecParameters *par = s1->streams[mx->video_stream_idx]->codecpar;
    if (par->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
      mxcam_add_video_packet(mx, pkt);
    }
  } else {
    ALOGE("MXCamEnc: unknown stream index %d\n", pkt->stream_index);
    return AVERROR(EINVAL);
  }
  return 0;
}

int mxcam_top_server(MxContext *mx) {
  if (mx->mx_server) {
    MxCamServer *server = (MxCamServer *)mx->mx_server;
    server->stop();
    delete server;
    mx->mx_server = NULL;
  }
  return 0;
}

int mxcam_close_pipes(MxContext *mx) { return 0; }