#ifndef THREAD_SHARING_H
#define THREAD_SHARING_H

#include "ascii.h"
#include "nl_thread.h"
#include "nl_types.h"

typedef struct {
  uint8_t *buf[2];      // Double buffer, one slot per thread
  int width, height;    // capture dimensions
  int ascii_w, ascii_h; // Ascii width and height
  int ready_idx;        // which slot has the freshest frame
  int has_frame;        // non-zero once producer has written once
  nl_mutex_t lock;
  nl_cond_t cond;
  volatile int stop;
  ascii_opts_t opts;
} shared_frame_t;

void *capture_thread(void *arg);
void *render_thread(void *arg);

#endif
