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

#include "im2d.h"
#include "mxcam_dma_allocator.h"
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#define MAX_CMD_SIZE 1024
using namespace std::chrono;
using boost::asio::ip::tcp;

static inline char *av_err2str2(int errnum) {
  static char errbuf_static[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(errnum, errbuf_static, AV_ERROR_MAX_STRING_SIZE);
  return errbuf_static;
}

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

void multiply_by_volume(const float volume, int16_t *a, const size_t n) {
  constexpr int_fast32_t kDenominator = 32768;
  const int_fast32_t numerator =
      static_cast<int_fast32_t>(round(volume * kDenominator));

  if (numerator >= kDenominator) {
    return; // (numerator > kDenominator) is not expected
  } else if (numerator <= 0) {
    memset(a, 0, n * sizeof(*a));
    return; // (numerator < 0) is not expected
  }

  int16_t *end = a + n;

  // The unroll code below is to save on CPU branch instructions.
  // 8 is arbitrary chosen.

#define STEP                                                                   \
  *a = (*a * numerator + kDenominator / 2) / kDenominator;                     \
  ++a

  switch (n % 8) {
  case 7:
    goto l7;
  case 6:
    goto l6;
  case 5:
    goto l5;
  case 4:
    goto l4;
  case 3:
    goto l3;
  case 2:
    goto l2;
  case 1:
    goto l1;
  default:
    break;
  }

  while (a < end) {
    STEP;
  l7:
    STEP;
  l6:
    STEP;
  l5:
    STEP;
  l4:
    STEP;
  l3:
    STEP;
  l2:
    STEP;
  l1:
    STEP;
  }

#undef STEP
}

static inline int pipe_read_string(int fd, char *buffer, int size,
                                   int timeout_ms) {
  int bytes_read = 0;
  int ret = -1;
  steady_clock::time_point start_tp = steady_clock::now();
  steady_clock::time_point end_tp;
  while (bytes_read < size) {
    pollfd fds[] = {{
        .fd = fd,
        .events = POLLIN | POLLHUP,
        .revents = 0,
    }};

    end_tp = steady_clock::now();
    auto elapsed_time =
        duration_cast<std::chrono::milliseconds>(end_tp - start_tp);
    if (timeout_ms > 0 && elapsed_time.count() > timeout_ms) {
      ALOGE("pipe_read_data timeout");
      return -1;
    }

    ret = poll(fds, 1, timeout_ms);

    if (ret == 0) {
      continue;
    } else if (ret == -1) {
      ALOGE("error poll %d, err=%s", ret, strerror(errno));
      return -1;
    }

    if (fds[0].revents & (POLLHUP)) {
      // ALOGE("poll POLLHUP, failed");
      return -1;
    } else if (fds[0].revents & POLLIN) {
      /*
      If all file descriptors referring to the write end of a pipe have
       been closed, then an attempt to read(2) from the pipe will see
       end-of-file (read(2) will return 0).
       */
      while ((ret = read(fd, buffer + bytes_read, size - bytes_read)) > 0) {
        // ALOGD("pipe_read_string ret=%d, bytes_read=%d, size=%d", ret,
        // bytes_read,
        //       size);
        if (bytes_read + ret >= size) {
          break;
        }

        bytes_read += ret;

        if (buffer[bytes_read - 1] == '\0') {
          return bytes_read;
        }
      }

      if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        } else {
          ALOGE("read error %d, err=%s", ret, strerror(errno));
          return -1;
        }
      }

      // all reader closed..
      if (ret == 0) {
        return -1;
      }

    } else {
      ALOGE("unknown events %08x", fds[0].revents);
      return -1;
    }
  }

  ALOGE("error pipe_read_data ret=%d err=%s", ret, strerror(errno));
  return ret;
}

static inline int pipe_read_data(int fd, char *buffer, int size,
                                 int timeout_ms) {
  int bytes_read = 0;
  int ret = -1;
  steady_clock::time_point start_tp = steady_clock::now();
  steady_clock::time_point end_tp;
  while (bytes_read < size) {
    pollfd fds[] = {{
        .fd = fd,
        .events = POLLIN | POLLHUP,
        .revents = 0,
    }};

    end_tp = steady_clock::now();
    auto elapsed_time =
        duration_cast<std::chrono::milliseconds>(end_tp - start_tp);
    if (timeout_ms > 0 && elapsed_time.count() > timeout_ms) {
      ALOGE("pipe_read_data timeout");
      ret = -1;
      break;
    }

    ret = poll(fds, 1, timeout_ms);

    if (ret == 0) {
      continue;
    } else if (ret == -1) {
      ALOGE("error poll %d, err=%s", ret, strerror(errno));
      break;
    }

    if (fds[0].revents & (POLLHUP)) {
      ALOGE("poll POLLHUP, failed");
      break;
    } else if (fds[0].revents & POLLIN) {
      /*
      If all file descriptors referring to the write end of a pipe have
       been closed, then an attempt to read(2) from the pipe will see
       end-of-file (read(2) will return 0).
       */
      while ((ret = read(fd, buffer + bytes_read, size - bytes_read)) > 0) {
        ALOGD("pipe_read_data ret=%d, bytes_read=%d, size=%d", ret, bytes_read,
              size);
        bytes_read += ret;
      }

      if (bytes_read == size) {
        return size;
      }

      if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        } else {
          ALOGE("read error %d, err=%s", ret, strerror(errno));
          return -1;
        }
      }

      // all reader closed..
      if (ret == 0) {
        return -1;
      }

    } else {
      ALOGE("unknown events %08x", fds[0].revents);
      break;
    }
  }

  ALOGE("error pipe_read_data ret=%d err=%s", ret, strerror(errno));
  return ret;
}

static inline int pipe_write_data(int fd, const char *buffer, int size,
                                  int timeout_ms) {
  int bytes_written = 0;
  int ret = -1;

  steady_clock::time_point start_tp = steady_clock::now();
  steady_clock::time_point end_tp;

  while (bytes_written < size) {
    pollfd fds[] = {{
        .fd = fd,
        .events = POLLOUT | POLLHUP,
        .revents = 0,
    }};

    end_tp = steady_clock::now();
    auto elapsed_time =
        duration_cast<std::chrono::milliseconds>(end_tp - start_tp);
    if (timeout_ms > 0 && elapsed_time.count() > timeout_ms) {
      ALOGE("timeout");
      ret = -1;
      break;
    }

    ALOGD("try poll timeout=%d", timeout_ms);
    ret = poll(fds, 1, timeout_ms);
    ALOGD("poll=%d", ret);

    if (ret == 0) {
      continue;
    } else if (ret == -1) {
      ALOGE("error poll %d, err=%s", ret, strerror(errno));
      break;
    }

    if (fds[0].revents & (POLLHUP | POLLERR)) {
      ALOGE("poll POLLHUP, failed");
      break;
    } else if (fds[0].revents & POLLOUT) {
      /*
        If all file descriptors referring to the read end of a pipe have been
       closed, then a write(2) will cause a SIGPIPE signal to be generated for
       the calling process.  If the calling process is ignoring this signal,
       then write(2) fails with the error EPIPE.
       */
      while ((ret = write(fd, (char *)buffer + bytes_written,
                          size - bytes_written)) > 0) {
        ALOGD("pipe_write_data ret=%d, bytes_written=%d, size=%d", ret,
              bytes_written, size);
        bytes_written += ret;
        if (bytes_written == size) {
          return size;
        }
      }

      ALOGD("pipe_write_data ret=%d,  bytes_written=%d, size=%d, err=%d:%s",
            ret, bytes_written, size, errno, strerror(errno));
      if (errno == EAGAIN) {
        continue;
      } else if (errno == EPIPE) {
        ALOGE("noreader, write error %d, err=%s", ret, strerror(errno));
        return -1;
      } else {
        ALOGE("write error %d, err=%s", ret, strerror(errno));
        return -1;
      }
    } else {
      ALOGE("unknown events %08x", fds[0].revents);
      break;
    }
  }
  return ret;
}

class MxImage2D {
#define MAX_WIDTH 4096
#define MAX_HEIGHT 4096
#define DEFAULT_DMA_BUFFER_SIZE (MAX_WIDTH * MAX_HEIGHT * 4)
  int mDmaBufferFd1 = 0;
  int mDmaBufferFd2 = 0;

  char *mDmaBuffer1 = nullptr;
  char *mDmaBuffer2 = nullptr;

  rga_buffer_handle_t mDmaBufferHandle1 = 0;
  rga_buffer_handle_t mDmaBufferHandle2 = 0;

private:
  MxImage2D() {}
  ~MxImage2D() {}

  void destroy() {
    if (mDmaBufferHandle1) {
      releasebuffer_handle(mDmaBufferHandle1);
      mDmaBufferHandle1 = 0;
    }
    if (mDmaBufferHandle2) {
      releasebuffer_handle(mDmaBufferHandle2);
      mDmaBufferHandle2 = 0;
    }
    if (mDmaBuffer1) {
      dma_buf_free(DEFAULT_DMA_BUFFER_SIZE, &mDmaBufferFd1, mDmaBuffer1);
      mDmaBuffer1 = nullptr;
    }
    if (mDmaBuffer2) {
      dma_buf_free(DEFAULT_DMA_BUFFER_SIZE, &mDmaBufferFd2, mDmaBuffer2);
      mDmaBuffer2 = nullptr;
    }
  }

public:
  static MxImage2D *getInstance() {
    static MxImage2D instance;
    return &instance;
  }

  int initialize() {
    do {

      int ret =
          dma_buf_alloc(DMA_HEAP_DMA32_UNCACHED_PATH, DEFAULT_DMA_BUFFER_SIZE,
                        &mDmaBufferFd1, (void **)&mDmaBuffer1);
      if (ret < 0) {
        ALOGE("dma_buf_alloc failed %d", ret);
        break;
      }

      ret = dma_buf_alloc(DMA_HEAP_DMA32_UNCACHED_PATH, DEFAULT_DMA_BUFFER_SIZE,
                          &mDmaBufferFd2, (void **)&mDmaBuffer2);
      if (ret < 0) {
        ALOGE("dma_buf_alloc failed %d", ret);
        break;
      }

      mDmaBufferHandle1 =
          importbuffer_fd(mDmaBufferFd1, DEFAULT_DMA_BUFFER_SIZE);
      mDmaBufferHandle2 =
          importbuffer_fd(mDmaBufferFd2, DEFAULT_DMA_BUFFER_SIZE);

      if (mDmaBufferHandle1 <= 0) {
        ALOGE("importbuffer_fd1 failed %s", imStrError());
        break;
      }
      if (mDmaBufferHandle2 <= 0) {
        ALOGE("importbuffer_fd2 failed %s", imStrError());
        break;
      }

      return 0;
    } while (0);

    destroy();
    return -1;
  }

  /*
  * @brief process image to nv12
    dst_rotate, 1,2,3  90, 180, 270
  */
  int process_image_nv12(void *src, int src_size, int src_width, int src_height,
                         int sw_stride, int sh_stride, void **dst,
                         int dst_width, int dst_height, int dst_rotate) {
    memcpy(mDmaBuffer1, src, src_size);
    int ret = IM_STATUS_NOERROR;
    int new_width = 0;
    int new_height = 0;
    *dst = NULL;

    // check need resize
    if (src_width > dst_width || src_height > dst_height) {
      // 等比缩放, 按照最大的比例缩放
      ALOGD("resize 1 video frame from %dx%d to %dx%d", src_width, src_height,
            dst_width, dst_height);
      float scale_x = (float)src_width / dst_width;
      float scale_y = (float)src_height / dst_height;

      if (scale_x > scale_y) {
        new_width = dst_width;
        new_height = (int)(src_height / scale_x);
      } else {
        new_width = (int)(src_width / scale_y);
        new_height = dst_height;
      }

      // 找出小于new_width的,是16倍数的最大值
      new_width -= new_width % 16;
      new_height -= new_height % 4;

      ALOGD("resize 2 video frame %dx%d to %dx%d", src_width, src_height,
            new_width, new_height);

      rga_buffer_t srcImage = wrapbuffer_handle(
          mDmaBufferHandle1, src_width, src_height, RK_FORMAT_YCbCr_420_SP);
      rga_buffer_t dstImage = wrapbuffer_handle(
          mDmaBufferHandle2, new_width, new_height, RK_FORMAT_YCbCr_420_SP);
      ret = imcheck(srcImage, dstImage, {}, {});

      if (ret < 0) {
        ALOGE("imcheck failed %s", imStrError(ret));
        return ret;
      }
      ret = imresize(srcImage, dstImage);

      if (ret < 0) {
        ALOGE("imresize failed %s", imStrError(ret));
        return ret;
      }
      ALOGD("resize video frame success size=%d",
            new_width * new_height * 3 / 2);
    } else {
      new_width = src_width;
      new_height = src_height;
      memcpy(mDmaBuffer2, src, src_size);
      ALOGD("no need to resize video frame %dx%d to %dx%d", src_width,
            src_height, new_width, new_height);
    }

    // fill padding
    int topBorder = (dst_height - new_height) / 2;
    int bottomBorder = dst_height - new_height - topBorder;
    int leftBorder = (dst_width - new_width) / 2;
    int rightBorder = dst_width - new_width - leftBorder;

    ALOGD("pad video frame %dx%d to %dx%d top=%d bottom=%d left=%d right=%d",
          new_width, new_height, dst_width, dst_height, topBorder, bottomBorder,
          leftBorder, rightBorder);

    rga_buffer_t srcImage2 = wrapbuffer_handle(
        mDmaBufferHandle2, new_width, new_height, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dstImage2 = wrapbuffer_handle(
        mDmaBufferHandle1, dst_width, dst_height, RK_FORMAT_YCbCr_420_SP);

    ret = imcheck(srcImage2, dstImage2, {}, {});
    if (ret < 0) {
      ALOGE("imcheck failed %s", imStrError(ret));
      return ret;
    }

    ret = immakeBorder(srcImage2, dstImage2, topBorder, bottomBorder,
                       leftBorder, rightBorder, IM_BORDER_CONSTANT, 0);
    // ret = imresize(srcImage2, dstImage2, 1.0, 1.0, IM_HAL_TRANSFORM_FLIP_H);
    if (ret < 0) {
      ALOGE("immakeBorder failed %s", imStrError(ret));
      return ret;
    }
    ALOGD("immakeBorder success size=%d", dst_width * dst_height * 3 / 2);

    if (dst_rotate == 1) {
      rga_buffer_t srcImage3 = wrapbuffer_handle(
          mDmaBufferHandle1, dst_width, dst_height, RK_FORMAT_YCbCr_420_SP);
      rga_buffer_t dstImage3 = wrapbuffer_handle(
          mDmaBufferHandle2, dst_width, dst_height, RK_FORMAT_YCbCr_420_SP);

      ret = imcheck(srcImage3, dstImage3, {}, {});
      if (ret < 0) {
        ALOGE("imcheck failed %s", imStrError(ret));
        return ret;
      }

      ret = imrotate(srcImage3, dstImage3, IM_HAL_TRANSFORM_ROT_180);
      if (ret < 0) {
        ALOGE("imrotate failed %s", imStrError(ret));
        return ret;
      }
      ALOGD("imrotate success size=%d", dst_width * dst_height * 3 / 2);
      // memcpy(dst, mDmaBuffer2, dst_width * dst_height * 3 / 2);
      *dst = mDmaBuffer2;
    } else {
      // memcpy(dst, mDmaBuffer1, dst_width * dst_height * 3 / 2);
      *dst = mDmaBuffer1;
    }

    return dst_width * dst_height * 3 / 2;
  }
};

#define AUDIO_BYTES_PER_SECOND (44100 * 2 * 2)
#define AUDIO_CACHE_SIZE (AUDIO_BYTES_PER_SECOND * 10)
class MxCamFrameCache {
public:
  static MxCamFrameCache *getInstance() {
    static MxCamFrameCache instance;
    return &instance;
  }
#define MAX_VIDEO_FRAME_CACHE 30

private:
  std::list<AVFrame *> mVideoFrameCache;
  std::mutex mVideoMutex;
  std::condition_variable mVideoCond;

  std::mutex mAudioBufferMutex;
  std::condition_variable mAudioBufferCond;
  std::mutex mAudioBufferCondMutext;
  AVFifo *mAudioFifo = NULL;
  float mVolumn = 5.0; // 声音大小

private:
  MxCamFrameCache() {
    mAudioFifo = av_fifo_alloc2(AUDIO_CACHE_SIZE, 1, AV_FIFO_FLAG_AUTO_GROW);
  }
  ~MxCamFrameCache() {
    if (mAudioFifo) {
      av_fifo_freep2(&mAudioFifo);
      mAudioFifo = NULL;
    }
    for (auto frame : mVideoFrameCache) {
      av_frame_free(&frame);
    }
    mVideoFrameCache.clear();
  }

public:
  void addVideoFrame(AVFrame *frame) {
    std::unique_lock<std::mutex> lock(mVideoMutex);
    mVideoFrameCache.push_back(frame);
    // ALOGD("mxcam add video frame, pts=%d", frame->pts);
    if (mVideoFrameCache.size() >= MAX_VIDEO_FRAME_CACHE) {
      AVFrame *old_frame = mVideoFrameCache.front();
      mVideoFrameCache.pop_front();
      // ALOGD("mxcam video frame cache full, free oldest frame pts=%d",
      //       old_frame->pts);
      av_frame_free(&old_frame);
    }
    mVideoCond.notify_one();
  }

  AVFrame *getVideoFrame() {
    std::unique_lock<std::mutex> lock(mVideoMutex);
    if (mVideoFrameCache.empty()) {
      return NULL;
    } else if (mVideoFrameCache.size() == 1) {
      // 只有一个元素, 复制返回
      // ALOGD("mxcam get video frame, only one frame");
      AVFrame *frame = mVideoFrameCache.front();
      return av_frame_clone(frame);
    }
    AVFrame *frame = mVideoFrameCache.front();
    mVideoFrameCache.pop_front();
    // ALOGD("mxcam get video frame, pts=%d", frame->pts);
    return frame;
  }

  void add_audio_frame(AVCodecParameters *par, const uint8_t *buf, int size) {

    std::unique_lock<std::mutex> lock(mAudioBufferMutex);
    int ret = 0;
    int space = av_fifo_can_write(mAudioFifo);
    if (space < size) {
      av_fifo_drain2(mAudioFifo, size - space);
      ALOGE("av_fifo_drain2 %d bytes", size - space);
    }

    multiply_by_volume(mVolumn, (int16_t *)buf, size / 2);

    ret = av_fifo_write(mAudioFifo, buf, size);
    if (ret < 0) {
      ALOGE("audio fifo write failed %d", ret);
      return;
    }
    mAudioBufferCond.notify_one();
    int cache_size = av_fifo_can_read(mAudioFifo);
    ALOGD("audo frame total cache:%d", cache_size);
  }

  int get_audio_frame(char *buf, int audio_size, int format, int sample_rate_hz,
                      int ch) {
    std::unique_lock<std::mutex> lock(mAudioBufferMutex);

    int cache_size = av_fifo_can_read(mAudioFifo);
    ALOGD("audio size=%d/%d format=%d hz=%d ch=%d ", audio_size, cache_size,
          format, sample_rate_hz, ch);

    if (cache_size < audio_size) {
      ALOGE("audio buffer not enough, need %d bytes, but only %d bytes",
            audio_size, cache_size);
      return -1;
    }

    int bytes_read = av_fifo_read(mAudioFifo, buf, audio_size);
    if (bytes_read < 0) {
      ALOGE("audio fifo read failed %d %s", bytes_read,
            av_err2str2(bytes_read));
      return -1;
    }

    return audio_size;
  }
};

static int mxcam_add_video_packet(MxContext *mx, AVFrame *src_frame) {

  const char *x = av_get_pix_fmt_name((AVPixelFormat)src_frame->format);
  const char *fmt = x ? x : "unknown";
  int ret = 0;
  // pkt pix is drm,,copy frame frame gpu to cpu, and insert to list
  if (src_frame->format == AV_PIX_FMT_DRM_PRIME) {
    AVFrame *sw_frame = av_frame_alloc();
    if (!sw_frame) {
      ALOGE("mxcam_add_video_packet av_frame_alloc sw_frame failed");
      return -1;
    }
    /* retrieve data from GPU to CPU */
    if ((ret = av_hwframe_transfer_data(sw_frame, src_frame, 0)) != 0) {
      ALOGE("av_hwframe_transfer_data failed %s", av_err2str2(ret));
      av_frame_free(&sw_frame);
      return -1;
    }
    if ((ret = av_frame_copy_props(sw_frame, src_frame)) < 0) {
      ALOGE("av_frame_copy_props failed %s", av_err2str2(ret));
      av_frame_free(&sw_frame);
      return -1;
    }

    MxCamFrameCache::getInstance()->addVideoFrame(sw_frame);
    return 0;
  } else if (src_frame->format == AV_PIX_FMT_NV12) {
    MxCamFrameCache::getInstance()->addVideoFrame(av_frame_clone(src_frame));
    return 0;
  } else {
    ALOGE("mxcam_add_video_packet src_frame pix fmt %d:%s not drm",
          src_frame->format, fmt);
  }
  return -1;
}

class MxCamSockClient : public boost::enable_shared_from_this<MxCamSockClient>,
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
  MxCamSockClient(MxContext *mx, boost::asio::io_context &ioc)
      : mMxCtx(mx), mIoContext(ioc), mClientSocket(ioc), mTimer(ioc) {
    mLastActionTime = std::chrono::steady_clock::now();
    mReplyBuf = (char *)malloc(32 * 1024 * 1024);
    mReplyBufSize = 32 * 1024 * 1024;

    int snd_buf_size = 2 * 1024 * 1024;
    int recv_buf_size = 64 * 1024;
    mClientSocket.set_option(
        boost::asio::socket_base::send_buffer_size(snd_buf_size));
    mClientSocket.set_option(
        boost::asio::socket_base::receive_buffer_size(recv_buf_size));
  }

  ~MxCamSockClient() {
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
    mTimer.async_wait(boost::bind(&MxCamSockClient::on_timer, self,
                                  boost::asio::placeholders::error));
  }

  void start_read() {
    mClientSocket.async_read_some(
        boost::asio::buffer(mRecvBuf, MAX_CMD_SIZE),
        boost::bind(&MxCamSockClient::handle_read, shared_from_this(),
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
      snprintf(mReplyBuf, mReplyBufSize - 1,
               "dim=%dx%d pix=%d"
               " fps=%d bitrate=%d",
               mMxCtx->video_width, mMxCtx->video_height, mMxCtx->video_format,
               mMxCtx->video_fps, mMxCtx->video_bitrate);
      *reply = mReplyBuf;
      *reply_size = strlen(mReplyBuf);
      ALOGD("MXCamEnc: start video stream %s", mReplyBuf);
      return 0;
    } else if (strcmp(query_name, _cmd_stop) == 0) {
      return 0;
    } else if (strcmp(query_name, _cmd_query_vframe) == 0) {

      AVFrame *frame = MxCamFrameCache::getInstance()->getVideoFrame();
      if (!frame) {
        ALOGE("MxCamClient: get frame from packet failed");
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

      av_frame_free(&frame);

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

class MxCamSockServer {
private:
  MxContext *mMxCtx;
  boost::asio::io_context mIoContext;
  boost::asio::ip::tcp::acceptor mAcceptor;

public:
  MxCamSockServer(MxContext *mx, const char *listen_ip, int port)
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
    boost::shared_ptr<MxCamSockClient> client(
        new MxCamSockClient(mMxCtx, mIoContext));
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

/*
* https://man7.org/linux/man-pages/man3/mkfifo.3.html
  https://man7.org/linux/man-pages/man7/fifo.7.html
  https://man7.org/linux/man-pages/man7/pipe.7.html

       If all file descriptors referring to the write end of a pipe have
       been closed, then an attempt to read(2) from the pipe will see
       end-of-file (read(2) will return 0).  If all file descriptors
       referring to the read end of a pipe have been closed, then a
       write(2) will cause a SIGPIPE signal to be generated for the
       calling process.  If the calling process is ignoring this signal,
       then write(2) fails with the error EPIPE.  An application that
       uses pipe(2) and fork(2) should use suitable close(2) calls to
       close unnecessary duplicate file descriptors; this ensures that
       end-of-file and SIGPIPE/EPIPE are delivered when appropriate.
*/
class MxCamPipe {
private:
  std::string mRdFile; // fifo to read
  std::string mWtFile; // fifo to write
  int mRdFd;
  int mWtFd;

public:
  MxCamPipe(const char *rd_file, const char *wt_file)
      : mRdFile(rd_file), mWtFile(wt_file), mRdFd(-1), mWtFd(-1) {}

  ~MxCamPipe() {}

  int initialize() {
    if (access(mRdFile.c_str(), F_OK)) {
      mkfifo(mRdFile.c_str(), 0666);
      chmod(mRdFile.c_str(), 0666);
    }

    if (access(mWtFile.c_str(), F_OK)) {
      mkfifo(mWtFile.c_str(), 0666);
      chmod(mWtFile.c_str(), 0666);
    }

    auto rd_fd = open(mRdFile.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (rd_fd < 0) {
      ALOGE("MxCamPipe: open pipe(%s) failed %s", mRdFile.c_str(),
            strerror(errno));
      return false;
    }

    auto wt_fd = open(mWtFile.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (wt_fd < 0) {
      ALOGE("MxCamPipe: open pipe(%s) failed %s", mWtFile.c_str(),
            strerror(errno));
      close(rd_fd);
      return false;
    }
    mRdFd = rd_fd;
    mWtFd = wt_fd;

    int ret = fcntl(mWtFd, F_SETPIPE_SZ, 1024 * 1024);
    if (ret == -1) {
      ALOGE("set pipe size failed %s", strerror(errno));
    } else {
      ALOGD("set pipe size success");
    }

    ALOGD("initialize pipe success ctrl=%s", mRdFile.c_str());
    ALOGD("initialize pipe success reply=%s", mWtFile.c_str());

    // signal(SIGPIPE, [](int sig) { ALOGE("SIGPIPE"); });

    return 0;
  }

  void stop() {
    if (mRdFd >= 0) {
      close(mRdFd);
      mRdFd = -1;
    }
    if (mWtFd >= 0) {
      close(mWtFd);
      mWtFd = -1;
    }
  }

  int reset_pipe() {
    stop();
    return initialize();
  }

  int run_once(int timeout_ms) {
    char cmd[1024] = {0};
    int ret = pipe_read_string(mRdFd, cmd, sizeof(cmd), -1);
    if (ret <= 0) {
      ALOGE("read command failed %d", ret);
      return -1;
    }

    ALOGD("run_once command: %s", cmd);
    char *reply = NULL;
    int reply_size = 0;
    ret = handle_command(cmd, &reply, &reply_size);
    if (ret < 0) {
      ALOGE("handle command failed %d", ret);
      return -1;
    }

    static char reply_header[16] = {0};
    if (reply_size > 0) {
      snprintf(reply_header, sizeof(reply_header), "%08xok:", reply_size + 3);
    } else {
      snprintf(reply_header, sizeof(reply_header), "00000003ok\0");
    }

    ALOGD("run_once try reply_header: %s", reply_header);

    ret = pipe_write_data(mWtFd, reply_header, 11, timeout_ms);
    if (ret < 0 || ret != 11) {
      ALOGE("write header(11 bytes) failed %d", ret);
      return -1;
    }
    if (reply_size > 0) {
      ALOGD("run_once try reply data: %d bytes", reply_size);
      ret = pipe_write_data(mWtFd, reply, reply_size, timeout_ms);
      if (ret < 0 || ret != reply_size) {
        ALOGE("write data(%d bytes) failed %d", reply_size, ret);
        return -1;
      }
    }
    ALOGD("run_once ok");
    return 0;
  }

private:
  virtual int handle_command_start(const char *params, char **reply,
                                   int *reply_size) {
    return 0;
  }
  virtual int handle_command_stop(const char *params, char **reply,
                                  int *reply_size) {
    return 0;
  }
  virtual int handle_command_frame(const char *params, char **reply,
                                   int *reply_size) {
    return 0;
  }

  int handle_command(const char *command, char **reply, int *reply_size) {
    char query_name[64] = {0};
    const char *query_param = NULL;

    *reply = NULL;
    *reply_size = 0;

    if (_parse_query((const char *)command, query_name, sizeof(query_name),
                     &query_param)) {
      ALOGE("parse query failed");
      return -1;
    }
    if (strncmp(query_name, "start", 5) == 0) {
      return handle_command_start(query_param, reply, reply_size);
    } else if (strncmp(query_name, "stop", 4) == 0) {
      return handle_command_stop(query_param, reply, reply_size);
    } else if (strncmp(query_name, "frame", 5) == 0) {
      return handle_command_frame(query_param, reply, reply_size);
    }
    return -1;
  }
};

class MxCamVideoPipe : public MxCamPipe {
private:
  MxContext *mMxCtx;
  char *mReplyBuf = NULL;
  int mReplyBufSize = 0;
  int mReplyBufPos = 0;

public:
  MxCamVideoPipe(MxContext *mx, const char *rd_file, const char *wt_file)
      : MxCamPipe(rd_file, wt_file), mMxCtx(mx) {
    mReplyBuf = (char *)malloc(32 * 1024 * 1024);
    mReplyBufSize = 32 * 1024 * 1024;
    mReplyBufPos = 0;
  }

  ~MxCamVideoPipe() {}

  virtual int handle_command_start(const char *params, char **reply,
                                   int *reply_size) {
    snprintf(mReplyBuf, mReplyBufSize - 1, "dim=%dx%d pix=%d fps=%d bitrate=%d",
             mMxCtx->video_width, mMxCtx->video_height, mMxCtx->video_format,
             mMxCtx->video_fps, mMxCtx->video_bitrate);
    *reply = mReplyBuf;
    *reply_size = strlen(mReplyBuf);
    ALOGD("handle_command_start response:%d bytes %s", strlen(mReplyBuf),
          mReplyBuf);
    return 0;
  }
  virtual int handle_command_frame(const char *params, char **reply,
                                   int *reply_size) {

    int video_size = 0, format = 0, width = 0, height = 0, rotate = 0;

    int x = sscanf(params, "video=%d format=%d dim=%dx%d rotate=%d",
                   &video_size, &format, &width, &height, &rotate);
    if (x != 5) {
      ALOGE("parse query_param failed");
      *reply = NULL;
      *reply_size = 0;
      return -1;
    }

    AVFrame *frame = MxCamFrameCache::getInstance()->getVideoFrame();
    if (!frame) {
      ALOGE("MxCamClient: get frame from packet failed");
      return -1;
    }

    int frameSize = av_image_get_buffer_size((AVPixelFormat)frame->format,
                                             frame->width, frame->height, 1);
    int frame_width = frame->width;
    int frame_height = frame->height;

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

    av_frame_free(&frame);

    if (bytesCopied != frameSize) {
      ALOGE("MXCamEnc: copy video frame failed");
      return -1;
    }

    if (MxImage2D::getInstance()->process_image_nv12(
            mReplyBuf, frameSize, frame_width, frame_height, frame_width,
            frame_height, (void **)reply, width, height,
            rotate) != video_size) {
      ALOGE("process image failed");
      return -1;
    }

    *reply_size = video_size;

    return 0;
  }

  void stop() {
    if (mReplyBuf) {
      free(mReplyBuf);
      mReplyBuf = NULL;
    }
    MxCamPipe::stop();
  }
};

static void *videopipe_threadfunc(void *arg) {
  MxContext *mx = (MxContext *)arg;
  MxCamVideoPipe *pipe = (MxCamVideoPipe *)mx->mx_video_pipeserver;

  while (!mx->is_stop) {
    int ret = pipe->run_once(200);
    if (ret < 0) {
      // ALOGE("MXCamEnc: video pipe run_once failed");

      usleep(10000);
      pipe->reset_pipe();
    }
  }

  pipe->stop();
  delete pipe;
  ALOGD("MXCamEnc: video pipe thread exit");
  return 0;
}

class MxCamAudioPipe : public MxCamPipe {
private:
  MxContext *mMxCtx;

  char *mReplyBuf = NULL;
  int mReplyBufSize = 0;

public:
  MxCamAudioPipe(MxContext *mx, const char *rd_file, const char *wt_file)
      : MxCamPipe(rd_file, wt_file), mMxCtx(mx) {}

  ~MxCamAudioPipe() { stop(); }

  virtual int handle_command_frame(const char *params, char **reply,
                                   int *reply_size) {

    int audio_size = 0, format = 0, sample_rate_hz = 0, ch = 0, tt = 0;

    int x = sscanf(params, "audio=%zu format=%d hz=%d ch=%d time=%d",
                   &audio_size, &format, &sample_rate_hz, &ch, &tt);
    if (x != 5) {
      ALOGE("parse query_param failed");
      *reply = NULL;
      *reply_size = 0;
      return -1;
    }
    ALOGD("audio queryFrame: audio size=%d format=%d hz=%d ch=%d time=%d",
          audio_size, format, sample_rate_hz, ch, tt);

    if (mReplyBufSize < audio_size) {
      if (mReplyBuf) {
        free(mReplyBuf);
      }
      mReplyBuf = (char *)malloc(audio_size);
      mReplyBufSize = audio_size;
    }

    int ret = MxCamFrameCache::getInstance()->get_audio_frame(
        mReplyBuf, audio_size, format, sample_rate_hz, ch);

    if (ret < 0) {
      ALOGE("get audio frame failed %d", ret);
      return -1;
    } else if (ret != audio_size) {
      ALOGE("get audio frame size %d/%d", ret, audio_size);
      return -1;
    }

    *reply = mReplyBuf;
    *reply_size = audio_size;
    return 0;
  }

  void stop() { MxCamPipe::stop(); }
};

static void *audiopipe_threadfunc(void *arg) {
  MxContext *mx = (MxContext *)arg;

  MxCamAudioPipe *pipe = (MxCamAudioPipe *)mx->mx_audio_pipeserver;
  while (!mx->is_stop) {
    int ret = pipe->run_once(200);
    if (ret < 0) {
      usleep(10000);
      pipe->reset_pipe();
    }
  }

  pipe->stop();
  delete pipe;
  ALOGD("MxCamAudioPipe thread exit");
  return 0;
}

int mxcam_start_server(MxContext *mx) {
  ALOGD("MXCamEnc: mxcam_start_server=%d", mx->phone);
  if (MxImage2D::getInstance()->initialize() != 0) {
    ALOGE("MxImage2D initialize failed, check DMA permission/memory enought?");
    return -1;
  }
  int phone = mx->phone;
  char rd_file[256];
  char wt_file[256];

  // create audio pipe
  snprintf(rd_file, sizeof(rd_file), ACRTL_FILE, phone);
  snprintf(wt_file, sizeof(wt_file), AREPLY_FILE, phone);

  ALOGD("audiopipe_threadfunc rd_file=%s wt_file=%s", rd_file, wt_file);
  MxCamAudioPipe *pipe = new MxCamAudioPipe(mx, rd_file, wt_file);
  if (pipe->initialize() < 0) {
    ALOGE("MxCamAudioPipe initialize failed");
    delete pipe;
    return -1;
  }
  mx->mx_audio_pipeserver = pipe;
  pthread_create(&mx->audio_io_worker, NULL, audiopipe_threadfunc, mx);
  ALOGD("audiopipe_threadfunc create %d\n", mx->phone);

  // create video pipe
  snprintf(rd_file, sizeof(rd_file), VCTRL_FILE, phone);
  snprintf(wt_file, sizeof(wt_file), VREPLY_FILE, phone);

  ALOGD("videopipe_threadfunc rd_file=%s wt_file=%s", rd_file, wt_file);
  MxCamVideoPipe *vpipe = new MxCamVideoPipe(mx, rd_file, wt_file);
  if (vpipe->initialize() < 0) {
    ALOGE("MxCamAudioPipe initialize failed");
    delete vpipe;
    return -1;
  }
  mx->mx_video_pipeserver = vpipe;
  pthread_create(&mx->video_io_worker, NULL, videopipe_threadfunc, mx);
  ALOGD("MXCamEnc: videopipe_threadfunc create %d\n", mx->phone);

  return 0;
}

int mxcam_handle_packet(AVFormatContext *s1, AVPacket *pkt) {

  MxContext *mx = (MxContext *)s1->priv_data;

  // 将packet放到对应的list
  if (pkt->stream_index == mx->audio_stream_idx) {
    AVCodecParameters *par = s1->streams[pkt->stream_index]->codecpar;
    MxCamFrameCache::getInstance()->add_audio_frame(par, pkt->data, pkt->size);
    // get bytes in 1 second

    return 0;
  } else if (pkt->stream_index == mx->video_stream_idx) {
    AVCodecParameters *par = s1->streams[mx->video_stream_idx]->codecpar;
    if (par->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
      return mxcam_add_video_packet(mx, (AVFrame *)pkt->data);
    }
  }
  ALOGE("MXCamEnc: unknown stream index %d\n", pkt->stream_index);
  return AVERROR(EINVAL);
}

int mxcam_stop_server(MxContext *mx) {
  mx->is_stop = 1;
  return 0;
}

int mxcam_close_pipes(MxContext *mx) { return 0; }
