#ifndef THREAD_SHARING_H
#define THREAD_SHARING_H

/*
 * Capture thread: pulls frames from camera, converts them to luma and hands
 * newest one to render loop through a frame_exchange_t triple buffer. Camera
 * waits and YUYV->luma conversion run off main loop, so a stalled camera no
 * longer blocks keyboard/mouse handling and capture overlaps with rendering
 *
 * Camera must already be open; thread only calls webcam_wait_frame /
 * webcam_capture_frame / webcam_requeue_buffer on it. Hardware-control ioctls
 * from main thread are fine (they only use fd)
 */

#include "capture.h"
#include "frame_exchange.h"
#include "nl_thread.h"
#include "nl_types.h"

typedef struct {
  uint8_t *gray; // width*height luma (+ slack for SIMD tail)
  uint8_t *yuyv; // copy of raw camera frame; NULL if not requested
  int has_yuyv;  // yuyv holds this frame's raw data
} capture_frame_t;

typedef struct {
  webcam_t *cam;
  size_t gray_bytes;
  size_t yuyv_bytes;
  void *slab;
  size_t slab_bytes;
  capture_frame_t frame[FX_SLOTS];
  frame_exchange_t fx;
  nl_thread_t thread;
  int started;
  volatile int stop;
  volatile int failed; // capture thread hit an unrecoverable error
} capture_pipe_t;

// Start capture thread. want_yuyv also copies each raw frame (needed for
// colour, which is derived from it)
// Returns 0, or -1 if memory or thread could not be obtained: caller then
// captures inline instead
int capture_pipe_start(capture_pipe_t *p, webcam_t *cam, int want_yuyv);

// Wait up to timeout_ms for a frame newer than last one returned
//   1  *out is newest frame; owned by caller until next call
//   0  nothing new yet (timeout)
//  -1  capture failed; no further frames will arrive
int capture_pipe_acquire(capture_pipe_t *p, int timeout_ms,
                         capture_frame_t **out);

// Stop and join thread and free buffers. Safe to call repeatedly
void capture_pipe_stop(capture_pipe_t *p);

#endif
