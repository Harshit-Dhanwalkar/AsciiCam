#ifdef PLATFORM_LINUX

#include "nolibc.h"

#include "nl_types.h"
#include "nl_v4l2.h"

#include "ascii.h"
#include "capture.h"
#include "platform.h"

typedef struct webcam_impl webcam_impl_t;

// Few buffers so driver always has somewhere to write while converting and
// render one being held; capture_frame drains to newest
#define NBUFS 4

struct webcam_impl {
  struct v4l2_buffer buf_info; // buffer currently dequeued by capture_frame
  void *maps[NBUFS];
  uint32_t lens[NBUFS];
  int nbufs;
  int auto_exposure_disabled;
  int auto_wb_disabled;
  int ae_prio_changed; // cleared AUTO_PRIORITY, restore it on cleanup
  int ae_prio_saved;
};

static webcam_impl_t _impl_storage;

static int v4l2_get_value(int fd, unsigned int id, int *value);
static int v4l2_set_value(int fd, unsigned int id, int value);

static void unmap_all(webcam_impl_t *im) {
  for (int i = 0; i < im->nbufs; i++) {
    if (im->maps[i] && im->maps[i] != MAP_FAILED) {
      munmap(im->maps[i], im->lens[i]);
    }

    im->maps[i] = (void *)0;
  }

  im->nbufs = 0;
}

static int init_fail(webcam_t *cam) {
  unmap_all(cam->impl);
  close(cam->fd);

  cam->fd = -1;
  cam->buffer = MAP_FAILED;

  return -1;
}

int webcam_init(webcam_t *cam, const char *device, int width, int height) {
  nl_memset(&_impl_storage, 0, sizeof(_impl_storage));
  cam->impl = &_impl_storage;
  cam->buffer = MAP_FAILED;
  cam->stride = 0;

  // Open device non-blocking (for select)
  cam->fd = open(device ? device : "/dev/video0", O_RDWR | O_NONBLOCK, 0);
  if (cam->fd < 0) {
    return -1;
  }

  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = (unsigned)width;
  fmt.fmt.pix.height = (unsigned)height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
    return init_fail(cam);
  }

  // S_FMT may silently substitute another format; everything downstream
  // (gray, rgb) assumes packed YUYV, so refuse anything else
  if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
    errno = 22; // EINVAL

    return init_fail(cam);
  }

  // Rows may be padded: honour bytesperline instead of assuming width * 2
  cam->stride = (int)fmt.fmt.pix.bytesperline;
  if (cam->stride < cam->width * 2) {
    cam->stride = cam->width * 2;
  }

  cam->width = (int)fmt.fmt.pix.width;
  cam->height = (int)fmt.fmt.pix.height;

  struct v4l2_requestbuffers req = {0};
  req.count = NBUFS;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
    return init_fail(cam);
  }

  int want = (req.count > NBUFS) ? NBUFS : (int)req.count;
  for (int i = 0; i < want; i++) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = (uint32_t)i;

    if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
      return init_fail(cam);
    }

    void *m = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                   cam->fd, (long)buf.m.offset);
    if (m == MAP_FAILED) {
      return init_fail(cam);
    }

    cam->impl->maps[i] = m;
    cam->impl->lens[i] = buf.length;
    cam->impl->nbufs = i + 1;

    if (ioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
      return init_fail(cam);
    }
  }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
    return init_fail(cam);
  }

  // NOTE: In auto-exposure many UVC cameras trade frame rate for exposure
  // time when it is dim: 30 fps silently becomes ~7 fps with long, smeary
  // exposures Ask for a constant frame rate instead; previous value is put
  // back in webcam_cleanup so other apps are not affected
  int prio;
  if (v4l2_get_value(cam->fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY, &prio) == 0 &&
      prio != 0 &&
      v4l2_set_value(cam->fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0) == 0) {
    cam->impl->ae_prio_saved = prio;
    cam->impl->ae_prio_changed = 1;
  }

  return 0;
}

int webcam_wait_frame(const webcam_t *cam, int timeout_ms) {
  nl_fd_set fds;
  struct nl_timeval tv;

  NL_FD_ZERO(&fds);
  NL_FD_SET(cam->fd, &fds);

  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (long)(timeout_ms % 1000) * 1000L;

  int ret = nl_select(cam->fd + 1, &fds, (nl_fd_set *)0, (nl_fd_set *)0, &tv);

  return (ret <= 0) ? -1 : 0;
}

static int dq(const webcam_t *cam, struct v4l2_buffer *buf) {
  nl_memset(buf, 0, sizeof(*buf));

  buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf->memory = V4L2_MEMORY_MMAP;

  return ioctl(cam->fd, VIDIOC_DQBUF, buf);
}

int webcam_capture_frame(webcam_t *cam, uint8_t *gray_buffer) {
  webcam_impl_t *im = cam->impl;

  struct v4l2_buffer buf;
  if (dq(cam, &buf) < 0) {
    return -1;
  }

  struct v4l2_buffer newer;
  while (dq(cam, &newer) == 0) {
    ioctl(cam->fd, VIDIOC_QBUF, &buf);
    buf = newer;
  }

  if (buf.index >= (uint32_t)im->nbufs) {
    ioctl(cam->fd, VIDIOC_QBUF, &buf);

    return -1;
  }

  uint32_t full = (uint32_t)cam->stride * (uint32_t)cam->height;
  if ((buf.flags & V4L2_BUF_FLAG_ERROR) ||
      (buf.bytesused != 0 && buf.bytesused < full)) {
    ioctl(cam->fd, VIDIOC_QBUF, &buf);

    return 1;
  }

  im->buf_info = buf;
  cam->buffer = im->maps[buf.index];

  const uint8_t *src = (const uint8_t *)cam->buffer;
  if (cam->stride == cam->width * 2) {
    yuyv_to_gray_simd(src, gray_buffer, cam->width, cam->height);
  } else {
    for (int y = 0; y < cam->height; y++) {
      yuyv_to_gray_simd(src + (size_t)y * (size_t)cam->stride,
                        gray_buffer + (size_t)y * (size_t)cam->width,
                        cam->width, 1);
    }
  }

  return 0;
}

int webcam_requeue_buffer(webcam_t *cam) {
  return (ioctl(cam->fd, VIDIOC_QBUF, &cam->impl->buf_info) < 0) ? -1 : 0;
}

void webcam_cleanup(webcam_t *cam) {
  if (cam->fd >= 0) {
    if (cam->impl && cam->impl->ae_prio_changed) {
      v4l2_set_value(cam->fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY,
                     cam->impl->ae_prio_saved);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    if (cam->impl) {
      unmap_all(cam->impl);
    }

    close(cam->fd);
  }

  cam->fd = -1;
  cam->buffer = MAP_FAILED;
  cam->impl = (webcam_impl_t *)0;
}

// Hardware controls (V4L2_CID_*)
static int v4l2_query_range(int fd, unsigned int id, int *min, int *max) {
  struct v4l2_queryctrl q;
  nl_memset(&q, 0, sizeof(q));
  q.id = id;
  if (ioctl(fd, VIDIOC_QUERYCTRL, &q) < 0) {
    return -1;
  }
  if (q.flags & V4L2_CTRL_FLAG_DISABLED) {
    return -1;
  }
  if (min) {
    *min = q.minimum;
  }
  if (max) {
    *max = q.maximum;
  }

  return 0;
}

static int v4l2_get_value(int fd, unsigned int id, int *value) {
  struct v4l2_control c;
  nl_memset(&c, 0, sizeof(c));
  c.id = id;
  if (ioctl(fd, VIDIOC_G_CTRL, &c) < 0) {
    return -1;
  }
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

// cppcheck-suppress constParameterPointer
int webcam_set_auto_exposure(const webcam_t *cam, int enable) {
  if (!cam || cam->fd < 0) {
    return -1;
  }

  // NOTE: UVC drivers expose V4L2_CID_EXPOSURE_AUTO as a menu (0=manual,
  // 1=aperture priority, 3=auto, driver-dependent which subset exists).
  if (v4l2_set_value(cam->fd, V4L2_CID_EXPOSURE_AUTO,
                     enable ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL) == 0) {
    return 0;
  }

  return v4l2_set_value(cam->fd, V4L2_CID_AUTOGAIN, enable ? 1 : 0);
}

// cppcheck-suppress constParameterPointer
int webcam_set_auto_white_balance(const webcam_t *cam, int enable) {
  if (!cam || cam->fd < 0) {
    return -1;
  }

  return v4l2_set_value(cam->fd, V4L2_CID_AUTO_WHITE_BALANCE, enable ? 1 : 0);
}

int webcam_get_exposure(const webcam_t *cam, int *value) {
  if (!cam || cam->fd < 0 || !value) {
    return -1;
  }

  return v4l2_get_value(cam->fd, V4L2_CID_EXPOSURE_ABSOLUTE, value);
}

int webcam_get_contrast(const webcam_t *cam, int *value) {
  if (!cam || cam->fd < 0 || !value) {
    return -1;
  }

  return v4l2_get_value(cam->fd, V4L2_CID_CONTRAST, value);
}

int webcam_get_white_balance(const webcam_t *cam, int *value) {
  if (!cam || cam->fd < 0 || !value) {
    return -1;
  }

  return v4l2_get_value(cam->fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, value);
}

int webcam_get_exposure_range(const webcam_t *cam, int *min, int *max) {
  if (!cam || cam->fd < 0) {
    return -1;
  }

  return v4l2_query_range(cam->fd, V4L2_CID_EXPOSURE_ABSOLUTE, min, max);
}

int webcam_get_contrast_range(const webcam_t *cam, int *min, int *max) {
  if (!cam || cam->fd < 0) {
    return -1;
  }

  return v4l2_query_range(cam->fd, V4L2_CID_CONTRAST, min, max);
}

int webcam_get_white_balance_range(const webcam_t *cam, int *min, int *max) {
  if (!cam || cam->fd < 0) {
    return -1;
  }

  return v4l2_query_range(cam->fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, min,
                          max);
}

int webcam_adjust_exposure(const webcam_t *cam, int delta, int *out_value) {
  if (!cam || cam->fd < 0) {
    return -1;
  }

  if (!cam->impl->auto_exposure_disabled) {
    webcam_set_auto_exposure(cam, 0);
    cam->impl->auto_exposure_disabled = 1;
  }

  int min;
  int max;
  int cur;
  if (v4l2_query_range(cam->fd, V4L2_CID_EXPOSURE_ABSOLUTE, &min, &max) < 0) {
    return -1;
  }
  if (v4l2_get_value(cam->fd, V4L2_CID_EXPOSURE_ABSOLUTE, &cur) < 0) {
    cur = min;
  }

  int next = v4l2_clamp(cur + delta, min, max);
  if (v4l2_set_value(cam->fd, V4L2_CID_EXPOSURE_ABSOLUTE, next) < 0) {
    return -1;
  }

  if (out_value) {
    *out_value = next;
  }

  return 0;
}

// cppcheck-suppress constParameterPointer
int webcam_adjust_contrast(const webcam_t *cam, int delta, int *out_value) {
  if (!cam || cam->fd < 0) {
    return -1;
  }

  int min;
  int max;
  int cur;
  if (v4l2_query_range(cam->fd, V4L2_CID_CONTRAST, &min, &max) < 0) {
    return -1;
  }
  if (v4l2_get_value(cam->fd, V4L2_CID_CONTRAST, &cur) < 0) {
    cur = min;
  }

  int next = v4l2_clamp(cur + delta, min, max);
  if (v4l2_set_value(cam->fd, V4L2_CID_CONTRAST, next) < 0) {
    return -1;
  }

  if (out_value) {
    *out_value = next;
  }

  return 0;
}

int webcam_adjust_white_balance(const webcam_t *cam, int delta,
                                int *out_value) {
  if (!cam || cam->fd < 0) {
    return -1;
  }

  if (!cam->impl->auto_wb_disabled) {
    webcam_set_auto_white_balance(cam, 0);
    cam->impl->auto_wb_disabled = 1;
  }

  int min;
  int max;
  int cur;
  if (v4l2_query_range(cam->fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, &min,
                       &max) < 0) {
    return -1;
  }
  if (v4l2_get_value(cam->fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, &cur) < 0) {
    cur = min;
  }

  int next = v4l2_clamp(cur + delta, min, max);
  if (v4l2_set_value(cam->fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, next) < 0) {
    return -1;
  }

  if (out_value) {
    *out_value = next;
  }

  return 0;
}

#endif
