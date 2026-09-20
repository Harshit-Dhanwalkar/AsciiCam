#ifndef NL_PRINTF_H
#define NL_PRINTF_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __LINUX_NOLIBC__

#include "nl_string.h"

int nl_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int nl_snprintf(char *buf, size_t size, const char *fmt, ...);
void nl_eprint(const char *msg);

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

#else

#include <stdio.h>

#define nl_vsnprintf vsnprintf
#define nl_snprintf snprintf

static inline void nl_eprint(const char *msg) {
    fputs(msg, stderr);
}

static inline int nl_fmt_fps(char *buf, size_t sz, double fps) {
    int whole = (int)fps;
    int frac = (int)((fps - whole) * 10.0 + 0.5);
    if (frac >= 10) {
        whole++;
        frac = 0;
    }
    return snprintf(buf, sz, "%d.%d", whole, frac);
}

#endif

#endif /* NL_PRINTF_H */
