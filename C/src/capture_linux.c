#include "ascii.h"
#include "capture.h"
#include "platform.h"

#ifdef PLATFORM_LINUX

#include "nolibc.h"
#include <linux/videodev2.h>
#include <stdint.h>

typedef struct webcam_impl webcam_impl_t;

struct webcam_impl {
  struct v4l2_buffer buf_info;
};

static webcam_impl_t _impl_storage;

int webcam_init(webcam_t *cam, const char *device, int width, int height) {
  cam->impl = &_impl_storage;
  cam->buffer = MAP_FAILED;

  // Open device non-blocking (for select)
  cam->fd = open(device ? device : "/dev/video0", O_RDWR | O_NONBLOCK, 0);
  if (cam->fd < 0)
    return -1;

  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = (unsigned)width;
  fmt.fmt.pix.height = (unsigned)height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
    close(cam->fd);
    return -1;
  }

  cam->width = (int)fmt.fmt.pix.width;
  cam->height = (int)fmt.fmt.pix.height;

  struct v4l2_requestbuffers req = {0};
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
    close(cam->fd);
    return -1;
  }

  struct v4l2_buffer buf = {0};
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = 0;

  if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
    close(cam->fd);
    return -1;
  }

  cam->buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                     cam->fd, (long)buf.m.offset);
  if (cam->buffer == MAP_FAILED) {
    close(cam->fd);
    return -1;
  }

  cam->impl->buf_info = buf;

  if (ioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
    munmap(cam->buffer, buf.length);
    close(cam->fd);
    return -1;
  }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
    munmap(cam->buffer, buf.length);
    close(cam->fd);
    return -1;
  }

  return 0;
}

int webcam_wait_frame(webcam_t *cam, int timeout_ms) {
  nl_fd_set fds;
  struct nl_timeval tv;
  NL_FD_ZERO(&fds);
  NL_FD_SET(cam->fd, &fds);
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  int ret = nl_select(cam->fd + 1, &fds, (nl_fd_set *)0, (nl_fd_set *)0, &tv);
  return (ret <= 0) ? -1 : 0;
}

int webcam_capture_frame(webcam_t *cam, uint8_t *gray_buffer) {
  struct v4l2_buffer buf = cam->impl->buf_info;
  if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0)
    return -1;

  yuyv_to_gray_simd((uint8_t *)cam->buffer, gray_buffer, cam->width,
                    cam->height);

  cam->impl->buf_info = buf;
  return 0;
}

int webcam_requeue_buffer(webcam_t *cam) {
  return (ioctl(cam->fd, VIDIOC_QBUF, &cam->impl->buf_info) < 0) ? -1 : 0;
}

void webcam_cleanup(webcam_t *cam) {
  if (cam->fd >= 0) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    if (cam->buffer != MAP_FAILED)
      munmap(cam->buffer, cam->impl->buf_info.length);
    close(cam->fd);
  }
  cam->fd = -1;
  cam->buffer = MAP_FAILED;
  cam->impl = (webcam_impl_t *)0;
}

#endif /* PLATFORM_LINUX */
