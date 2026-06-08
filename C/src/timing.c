#include "timing.h"

#include <time.h>

#include "nolibc.h"

static long frame_duration_ns = 0; // nanoseconds per frame

void timing_init(int fps) { frame_duration_ns = 1000000000L / fps; }

void timing_sleep(struct timespec *start_time) {
  struct timespec end_time;
  clock_gettime(CLOCK_MONOTONIC, &end_time);

  long elapsed_ns = (end_time.tv_sec - start_time->tv_sec) * 1000000000L +
                    (end_time.tv_nsec - start_time->tv_nsec);
  long sleep_ns = frame_duration_ns - elapsed_ns;
  if (sleep_ns > 0) {
    struct timespec ts = {sleep_ns / 1000000000L, sleep_ns % 1000000000L};
    while (nl_nanosleep(&ts, &ts) == -1 && errno == EINTR)
      ;
  }
}

void fps_push(fps_counter_t *fc, long ns) {
  fc->samples[fc->head % 16] = ns;
  fc->head++;
  if (fc->count < 16)
    fc->count++;
}

double fps_get(const fps_counter_t *fc) {
  if (fc->count == 0)
    return 0;

  long sum = 0;
  for (int i = 0; i < fc->count; i++) {
    sum += fc->samples[i];
  }

  return 1e9 * fc->count / sum;
}
