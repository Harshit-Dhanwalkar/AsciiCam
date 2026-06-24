#ifndef NL_PRINTF_H
#define NL_PRINTF_H

#include "nl_string.h"
#include <stdarg.h>
#include <stddef.h>

int nl_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int nl_snprintf(char *buf, size_t size, const char *fmt, ...);
void nl_eprint(const char *msg);
void nl_eprintf(const char *fmt, ...);

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

#endif
