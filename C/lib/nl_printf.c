#include "nolibc.h"

#include <stdarg.h>
#include <stddef.h>

static int _uint_to_dec(char *out, size_t avail, unsigned long long v) {
  char tmp[24];
  int len = 0;
  if (v == 0) {
    tmp[len++] = '0';
  } else {
    while (v) {
      tmp[len++] = '0' + (int)(v % 10);
      v /= 10;
    }
  }
  int w = 0;
  for (int i = len - 1; i >= 0 && (size_t)w < avail; i--)
    out[w++] = tmp[i];
  return w;
}

static int _uint_to_hex(char *out, size_t avail, unsigned long long v) {
  static const char hx[] = "0123456789abcdef";
  char tmp[20];
  int len = 0;
  if (v == 0) {
    tmp[len++] = '0';
  } else {
    while (v) {
      tmp[len++] = hx[v & 0xF];
      v >>= 4;
    }
  }
  int w = 0;
  for (int i = len - 1; i >= 0 && (size_t)w < avail; i--)
    out[w++] = tmp[i];
  return w;
}

int nl_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  if (!buf || size == 0)
    return 0;
  size_t pos = 0;

#define PUT(c)                                                                 \
  do {                                                                         \
    char _ch = (c);                                                            \
    if (pos + 1 < size)                                                        \
      buf[pos++] = _ch;                                                        \
  } while (0)

  while (*fmt) {
    if (*fmt != '%') {
      char ch = *fmt;
      fmt++;
      PUT(ch);
      continue;
    }
    fmt++; // skip '%'

    int zero_pad = 0, left_align = 0;
    while (*fmt == '0' || *fmt == '-') {
      if (*fmt == '0')
        zero_pad = 1;
      if (*fmt == '-')
        left_align = 1;
      fmt++;
    }
    (void)zero_pad;
    (void)left_align;

    int width = 0;
    while (*fmt >= '0' && *fmt <= '9')
      width = width * 10 + (*fmt++ - '0');

    switch (*fmt++) {
    case 'd': {
      long long v = (long long)va_arg(ap, int);
      char tmp[24];
      int n = 0;
      if (v < 0) {
        PUT('-');
        v = -v;
      }
      n = _uint_to_dec(tmp, sizeof(tmp), (unsigned long long)v);
      for (int i = n; i < width && pos + 1 < size; i++)
        PUT(' ');
      for (int i = 0; i < n && pos + 1 < size; i++)
        PUT(tmp[i]);
      break;
    }
    case 'u': {
      unsigned long long v = (unsigned long long)va_arg(ap, unsigned int);
      char tmp[24];
      int n = _uint_to_dec(tmp, sizeof(tmp), v);
      for (int i = n; i < width && pos + 1 < size; i++)
        PUT(' ');
      for (int i = 0; i < n && pos + 1 < size; i++)
        PUT(tmp[i]);
      break;
    }
    case 'x': {
      unsigned long long v = (unsigned long long)va_arg(ap, unsigned int);
      char tmp[20];
      int n = _uint_to_hex(tmp, sizeof(tmp), v);
      for (int i = 0; i < n && pos + 1 < size; i++)
        PUT(tmp[i]);
      break;
    }
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      int slen = 0;
      while (s[slen])
        slen++;
      for (int i = slen; i < width && pos + 1 < size; i++)
        PUT(' ');
      for (int i = 0; i < slen && pos + 1 < size; i++)
        PUT(s[i]);
      break;
    }
    case 'c':
      PUT((char)va_arg(ap, int));
      break;
    case '%':
      PUT('%');
      break;
    case 'l':
      if (*fmt == 'd') {
        fmt++;
        long long v = va_arg(ap, long long);
        char tmp[24];
        int n = 0;
        if (v < 0) {
          PUT('-');
          v = -v;
        }
        n = _uint_to_dec(tmp, sizeof(tmp), (unsigned long long)v);
        for (int i = 0; i < n && pos + 1 < size; i++)
          PUT(tmp[i]);
      } else if (*fmt == 'u') {
        fmt++;
        unsigned long long v = va_arg(ap, unsigned long long);
        char tmp[24];
        int n = _uint_to_dec(tmp, sizeof(tmp), v);
        for (int i = 0; i < n && pos + 1 < size; i++)
          PUT(tmp[i]);
      }
      break;
    default:
      PUT('?');
      break;
    }
  }
#undef PUT
  buf[pos] = '\0';
  return (int)pos;
}

int nl_snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = nl_vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return r;
}

void nl_eprint(const char *msg) {
  nl_write(2, msg, nl_strlen(msg));
}
