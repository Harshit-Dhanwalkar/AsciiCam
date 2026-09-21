#ifndef NL_TIME_H
#define NL_TIME_H

#ifdef __LINUX_NOLIBC__
struct timespec {
  long tv_sec;
  long tv_nsec;
};

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#else
#include <time.h>
#endif

#endif /* NL_TIME_H */
