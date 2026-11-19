#ifndef NL_MATH_H
#define NL_MATH_H

/*
 * nl_math.h - maths needed, without libm
 *
 * Everything is static inline, so nothing is linked and `nm -u` stays 0
 * Computed in double and rounded: about 1e-7 relative error once result is a
 * float
 *
 *   nl_fabs  nl_floor  nl_ceil  nl_fmod                 exact
 *   nl_sqrt  nl_exp    nl_log   nl_sin   nl_cos         series, see each
 *   nl_erf   nl_asinh  nl_tanh                          used by ASR net
 */

#include "nl_types.h"

#define NL_LN2 0.69314718055994531
#define NL_PI 3.14159265358979323846
// ln 2 and pi/2 in two parts each: high part has trailing zero bits, so k * HI
// is exact for k that matter and error of x - k*c stays small
#define NL_LN2_HI 6.93147180369123816490e-01
#define NL_LN2_LO 1.90821492927058770002e-10
#define NL_PIO2_HI 1.57079632673412561417e+00
#define NL_PIO2_LO 6.07710050650619224932e-11
#define NL_2_SQRTPI 1.12837916709551257
#define NL_SQRTPI 1.77245385090551603

// Bits of a double, for functions that need exponent
typedef union {
  double d;
  uint64_t u;
} nl_dbits_t;

static inline double nl_fabs(double x) {
  nl_dbits_t v = {x};
  v.u &= ~(1ull << 63u);

  return v.d;
}

// 1 for NaN and for infinities: neither has a fractional part to work on
static inline int nl_nonfinite_(double x) {
  nl_dbits_t v = {x};

  return ((v.u >> 52u) & 0x7ffu) == 0x7ffu;
}

// Beyond 2^52 every double is a whole number already
static inline int nl_whole_(double x) {
  return nl_fabs(x) >= 4503599627370496.0;
}

// Largest whole number <= x. NaN and infinities come back as they are
static inline double nl_floor(double x) {
  if (nl_nonfinite_(x) || nl_whole_(x) || x == 0) {
    return x; // x == 0 also keeps sign of -0.0
  }

  double t = (double)(long long)x; // truncates towards zero
  if (t > x) {
    t -= 1;
  }

  return t;
}

// Smallest whole number >= x
static inline double nl_ceil(double x) {
  if (nl_nonfinite_(x) || nl_whole_(x) || x == 0) {
    return x;
  }

  double t = (double)(long long)x;
  if (t < x) {
    t += 1;
  }

  if (t == 0 && x < 0) {
    nl_dbits_t z = {0}; // -0.5 -> -0.0
    z.u = 1ull << 63u;
    return z.d;
  }

  return t;
}

/*
 * x - n*y for whole number n that makes result same sign as x and smaller than
 * |y| in size
 * Exact: no rounding error at all, because it is done on mantissas as integers,
 * a bit at a time (at most 2046 steps for extreme pair 1e308, 1e-308; 1.0 steps
 * in usual phase wrapping)
 *
 *   y == 0, x \infty, either NaN  ->  NaN
 *   y \infty                      ->  x
 *   x == \pm 0                    ->  x
 * Result has sign of x
 */
static inline double nl_fmod(double x, double y) {
  nl_dbits_t ux = {x}, uy = {y}, r;
  uint64_t sign = ux.u & (1ull << 63u);
  uint64_t ax = ux.u & ~(1ull << 63u), ay = uy.u & ~(1ull << 63u);

  if (ay == 0 || ax >= 0x7ff0000000000000ull || ay > 0x7ff0000000000000ull) {
    r.u = 0x7ff8000000000000ull; // NaN
    return r.d;
  }

  if (ax < ay) {
    return x; // |x| < |y|, includes +-0 and an infinite y
  }

  if (ax == ay) {
    r.u = sign; // exact multiple: zero with sign of x
    return r.d;
  }

  // Value = m * 2^(e - 1075) with top bit of m (bit 52) set
  // A subnormal is shifted up until it is, which takes its exponent below 1
  int ex = (int)(ax >> 52u), ey = (int)(ay >> 52u);
  uint64_t mx = ax & 0xfffffffffffffull, my = ay & 0xfffffffffffffull;
  if (ex == 0) {
    ex = 1;
    while (!(mx & (1ull << 52u))) {
      mx <<= 1u;
      ex--;
    }
  } else {
    mx |= 1ull << 52u;
  }

  if (ey == 0) {
    ey = 1;
    while (!(my & (1ull << 52u))) {
      my <<= 1u;
      ey--;
    }
  } else {
    my |= 1ull << 52u;
  }

  // long division of mx*2^ex by my*2^ey, a bit at a time: remainder stays below
  // my, so shift never overflows (my < 2^53)
  for (; ex > ey; ex--) {
    if (mx >= my) {
      mx -= my;
    }

    mx <<= 1u;
  }

  if (mx >= my) {
    mx -= my;
  }

  if (mx == 0) {
    r.u = sign;
    return r.d;
  }

  // renormalise, then a normal number or, below smallest, a subnormal (exact:
  // remainder is a multiple of last bit of y)
  while (!(mx & (1ull << 52u))) {
    mx <<= 1u;
    ex--;
  }

  if (ex >= 1) {
    r.u = sign | ((uint64_t)ex << 52u) | (mx & 0xfffffffffffffull);
  } else {
    r.u = sign | (mx >> (unsigned)(1 - ex));
  }

  return r.d;
}

// x <= 0 (and NaN) give 0; Newton from a bit-trick start, 6 steps
static inline double nl_sqrt(double x) {
  if (!(x > 0)) {
    return 0;
  }

  nl_dbits_t v = {x};
  v.u = (v.u >> 1u) + 0x1ff8000000000000ull;
  double y = v.d;
  for (int i = 0; i < 6; i++) {
    y = 0.5 * (y + x / y);
  }

  return y;
}

// Above 709 it is clamped (no infinity), below -745 it is 0
static inline double nl_exp(double x) {
  if (x > 709.0) {
    x = 709.0;
  }

  if (x < -745.0) {
    return 0;
  }

  double kf = x / NL_LN2;
  long k = (long)(kf + (kf < 0 ? -0.5 : 0.5));
  double r = x - (double)k * NL_LN2_HI - (double)k * NL_LN2_LO; // |r| <= 0.35
  double t = 1, s = 1;
  for (int i = 1; i < 17; i++) {
    t *= r / (double)i;
    s += t;
  }

  nl_dbits_t p;
  // 2^k, split in two so that k down to -1074 does not underflow exponent
  long k1 = k / 2, k2 = k - k1;
  p.u = (uint64_t)(k1 + 1023) << 52u;
  double a = p.d;
  p.u = (uint64_t)(k2 + 1023) << 52u;

  return s * a * p.d;
}

// x <= 0 gives -745 (like exp underflow), not NaN
static inline double nl_log(double x) {
  if (!(x > 0)) {
    return -745.0;
  }

  nl_dbits_t v = {x};
  long e = (long)((v.u >> 52u) & 0x7ffu) - 1023;
  if (e == -1023) { // subnormal: scale up
    v.d = x * 18014398509481984.0;
    e = (long)((v.u >> 52u) & 0x7ffu) - 1023 - 54;
  }

  v.u = (v.u & 0xfffffffffffffull) | 0x3ff0000000000000ull;
  double m = v.d;
  if (m > 1.41421356237309505) {
    m *= 0.5;
    e++;
  }

  double s = (m - 1) / (m + 1);
  double s2 = s * s;
  double r = 0, term = s;
  for (int i = 0; i < 14; i++) {
    r += term / (double)(2 * i + 1);
    term *= s2;
  }

  return 2 * r + (double)e * NL_LN2;
}

// Power series, good anywhere |x| is small (after quadrant reduction)
static inline double nl_sin_core_(double x) {
  double x2 = x * x, t = x, r = x;
  for (int i = 1; i < 14; i++) {
    t *= -x2 / ((2.0 * i) * (2.0 * i + 1));
    r += t;
  }

  return r;
}

static inline double nl_cos_core_(double x) {
  double x2 = x * x, t = 1, r = 1;
  for (int i = 1; i < 14; i++) {
    t *= -x2 / ((2.0 * i - 1) * (2.0 * i));
    r += t;
  }

  return r;
}

// Quadrant reduction: x = q * pi/2 + r, |r| <= pi/4. Fine for |x| up to about
// 1e6 or so (q * pi/2 is exact only for a q that small): audio phases and
// angles, not 1e300. Reduce with nl_fmod first if it can be larger
static inline double nl_reduce_(double x, long *q) {
  double k = x / (NL_PI / 2);
  long n = (long)(k + (k < 0 ? -0.5 : 0.5));
  *q = n;

  return (x - (double)n * NL_PIO2_HI) - (double)n * NL_PIO2_LO;
}

static inline double nl_sin(double x) {
  long q;
  double r = nl_reduce_(x, &q);
  switch ((unsigned long)q & 3u) {
  case 0:
    return nl_sin_core_(r);
  case 1:
    return nl_cos_core_(r);
  case 2:
    return -nl_sin_core_(r);
  default:
    return -nl_cos_core_(r);
  }
}

static inline double nl_cos(double x) {
  long q;
  double r = nl_reduce_(x, &q);
  switch ((unsigned long)q & 3u) {
  case 0:
    return nl_cos_core_(r);
  case 1:
    return -nl_sin_core_(r);
  case 2:
    return -nl_cos_core_(r);
  default:
    return nl_sin_core_(r);
  }
}

static inline double nl_erf(double x) {
  double ax = x < 0 ? -x : x;
  double r;
  if (ax < 2.5) {
    // Maclaurin series, no cancellation trouble this close to 0
    double x2 = ax * ax, t = ax, s = ax;
    for (int n = 1; n < 80; n++) {
      t *= -x2 / (double)n;
      double term = t / (double)(2 * n + 1);
      s += term;
      if (term < 1e-17 && term > -1e-17) {
        break;
      }
    }

    r = NL_2_SQRTPI * s;
  } else if (ax > 6.0) {
    r = 1.0;
  } else {
    // erfc by continued fraction, evaluated from tail
    double f = ax;
    for (int k = 60; k >= 1; k--) {
      f = ax + 0.5 * (double)k / f;
    }

    r = 1.0 - nl_exp(-ax * ax) / (f * NL_SQRTPI);
  }

  return x < 0 ? -r : r;
}

static inline double nl_asinh(double x) {
  double ax = x < 0 ? -x : x;
  double r;
  if (ax < 1e-4) {
    r = ax - ax * ax * ax / 6.0;
  } else if (ax > 1e8) {
    r = nl_log(ax) + NL_LN2;
  } else {
    r = nl_log(ax + nl_sqrt(ax * ax + 1.0));
  }

  return x < 0 ? -r : r;
}

static inline double nl_tanh(double x) {
  if (x > 20) {
    return 1;
  }

  if (x < -20) {
    return -1;
  }

  double e = nl_exp(2 * x);

  return (e - 1) / (e + 1);
}

#endif
