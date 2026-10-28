/*
 * tests/fake_capture.c - software camera implementing capture.h, so real
 * main loop (src/main.c) can be run end to end without a webcam
 *
 * "device" string picks behaviour:
 *   fake                 30 fps moving gradient, tightly packed rows
 *   fake:pad             same, but rows padded like some UVC drivers
 *   fake:stall=N         camera goes silent after N frames (never delivers
 *                        again), like an unplugged or wedged device
 *
 * Linked in place of src/capture_linux.c by `make e2e`
 */

#include "../lib/nolibc.h"

#include "ascii.h"
#include "capture.h"

#define MAX_W 1280
#define MAX_H 720
#define PAD_BYTES 64
#define FRAME_NS 33333333L

static uint8_t raw[(MAX_W * 2 + PAD_BYTES) * MAX_H];
static long frame_no;
static long stall_after = -1;
static struct timespec next_frame;

static int starts_with(const char *s, const char *key) {
  while (*key) {
    if (*s++ != *key++) {
      return 0;
    }
  }

  return 1;
}

static long parse_after(const char *s, const char *key) {
  size_t kl = nl_strlen(key);

  for (; *s; s++) {
    if (starts_with(s, key)) {
      long v = 0;
      for (const char *d = s + kl; *d >= '0' && *d <= '9'; d++) {
        v = v * 10 + (*d - '0');
      }

      return v;
    }
  }

  return -1;
}

static int has(const char *s, const char *key) {
  for (; *s; s++) {
    if (starts_with(s, key)) {
      return 1;
    }
  }

  return 0;
}

int webcam_init(webcam_t *cam, const char *device, int width, int height) {
  if (width > MAX_W) {
    width = MAX_W;
  }

  if (height > MAX_H) {
    height = MAX_H;
  }

  width &= ~1;

  cam->fd = 3;
  cam->width = width;
  cam->height = height;
  cam->stride = width * 2 + (device && has(device, ":pad") ? PAD_BYTES : 0);
  cam->buffer = (void *)0;
  cam->impl = (webcam_impl_t *)0;

  frame_no = 0;
  stall_after = device ? parse_after(device, "stall=") : -1;
  clock_gettime(CLOCK_MONOTONIC, &next_frame);

  return 0;
}

int webcam_wait_frame(const webcam_t *cam, int timeout_ms) {
  (void)cam;

  if (stall_after >= 0 && frame_no >= stall_after) {
    nl_usleep((unsigned)timeout_ms * 1000u);

    return -1;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  long wait_ns = (next_frame.tv_sec - now.tv_sec) * 1000000000L +
                 (next_frame.tv_nsec - now.tv_nsec);
  if (wait_ns > 0) {
    if (wait_ns > (long)timeout_ms * 1000000L) {
      nl_usleep((unsigned)timeout_ms * 1000u);

      return -1;
    }

    nl_usleep((unsigned)(wait_ns / 1000));
  }

  return 0;
}

int webcam_capture_frame(webcam_t *cam, uint8_t *gray) {
  long t = frame_no++;

  next_frame.tv_nsec += FRAME_NS;
  if (next_frame.tv_nsec >= 1000000000L) {
    next_frame.tv_nsec -= 1000000000L;
    next_frame.tv_sec++;
  }

  // moving diagonal gradient with some colour
  for (int y = 0; y < cam->height; y++) {
    uint8_t *row = raw + (size_t)y * (size_t)cam->stride;

    for (int x = 0; x < cam->width; x += 2) {
      row[x * 2 + 0] = (uint8_t)(x + y + t * 3);     // Y0
      row[x * 2 + 1] = (uint8_t)(96 + (y & 63));     // U
      row[x * 2 + 2] = (uint8_t)(x + 1 + y + t * 3); // Y1
      row[x * 2 + 3] = (uint8_t)(160 - (x & 63));    // V
    }
  }

  cam->buffer = raw;

  for (int y = 0; y < cam->height; y++) {
    yuyv_to_gray_simd(raw + (size_t)y * (size_t)cam->stride,
                      gray + (size_t)y * (size_t)cam->width, cam->width, 1);
  }

  return 0;
}

int webcam_requeue_buffer(webcam_t *cam) {
  (void)cam;

  return 0;
}

void webcam_cleanup(webcam_t *cam) {
  cam->fd = -1;
  cam->buffer = MAP_FAILED;
}

// no hardware controls
int webcam_set_auto_exposure(const webcam_t *c, int e) {
  (void)c, (void)e;

  return -1;
}

int webcam_set_auto_white_balance(const webcam_t *c, int e) {
  (void)c, (void)e;

  return -1;
}

int webcam_adjust_exposure(const webcam_t *c, int d, int *o) {
  (void)c, (void)d, (void)o;

  return -1;
}

int webcam_adjust_contrast(const webcam_t *c, int d, int *o) {
  (void)c, (void)d, (void)o;

  return -1;
}

int webcam_adjust_white_balance(const webcam_t *c, int d, int *o) {
  (void)c, (void)d, (void)o;

  return -1;
}

int webcam_get_exposure(const webcam_t *c, int *v) {
  (void)c, (void)v;

  return -1;
}

int webcam_get_contrast(const webcam_t *c, int *v) {
  (void)c, (void)v;

  return -1;
}

int webcam_get_white_balance(const webcam_t *c, int *v) {
  (void)c, (void)v;

  return -1;
}

int webcam_get_exposure_range(const webcam_t *c, int *a, int *b) {
  (void)c, (void)a, (void)b;

  return -1;
}

int webcam_get_contrast_range(const webcam_t *c, int *a, int *b) {
  (void)c, (void)a, (void)b;

  return -1;
}

int webcam_get_white_balance_range(const webcam_t *c, int *a, int *b) {
  (void)c, (void)a, (void)b;

  return -1;
}
