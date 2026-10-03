#ifndef NL_V4L2_H
#define NL_V4L2_H

/*
 * nl_v4l2.h - minimal Linux V4L2 kernel ABI
 */

#include "nl_types.h"

#ifdef __LINUX_NOLIBC__

/* _IOC encoding (asm-generic, shared by x86-64 and arm64) */
#define NL_IOC_NRSHIFT 0
#define NL_IOC_TYPESHIFT 8
#define NL_IOC_SIZESHIFT 16
#define NL_IOC_DIRSHIFT 30
#define NL_IOC_WRITE 1U
#define NL_IOC_READ 2U
#define NL_IOC(dir, type, nr, size)                                            \
  (((unsigned long)(dir) << NL_IOC_DIRSHIFT) |                                 \
   ((unsigned long)(type) << NL_IOC_TYPESHIFT) |                               \
   ((unsigned long)(nr) << NL_IOC_NRSHIFT) |                                   \
   ((unsigned long)(size) << NL_IOC_SIZESHIFT))
#define NL_IOW(type, nr, t) NL_IOC(NL_IOC_WRITE, type, nr, sizeof(t))
#define NL_IOWR(type, nr, t)                                                   \
  NL_IOC(NL_IOC_READ | NL_IOC_WRITE, type, nr, sizeof(t))

/* Enums / constants */
enum v4l2_buf_type { V4L2_BUF_TYPE_VIDEO_CAPTURE = 1 };
enum v4l2_memory { V4L2_MEMORY_MMAP = 1 };
enum v4l2_field { V4L2_FIELD_NONE = 1 };

#define V4L2_FOURCC(a, b, c, d)                                                \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |              \
   ((uint32_t)(d) << 24))
#define V4L2_PIX_FMT_YUYV V4L2_FOURCC('Y', 'U', 'Y', 'V')

#define V4L2_CTRL_FLAG_DISABLED 0x0001

#define V4L2_CID_BASE 0x00980900
#define V4L2_CID_CAMERA_CLASS_BASE 0x009a0900
#define V4L2_CID_CONTRAST (V4L2_CID_BASE + 1)
#define V4L2_CID_AUTO_WHITE_BALANCE (V4L2_CID_BASE + 12)
#define V4L2_CID_AUTOGAIN (V4L2_CID_BASE + 18)
#define V4L2_CID_WHITE_BALANCE_TEMPERATURE (V4L2_CID_BASE + 26)
#define V4L2_CID_EXPOSURE_AUTO (V4L2_CID_CAMERA_CLASS_BASE + 1)
#define V4L2_CID_EXPOSURE_ABSOLUTE (V4L2_CID_CAMERA_CLASS_BASE + 2)
#define V4L2_CID_EXPOSURE_AUTO_PRIORITY (V4L2_CID_CAMERA_CLASS_BASE + 3)

#define V4L2_BUF_FLAG_ERROR 0x00000040

enum v4l2_exposure_auto_type {
  V4L2_EXPOSURE_AUTO = 0,
  V4L2_EXPOSURE_MANUAL = 1
};

/* Structures */
struct v4l2_pix_format {
  uint32_t width;
  uint32_t height;
  uint32_t pixelformat;
  uint32_t field;
  uint32_t bytesperline;
  uint32_t sizeimage;
  uint32_t colorspace;
  uint32_t priv;
  uint32_t flags;
  uint32_t ycbcr_enc; /* anonymous union with hsv_enc in the kernel */
  uint32_t quantization;
  uint32_t xfer_func;
};

struct v4l2_format {
  uint32_t type;
  union {
    struct v4l2_pix_format pix;
    uint8_t raw_data[200];
    void *_align; /* the kernel union holds pointers (v4l2_window): 8-aligned */
  } fmt;
};

struct v4l2_requestbuffers {
  uint32_t count;
  uint32_t type;
  uint32_t memory;
  uint32_t capabilities;
  uint8_t flags;
  uint8_t reserved[3];
};

struct v4l2_timecode {
  uint32_t type;
  uint32_t flags;
  uint8_t frames;
  uint8_t seconds;
  uint8_t minutes;
  uint8_t hours;
  uint8_t userbits[4];
};

struct v4l2_buffer {
  uint32_t index;
  uint32_t type;
  uint32_t bytesused;
  uint32_t flags;
  uint32_t field;
  struct {
    long tv_sec;
    long tv_usec;
  } timestamp;
  struct v4l2_timecode timecode;
  uint32_t sequence;
  uint32_t memory;
  union {
    uint32_t offset;
    unsigned long userptr;
    void *planes;
    int32_t fd;
  } m;
  uint32_t length;
  uint32_t reserved2;
  union {
    int32_t request_fd;
    uint32_t reserved;
  };
};

struct v4l2_control {
  uint32_t id;
  int32_t value;
};

struct v4l2_queryctrl {
  uint32_t id;
  uint32_t type;
  uint8_t name[32];
  int32_t minimum;
  int32_t maximum;
  int32_t step;
  int32_t default_value;
  uint32_t flags;
  uint32_t reserved[2];
};

/* ioctl request codes ('V' = 0x56) */
#define VIDIOC_S_FMT NL_IOWR('V', 5, struct v4l2_format)
#define VIDIOC_REQBUFS NL_IOWR('V', 8, struct v4l2_requestbuffers)
#define VIDIOC_QUERYBUF NL_IOWR('V', 9, struct v4l2_buffer)
#define VIDIOC_QBUF NL_IOWR('V', 15, struct v4l2_buffer)
#define VIDIOC_DQBUF NL_IOWR('V', 17, struct v4l2_buffer)
#define VIDIOC_STREAMON NL_IOW('V', 18, int)
#define VIDIOC_STREAMOFF NL_IOW('V', 19, int)
#define VIDIOC_G_CTRL NL_IOWR('V', 27, struct v4l2_control)
#define VIDIOC_S_CTRL NL_IOWR('V', 28, struct v4l2_control)
#define VIDIOC_QUERYCTRL NL_IOWR('V', 36, struct v4l2_queryctrl)

/* ABI pins: sizes are baked into the VIDIOC_* numbers above */
_Static_assert(sizeof(struct v4l2_pix_format) == 48, "v4l2_pix_format");
_Static_assert(sizeof(struct v4l2_format) == 208, "v4l2_format");
_Static_assert(sizeof(struct v4l2_requestbuffers) == 20, "v4l2_requestbuffers");
_Static_assert(sizeof(struct v4l2_buffer) == 88, "v4l2_buffer");
_Static_assert(sizeof(struct v4l2_control) == 8, "v4l2_control");
_Static_assert(sizeof(struct v4l2_queryctrl) == 68, "v4l2_queryctrl");

#endif /* __LINUX_NOLIBC__ */

#endif /* NL_V4L2_H */
