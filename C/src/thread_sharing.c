#include "nolibc.h"

#include "capture.h"
#include "thread_sharing.h"

#define CAPTURE_WAIT_MS 50 // upper bound on how long stop takes to be noticed
#define SLOT_ALIGN 64
#define GRAY_SLACK 16 // SIMD luma conversion may touch a few bytes past end

static size_t align_up(size_t n) {
  return (n + (SLOT_ALIGN - 1)) & ~(size_t)(SLOT_ALIGN - 1);
}

// Producer
static void *capture_thread(void *arg) {
  capture_pipe_t *p = arg;
  webcam_t *cam = p->cam;

  while (!p->stop) {
    if (webcam_wait_frame(cam, CAPTURE_WAIT_MS) < 0) {
      continue; // timeout (or signal): re-check stop and wait again
    }

    int slot = fx_write_begin(&p->fx);
    capture_frame_t *f = &p->frame[slot];

    int rc = webcam_capture_frame(cam, f->gray);
    if (rc < 0) {
      fx_write_abort(&p->fx);
      p->failed = 1;

      break;
    }

    if (rc > 0) {
      fx_write_abort(&p->fx);

      continue; // corrupt frame dropped, its buffer is already requeued
    }

    // Raw frame must be copied before buffer goes back to driver:
    // colour is derived from it after plugins have changed luma
    f->has_yuyv = 0;
    if (f->yuyv && cam->buffer && cam->buffer != MAP_FAILED) {
      nl_memcpy(f->yuyv, cam->buffer, p->yuyv_bytes);
      f->has_yuyv = 1;
    }

    if (webcam_requeue_buffer(cam) < 0) {
      fx_write_abort(&p->fx);
      p->failed = 1;

      break;
    }

    fx_write_publish(&p->fx);
  }

  return NULL;
}

static void *slab_alloc(size_t n) {
#ifdef __LINUX_NOLIBC__
  // Page-backed and committed lazily: three full-size slots never compete with
  // small-block arena (2 MiB) rest of program lives in
  void *m = nl_mmap(NULL, n, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  return (m == MAP_FAILED) ? NULL : m;
#else
  return nl_malloc(n);
#endif
}

static void slab_free(void *m, size_t n) {
#ifdef __LINUX_NOLIBC__
  nl_munmap(m, n);
#else
  (void)n;
  nl_free(m);
#endif
}

int capture_pipe_start(capture_pipe_t *p, webcam_t *cam, int want_yuyv) {
  nl_memset(p, 0, sizeof(*p));

  if (!cam || cam->width <= 0 || cam->height <= 0) {
    return -1;
  }

  int stride = cam->stride > 0 ? cam->stride : cam->width * 2;

  p->cam = cam;
  p->gray_bytes =
      align_up((size_t)cam->width * (size_t)cam->height + GRAY_SLACK);
  p->yuyv_bytes = want_yuyv ? (size_t)stride * (size_t)cam->height : 0;

  size_t per_slot = p->gray_bytes + align_up(p->yuyv_bytes);
  p->slab_bytes = per_slot * FX_SLOTS;
  p->slab = slab_alloc(p->slab_bytes);
  if (!p->slab) {
    return -1;
  }

  for (int i = 0; i < FX_SLOTS; i++) {
    uint8_t *base = (uint8_t *)p->slab + (size_t)i * per_slot;

    p->frame[i].gray = base;
    p->frame[i].yuyv = want_yuyv ? base + p->gray_bytes : NULL;
  }

  fx_init(&p->fx);

  if (nl_thread_create(&p->thread, capture_thread, p) < 0) {
    slab_free(p->slab, p->slab_bytes);
    p->slab = NULL;

    return -1;
  }

  p->started = 1;

  return 0;
}

int capture_pipe_acquire(capture_pipe_t *p, int timeout_ms,
                         capture_frame_t **out) {
  // No timed condition wait in nl_thread, so poll: 1 ms granularity is far
  // below a frame period, and an idle wait costs next to nothing
  for (int waited = 0;; waited++) {
    int slot = fx_read_acquire(&p->fx);
    if (slot >= 0) {
      *out = &p->frame[slot];

      return 1;
    }

    if (p->failed) {
      return -1; // frames published before failure were delivered above
    }

    if (waited >= timeout_ms) {
      return 0;
    }

    nl_usleep(1000);
  }
}

void capture_pipe_stop(capture_pipe_t *p) {
  if (p->started) {
    p->stop = 1;
    nl_thread_join(&p->thread);
    p->started = 0;
  }

  if (p->slab) {
    slab_free(p->slab, p->slab_bytes);
    p->slab = NULL;
  }
}
