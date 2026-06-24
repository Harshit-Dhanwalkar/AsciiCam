#ifndef THREAD_SHARING_H
#define THREAD_SHARING_H

#include "ascii.h"
#include <pthread.h>
#include <stdint.h>

typedef struct {
  uint8_t *gray_buf[2]; // Double buffer, one slot per thread
  uint8_t *rgb_buf[2];  // Double buffer, when color is enabled, one slot per thread
  int width, height;    // capture dimensions
  int ascii_w, ascii_h; // Ascii width and height
  int ready_idx;        // which slot has the freshest frame
  int has_frame;        // non-zero once producer has written once
  pthread_mutex_t lock;
  pthread_cond_t cond;
  volatile int stop;
  ascii_opts_t opts;
} shared_frame_t;

void *capture_thread(void *arg);
void *render_thread(void *arg);

#endif
