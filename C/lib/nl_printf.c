// cppcheck-suppress-file variableScope
/*
 * nl_printf.c - snprintf for nolibc build
 *
 * Supported:  %d %i %u %x %X %o %c %s %p %f %F %%
 * Flags:      '-' '+' ' ' '0' '#'
 * Width:      digits or '*'
 * Precision:  '.' digits or '.*' (integers: minimum digits, %s: maximum
 *             chars, %f: digits after point; default 6)
 * Length:     hh h (none) l ll z j t
 *
 * %f/%F is exact: double is expanded to its full decimal value and then rounded
 * half-to-even, which matches glibc for every finite input. No floating point
 * arithmetic is used at all (nothing here can pull in libgcc helpers) and
 * nan/inf print as nan/inf
 *
 * One deliberate difference from C99: return value is number of bytes stored
 * (not length that would have been written had buffer been large enough)
 * Callers in this tree rely on that to advance a cursor
 */

#include "nl_types.h"
#include "nolibc.h"

typedef struct {
  char *buf;
  size_t size;
  size_t pos;
} out_t;

static void out_ch(out_t *o, char c) {
  if (o->pos + 1 < o->size) {
    o->buf[o->pos++] = c;
  }
}

static void out_rep(out_t *o, char c, int n) {
  for (int i = 0; i < n; i++) {
    out_ch(o, c);
  }
}

#define F_LEFT 1u
#define F_PLUS 2u
#define F_SPACE 4u
#define F_ZERO 8u
#define F_ALT 16u
#define F_UPPER 32u

typedef struct {
  unsigned flags;
  int width;
  int prec; // -1 when absent
} spec_t;

#define PREC_CAP 4096

// Integer conversions

static void fmt_uint(out_t *o, const spec_t *sp, unsigned long long mag,
                     int neg, int is_signed, unsigned base) {
  const char *digs =
      (sp->flags & F_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
  char tmp[24];
  int nd = 0;
  while (mag) {
    tmp[nd++] = digs[mag % base];
    mag /= base;
  }

  int is_zero = (nd == 0);
  if (sp->prec == 0 && is_zero) {
    nd = 0; // "%.0d" of 0 prints nothing
  } else if (is_zero) {
    tmp[nd++] = '0';
  }

  int zeros = sp->prec > nd ? sp->prec - nd : 0;
  char prefix[3];
  int plen = 0;
  if (is_signed) {
    if (neg) {
      prefix[plen++] = '-';
    } else if (sp->flags & F_PLUS) {
      prefix[plen++] = '+';
    } else if (sp->flags & F_SPACE) {
      prefix[plen++] = ' ';
    }
  }

  if ((sp->flags & F_ALT) && base == 16 && !is_zero) {
    prefix[plen++] = '0';
    prefix[plen++] = (sp->flags & F_UPPER) ? 'X' : 'x';
  }
  if ((sp->flags & F_ALT) && base == 8 && zeros == 0 &&
      (nd == 0 || tmp[nd - 1] != '0')) {
    zeros = 1;
  }

  int total = plen + zeros + nd;
  if ((sp->flags & F_ZERO) && !(sp->flags & F_LEFT) && sp->prec < 0 &&
      sp->width > total) {
    zeros += sp->width - total;
    total = sp->width;
  }

  int pad = sp->width > total ? sp->width - total : 0;
  if (!(sp->flags & F_LEFT)) {
    out_rep(o, ' ', pad);
  }
  for (int i = 0; i < plen; i++) {
    out_ch(o, prefix[i]);
  }

  out_rep(o, '0', zeros);
  for (int i = nd - 1; i >= 0; i--) {
    out_ch(o, tmp[i]);
  }
  if (sp->flags & F_LEFT) {
    out_rep(o, ' ', pad);
  }
}

// Strings and characters

static void fmt_str(out_t *o, const spec_t *sp, const char *s, int slen) {
  int pad = sp->width > slen ? sp->width - slen : 0;
  if (!(sp->flags & F_LEFT)) {
    out_rep(o, ' ', pad);
  }
  for (int i = 0; i < slen; i++) {
    out_ch(o, s[i]);
  }
  if (sp->flags & F_LEFT) {
    out_rep(o, ' ', pad);
  }
}

// Floating point

// digits of mant * 2^e as a decimal integer scaled by 10^-fd; digits are kept
// least significant first. Halving is multiplying by 5 and moving decimal point
// one place, so only small-integer multiplications are needed
#define FDIG_MAX 1200

static int mul_small(unsigned char *d, int n, unsigned m) {
  unsigned carry = 0;
  for (int i = 0; i < n; i++) {
    unsigned v = (unsigned)d[i] * m + carry;
    d[i] = (unsigned char)(v % 10);
    carry = v / 10;
  }
  while (carry) {
    d[n++] = (unsigned char)(carry % 10);
    carry /= 10;
  }

  return n;
}

static void fmt_nonfinite(out_t *o, const spec_t *sp, int neg, int is_nan) {
  const char *s = is_nan ? "nan" : "inf";
  char tmp[5];
  int n = 0;
  if (neg) {
    tmp[n++] = '-';
  } else if (sp->flags & F_PLUS) {
    tmp[n++] = '+';
  } else if (sp->flags & F_SPACE) {
    tmp[n++] = ' ';
  }
  for (int i = 0; i < 3; i++) {
    char c = s[i];
    if (sp->flags & F_UPPER) {
      c = (c >= 'a' && c <= 'z') ? "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[c - 'a'] : c;
    }
    tmp[n++] = c;
  }

  fmt_str(o, sp, tmp, n);
}

static void fmt_double(out_t *o, const spec_t *sp, double v) {
  union {
    double d;
    uint64_t u;
  } cv;
  cv.d = v;
  int neg = (int)(cv.u >> 63u);
  int bexp = (int)((cv.u >> 52u) & 0x7FFu);
  uint64_t frac = cv.u & 0xFFFFFFFFFFFFFULL;

  if (bexp == 0x7FF) {
    fmt_nonfinite(o, sp, neg, frac != 0);
    return;
  }

  uint64_t mant = frac;
  int e;
  if (bexp == 0) {
    e = -1074; // subnormal or zero
  } else {
    mant |= 1ULL << 52u;
    e = bexp - 1075;
  }

  unsigned char d[FDIG_MAX];
  int n = 0;
  if (mant == 0) {
    d[n++] = 0;
  } else {
    for (uint64_t m = mant; m; m /= 10) {
      d[n++] = (unsigned char)(m % 10);
    }
  }

  int fd = 0;
  if (mant != 0) {
    if (e >= 0) {
      for (int i = 0; i < e; i++) {
        n = mul_small(d, n, 2);
      }
    } else {
      fd = -e;
      for (int i = 0; i < fd; i++) {
        n = mul_small(d, n, 5);
      }
    }
  }

  while (n <= fd) {
    d[n++] = 0; // values below 1 get an explicit integer digit of 0
  }

  int prec = sp->prec < 0 ? 6 : sp->prec;
  if (prec > PREC_CAP) {
    prec = PREC_CAP;
  }

  // Round half to even at 'prec' fractional digits
  if (prec < fd) {
    int start = fd - prec; // index of last kept digit
    int first = d[start - 1];
    int rest = 0;
    for (int i = 0; i < start - 1; i++) {
      if (d[i]) {
        rest = 1;
        break;
      }
    }

    int up = first > 5 || (first == 5 && (rest || ((unsigned)d[start] & 1u)));
    for (int i = 0; i < start; i++) {
      d[i] = 0;
    }
    if (up) {
      int i = start;
      while (i < n && d[i] == 9) {
        d[i++] = 0;
      }
      if (i == n) {
        d[n++] = 1;
      } else {
        d[i]++;
      }
    }
  }

  // Integer part is d[fd .. n); everything below index fd is fractional
  if (n > FDIG_MAX) {
    n = FDIG_MAX; // cannot happen: at most ~770 digits for any double
  }
  if (fd < 0 || fd >= n) {
    fd = 0; // likewise unreachable: n > fd was established above
  }

  int int_digits = n - fd;
  int dot = prec > 0 || (sp->flags & F_ALT);
  char sign = 0;
  if (neg) {
    sign = '-';
  } else if (sp->flags & F_PLUS) {
    sign = '+';
  } else if (sp->flags & F_SPACE) {
    sign = ' ';
  }

  int total = (sign ? 1 : 0) + int_digits + (dot ? 1 : 0) + prec;
  int pad = sp->width > total ? sp->width - total : 0;
  int zeros = 0;
  if ((sp->flags & F_ZERO) && !(sp->flags & F_LEFT)) {
    zeros = pad;
    pad = 0;
  }
  if (!(sp->flags & F_LEFT)) {
    out_rep(o, ' ', pad);
  }
  if (sign) {
    out_ch(o, sign);
  }

  out_rep(o, '0', zeros);
  for (int i = n - 1; i >= fd; i--) {
    out_ch(o, "0123456789"[d[i]]);
  }
  if (dot) {
    out_ch(o, '.');
  }
  for (int j = 1; j <= prec; j++) {
    int idx = fd - j;
    out_ch(o, (idx >= 0 && idx < n) ? "0123456789"[d[idx]] : '0');
  }
  if (sp->flags & F_LEFT) {
    out_rep(o, ' ', pad);
  }
}

// Driver

enum { L_HH = -2, L_H = -1, L_NONE = 0, L_LONG = 1 };

static long long get_signed(va_list *ap, int len) {
  switch (len) {
  case L_HH:
    return (signed char)va_arg(*ap, int);
  case L_H:
    return (short)va_arg(*ap, int);
  case L_LONG:
    return va_arg(*ap, long long);
  default:
    return va_arg(*ap, int);
  }
}

static unsigned long long get_unsigned(va_list *ap, int len) {
  switch (len) {
  case L_HH:
    return (unsigned char)va_arg(*ap, unsigned int);
  case L_H:
    return (unsigned short)va_arg(*ap, unsigned int);
  case L_LONG:
    return va_arg(*ap, unsigned long long);
  default:
    return va_arg(*ap, unsigned int);
  }
}

int nl_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  if (!buf || size == 0) {
    return 0;
  }

  va_list ap2;
  va_copy(ap2, ap);

  out_t o = {buf, size, 0};

  while (*fmt) {
    if (*fmt != '%') {
      out_ch(&o, *fmt++);

      continue;
    }

    fmt++; // skip '%'

    spec_t sp = {0, 0, -1};
    for (;; fmt++) {
      if (*fmt == '-') {
        sp.flags |= F_LEFT;
      } else if (*fmt == '+') {
        sp.flags |= F_PLUS;
      } else if (*fmt == ' ') {
        sp.flags |= F_SPACE;
      } else if (*fmt == '0') {
        sp.flags |= F_ZERO;
      } else if (*fmt == '#') {
        sp.flags |= F_ALT;
      } else {
        break;
      }
    }

    if (*fmt == '*') {
      fmt++;
      sp.width = va_arg(ap2, int);
      if (sp.width < 0) {
        sp.flags |= F_LEFT;
        sp.width = -sp.width;
      }
    } else {
      while (*fmt >= '0' && *fmt <= '9') {
        if (sp.width < 100000) {
          sp.width = sp.width * 10 + (*fmt - '0');
        }
        fmt++;
      }
    }

    if (*fmt == '.') {
      fmt++;
      sp.prec = 0;
      if (*fmt == '*') {
        fmt++;
        sp.prec = va_arg(ap2, int); // negative means "absent"
      } else {
        while (*fmt >= '0' && *fmt <= '9') {
          if (sp.prec < 100000) {
            sp.prec = sp.prec * 10 + (*fmt - '0');
          }
          fmt++;
        }
      }
    }

    // Length modifiers: l, ll, z, j, t are all 64-bit (LP64)
    int len = L_NONE;
    if (*fmt == 'h') {
      len = L_H;
      fmt++;
      if (*fmt == 'h') {
        len = L_HH;
        fmt++;
      }
    } else {
      while (*fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') {
        len = L_LONG;
        fmt++;
      }
    }

    if (*fmt == '\0') {
      break;
    }

    switch (*fmt++) {
    case 'd':
    case 'i': {
      long long v = get_signed(&ap2, len);
      unsigned long long mag = (unsigned long long)v;
      if (v < 0) {
        mag = 0ULL - mag;
      }

      fmt_uint(&o, &sp, mag, v < 0, 1, 10);

      break;
    }
    case 'u': {
      fmt_uint(&o, &sp, get_unsigned(&ap2, len), 0, 0, 10);

      break;
    }
    case 'x': {
      fmt_uint(&o, &sp, get_unsigned(&ap2, len), 0, 0, 16);

      break;
    }
    case 'X': {
      sp.flags |= F_UPPER;
      fmt_uint(&o, &sp, get_unsigned(&ap2, len), 0, 0, 16);

      break;
    }
    case 'o': {
      fmt_uint(&o, &sp, get_unsigned(&ap2, len), 0, 0, 8);

      break;
    }
    case 'p': {
      void *p = va_arg(ap2, void *);
      if (!p) {
        fmt_str(&o, &sp, "(nil)", 5);
      } else {
        sp.flags |= F_ALT;
        fmt_uint(&o, &sp, (unsigned long long)(uintptr_t)p, 0, 0, 16);
      }

      break;
    }
    case 'f': {
      fmt_double(&o, &sp, va_arg(ap2, double));

      break;
    }
    case 'F': {
      sp.flags |= F_UPPER;
      fmt_double(&o, &sp, va_arg(ap2, double));

      break;
    }
    case 's': {
      const char *s = va_arg(ap2, const char *);
      if (!s) {
        s = "(null)";
      }

      int slen = 0;
      while (s[slen] && (sp.prec < 0 || slen < sp.prec)) {
        slen++;
      }

      fmt_str(&o, &sp, s, slen);

      break;
    }
    case 'c': {
      char c = (char)va_arg(ap2, int);
      fmt_str(&o, &sp, &c, 1);

      break;
    }
    case '%': {
      out_ch(&o, '%');

      break;
    }
    default: {
      out_ch(&o, '?');

      break;
    }
    }
  }

  va_end(ap2);
  buf[o.pos] = '\0';

  return (int)o.pos;
}

int nl_snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = nl_vsnprintf(buf, size, fmt, ap);
  va_end(ap);

  return r;
}

void nl_eprint(const char *msg) { nl_write(2, msg, nl_strlen(msg)); }
