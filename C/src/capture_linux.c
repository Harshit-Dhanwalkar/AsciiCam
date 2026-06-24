#ifdef PLATFORM_LINUX

#include "nolibc.h"

#include "ascii.h"
#include "capture.h"
#include "platform.h"

#include <linux/videodev2.h>
#include <stdint.h>

typedef struct webcam_impl webcam_impl_t;

struct webcam_impl {
  struct v4l2_buffer buf_info;
  int auto_exposure_disabled;
  int auto_wb_disabled;
};

static webcam_impl_t _impl_storage;

int webcam_init(webcam_t *cam, const char *device, int width, int height) {
  nl_memset(&_impl_storage, 0, sizeof(_impl_storage));
  cam->impl = &_impl_storage;
  cam->buffer = MAP_FAILED;

  // open
  cam->fd = open(device ? device : "/dev/video0", O_RDWR | O_NONBLOCK, 0);
  if (cam->fd < 0) {
    return -1;
  }

  // negotiate format
  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = (unsigned)width;
  fmt.fmt.pix.height = (unsigned)height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
    // TODO: Print fail
    close(cam->fd);
    return -1;
  }
  cam->width = (int)fmt.fmt.pix.width;
  cam->height = (int)fmt.fmt.pix.height;

  // request + query buffer
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
  // nl_printf("VIDIOC_QUERYBUF  buf.length=%u  offset=%u",
  // (unsigned)buf.length,
  //        (unsigned)buf.m.offset);

  // mmap
  cam->buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                     cam->fd, (long)buf.m.offset);
  if (cam->buffer == MAP_FAILED) {
    close(cam->fd);
    return -1;
  }

  cam->impl->buf_info = buf;

  // queue buffer + start streaming
  if (ioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
    munmap(cam->buffer, buf.length);
    close(cam->fd);
    return -1;
  }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
    // nl_printf("VIDIOC_STREAMON  reinit failure USB (EBUSY=16)");
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
  if (ret <= 0) {
    // nl_printf("webcam_wait_frame: select timeout/error");
    return -1;
  }
  return 0;
}

int webcam_capture_frame(webcam_t *cam, uint8_t *gray_buffer) {
  struct v4l2_buffer buf = cam->impl->buf_info;
  if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
    // nl_printf("webcam_capture_frame: VIDIOC_DQBUF");
    return -1;
  }
  yuyv_to_gray_simd((uint8_t *)cam->buffer, gray_buffer, cam->width,
                    cam->height);
  cam->impl->buf_info = buf;
  return 0;
}

int webcam_requeue_buffer(webcam_t *cam) {
  if (ioctl(cam->fd, VIDIOC_QBUF, &cam->impl->buf_info) < 0) {
    // nl_printf("webcam_requeue_buffer: VIDIOC_QBUF");
    return -1;
  }
  return 0;
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

// V4L2 hardware control helpers
static int v4l2_query_range(int fd, unsigned int id, int *min, int *max) {
  struct v4l2_queryctrl q;
  nl_memset(&q, 0, sizeof(q));
  q.id = id;
  if (ioctl(fd, VIDIOC_QUERYCTRL, &q) < 0)
    return -1;
  if (q.flags & V4L2_CTRL_FLAG_DISABLED)
    return -1;
  if (min)
    *min = q.minimum;
  if (max)
    *max = q.maximum;
  return 0;
}

static int v4l2_get_value(int fd, unsigned int id, int *value) {
  struct v4l2_control c;
  nl_memset(&c, 0, sizeof(c));
  c.id = id;
  if (ioctl(fd, VIDIOC_G_CTRL, &c) < 0)
    return -1;
  *value = c.value;
  return 0;
}

static int v4l2_set_value(int fd, unsigned int id, int value) {
  struct v4l2_control c;
  nl_memset(&c, 0, sizeof(c));
  c.id = id;
  c.value = value;
  return ioctl(fd, VIDIOC_S_CTRL, &c);
}

static int v4l2_clamp(int v, int lo, int hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

int webcam_set_auto_exposure(webcam_t *cam, int enable) {
  if (!cam || cam->fd < 0)
    return -1;
  if (v4l2_set_value(cam->fd, V4L2_CID_EXPOSURE_AUTO,
                     enable ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL) == 0)
    return 0;
  return v4l2_set_value(cam->fd, V4L2_CID_AUTOGAIN, enable ? 1 : 0);
}

int webcam_set_auto_white_balance(webcam_t *cam, int enable) {
  if (!cam || cam->fd < 0)
    return -1;
  return v4l2_set_value(cam->fd, V4L2_CID_AUTO_WHITE_BALANCE, enable ? 1 : 0);
}

int webcam_get_exposure(webcam_t *cam, int *value) {
  if (!cam || cam->fd < 0 || !value)
    return -1;
  return v4l2_get_value(cam->fd, V4L2_CID_EXPOSURE_ABSOLUTE, value);
}

int webcam_get_contrast(webcam_t *cam, int *value) {
  if (!cam || cam->fd < 0 || !value)
    return -1;
  return v4l2_get_value(cam->fd, V4L2_CID_CONTRAST, value);
}

int webcam_get_white_balance(webcam_t *cam, int *value) {
  if (!cam || cam->fd < 0 || !value)
    return -1;
  return v4l2_get_value(cam->fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, value);
}

int webcam_get_exposure_range(webcam_t *cam, int *min, int *max) {
  if (!cam || cam->fd < 0)
    return -1;
  return v4l2_query_range(cam->fd, V4L2_CID_EXPOSURE_ABSOLUTE, min, max);
}

int webcam_get_contrast_range(webcam_t *cam, int *min, int *max) {
  if (!cam || cam->fd < 0)
    return -1;
  return v4l2_query_range(cam->fd, V4L2_CID_CONTRAST, min, max);
}

int webcam_get_white_balance_range(webcam_t *cam, int *min, int *max) {
  if (!cam || cam->fd < 0)
    return -1;
  return v4l2_query_range(cam->fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, min,
                          max);
}

int webcam_adjust_exposure(webcam_t *cam, int delta, int *out_value) {
  if (!cam || cam->fd < 0)
    return -1;
  if (!cam->impl->auto_exposure_disabled) {
    webcam_set_auto_exposure(cam, 0);
    cam->impl->auto_exposure_disabled = 1;
  }
  int min, max, cur;
  if (v4l2_query_range(cam->fd, V4L2_CID_EXPOSURE_ABSOLUTE, &min, &max) < 0)
    return -1;
  if (v4l2_get_value(cam->fd, V4L2_CID_EXPOSURE_ABSOLUTE, &cur) < 0)
    cur = min;
  int next = v4l2_clamp(cur + delta, min, max);
  if (v4l2_set_value(cam->fd, V4L2_CID_EXPOSURE_ABSOLUTE, next) < 0)
    return -1;
  if (out_value)
    *out_value = next;
  return 0;
}

int webcam_adjust_contrast(webcam_t *cam, int delta, int *out_value) {
  if (!cam || cam->fd < 0)
    return -1;
  int min, max, cur;
  if (v4l2_query_range(cam->fd, V4L2_CID_CONTRAST, &min, &max) < 0)
    return -1;
  if (v4l2_get_value(cam->fd, V4L2_CID_CONTRAST, &cur) < 0)
    cur = min;
  int next = v4l2_clamp(cur + delta, min, max);
  if (v4l2_set_value(cam->fd, V4L2_CID_CONTRAST, next) < 0)
    return -1;
  if (out_value)
    *out_value = next;
  return 0;
}

int webcam_adjust_white_balance(webcam_t *cam, int delta, int *out_value) {
  if (!cam || cam->fd < 0)
    return -1;
  if (!cam->impl->auto_wb_disabled) {
    webcam_set_auto_white_balance(cam, 0);
    cam->impl->auto_wb_disabled = 1;
  }
  int min, max, cur;
  if (v4l2_query_range(cam->fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, &min,
                       &max) < 0)
    return -1;
  if (v4l2_get_value(cam->fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, &cur) < 0)
    cur = min;
  int next = v4l2_clamp(cur + delta, min, max);
  if (v4l2_set_value(cam->fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, next) < 0)
    return -1;
  if (out_value)
    *out_value = next;
  return 0;
}

#endif /* PLATFORM_LINUX */
