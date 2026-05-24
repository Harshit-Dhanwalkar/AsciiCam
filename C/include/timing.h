#ifndef TIMING_H
#define TIMING_H

#include <time.h>

typedef struct {
  long samples[16]; // frame duration (ns) ring buffer
  int head;
  int count;
} fps_counter_t;

// Initialize framerate control
void timing_init(int fps);

void timing_sleep(struct timespec *start_time);

void fps_push(fps_counter_t *fc, long elapsed_ns);

double fps_get(const fps_counter_t *fc);

#endif
