#include "timing.h"
#include <errno.h>
#include <time.h>

static long frame_duration_ns = 0;  // nanoseconds per frame

void timing_init(int fps) {
    frame_duration_ns = 1000000000L / fps;
}

void timing_sleep(struct timespec *start_time) {
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);

    long elapsed_ns = (end_time.tv_sec - start_time->tv_sec) * 1000000000L +
                      (end_time.tv_nsec - start_time->tv_nsec);
    long sleep_ns = frame_duration_ns - elapsed_ns;
    if (sleep_ns > 0) {
        struct timespec ts = { sleep_ns / 1000000000L, sleep_ns % 1000000000L };
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR);
    }
}
