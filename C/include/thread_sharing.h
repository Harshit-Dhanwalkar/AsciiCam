#include <pthread.h>
#include <stdint.h>
#include "ascii.h"

typedef struct {
  uint8_t *buf[2]; // Duble buffer
  int width, height;
  int ascii_w, ascii_h;
  int ready_idx;
  int has_frame;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  volatile int stop;

  ascii_opts_t opts;
} shared_frame_t;

