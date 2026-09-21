#ifndef NL_TIME_H
#define NL_TIME_H

#ifdef __LINUX_NOLIBC__
struct timespec {
  long tv_sec;
  long tv_nsec;
};

#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME 0
#define clockid_t int

#else
#include <time.h>
#endif

#endif /* NL_TIME_H */
