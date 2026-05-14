#ifndef TIMING_H
#define TIMING_H

#include <time.h>

// Initialize framerate control
void timing_init(int fps);

void timing_sleep(struct timespec *start_time);

#endif
