#ifndef NL_PRINTF_H
#define NL_PRINTF_H

#include "nl_io.h"
#include "nl_string.h"
#include <stdarg.h>
#include <stddef.h>

int nl_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int nl_snprintf(char *buf, size_t size, const char *fmt, ...);

static inline void nl_eprint(const char *msg) {
  nl_write(2, msg, nl_strlen(msg));
}

static inline int nl_fmt_fps(char *buf, size_t sz, double fps) {
  int whole = (int)fps;
  int frac = (int)((fps - whole) * 10.0 + 0.5);
  if (frac >= 10) {
    whole++;
    frac = 0;
  }
  return nl_snprintf(buf, sz, "%d.%d", whole, frac);
}

#define snprintf nl_snprintf

#define stderr 2
#define fprintf(fd, fmt, ...)                                                  \
  do {                                                                         \
    char _fb[1024];                                                            \
    int _fn = nl_snprintf(_fb, sizeof(_fb), fmt, ##__VA_ARGS__);               \
    if (_fn > 0) {                                                             \
      size_t _nwrite = (_fn < (int)sizeof(_fb) - 1) ? _fn : sizeof(_fb) - 1;   \
      nl_write((int)(long)(fd), _fb, _nwrite);                                 \
    }                                                                          \
  } while (0)
#endif
