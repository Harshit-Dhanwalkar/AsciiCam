/*
 * tests/test_math.c - lib/nl_math.h against values from Python's math module
 *
 * No libm here either: reference values are written out as hex floats
 * fmod, floor, ceil and fabs must match them bit for bit (signed zeros and
 * subnormals included); series functions to tolerance in each line
 */

#include "../lib/nolibc.h"
#include "nl_math.h"

static int pass_n, fail_n;

#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static int same(double a, double b) {
  nl_dbits_t x = {a}, y = {b};
  if (a != a && b != b) {
    return 1; // both NaN
  }

  return x.u == y.u;
}

static int near(double a, double b, double tol) {
  return nl_fabs(a - b) <= tol * (1.0 + nl_fabs(b));
}

#define INF (1.0 / 0.0)
#define NAN_ (0.0 / 0.0)

static void test_fmod(void) {
  static const struct {
    double x, y, want;
  } tab[] = {
    {0x1.6000000000000p+2, 0x1.0000000000000p+1, 0x1.8000000000000p+0},
    {-0x1.6000000000000p+2, 0x1.0000000000000p+1, -0x1.8000000000000p+0},
    {0x1.6000000000000p+2, -0x1.0000000000000p+1, 0x1.8000000000000p+0},
    {-0x1.6000000000000p+2, -0x1.0000000000000p+1, -0x1.8000000000000p+0},
    {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0},
    {-0x1.0000000000000p+0, 0x1.0000000000000p+0, -0x0.0p+0},
    {0x0.0p+0, 0x1.8000000000000p+1, 0x0.0p+0},
    {-0x0.0p+0, 0x1.8000000000000p+1, -0x0.0p+0},
    {0x1.3333333333333p-2, 0x1.0000000000000p+0, 0x1.3333333333333p-2},
    {0x1.2d687e4189375p+20, 0x1.0000000000000p+0, 0x1.c83126ea00000p-1},
    {0x1.c000000000000p+2, 0x1.999999999999ap-4, 0x1.999999999997ep-4},
    {0x1.2a05f20000000p+33, 0x1.3333333333333p-2, 0x1.9999fcf0ea444p-4},
    {0x1.7e43c8800759cp+996, 0x1.c000000000000p+2, 0x1.0000000000000p+0},
    {0x1.fffffffffffffp+1023, 0x1.56e1fc2f8f359p-997, 0x1.1c210546b28c0p-1002},
    {0x1.0000000000000p+0, 0x0.0000000000001p-1022, 0x0.0p+0},
    {0x0.012688b70e62bp-1022, 0x0.00585c36eab73p-1022, 0x0.001d74124e3d2p-1022},
    {0x0.00585c36eab73p-1022, 0x0.012688b70e62bp-1022, 0x0.00585c36eab73p-1022},
    {0x1.4000000000000p+1, 0x0.012688b70e62bp-1022, 0x0.010243b7386a0p-1022},
    {-0x1.4000000000000p+1, 0x0.012688b70e62bp-1022, -0x0.010243b7386a0p-1022},
    {0x0.0000000000001p-1022, 0x0.0000000000001p-1022, 0x0.0p+0},
    {0x1.1ccf385ebc8a0p+1023, 0x0.730d67819e8d2p-1022, 0x0.28401cf53d610p-1022},
    {0x1.999999999999ap-4, 0x1.47ae147ae147bp-7, 0x1.0000000000000p-58},
    {0x1.9000000000000p+6, 0x1.6666666666666p-1, 0x1.333333333336cp-1},
    {-0x1.9000000000000p+6, 0x1.6666666666666p-1, -0x1.333333333336cp-1},
    {0x1.8000000000000p+1, 0x1.7e43c8800759cp+996, 0x1.8000000000000p+1},
    {0x1.4f8b588e368f1p-17, 0x1.0624dd2f1a9fcp-10, 0x1.4f8b588e368f1p-17},
    {0x1.921fb54442d18p+2, 0x1.921fb54442d18p+0, 0x0.0p+0},
    {0x0.0000000000001p-1022, 0x1.0000000000000p+1, 0x0.0000000000001p-1022},
    {0x1.0000000000000p+3, 0x1.0000000000000p-1, 0x0.0p+0},
    {0x1.0000000000000p+3, 0x1.8000000000000p+1, 0x1.0000000000000p+1},
    {0x1.0000000000000p+52, 0x1.8000000000000p+1, 0x1.0000000000000p+0},
    {0x1.0000000000000p+53, 0x1.8000000000000p+1, 0x1.0000000000000p+1},
    {0x1.d6f34547df3b6p+26, 0x1.0000000000000p-2, 0x1.f7ced80000000p-4},
  };
  for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
    double got = nl_fmod(tab[i].x, tab[i].y);
    CHECK(same(got, tab[i].want), "fmod case %u: got %.17g want %.17g", i, got,
          tab[i].want);
  }

  // Edges: NaN for y == 0, x infinite, a NaN in; x for y infinite
  CHECK(nl_fmod(1, 0) != nl_fmod(1, 0), "fmod(1, 0) is NaN");
  CHECK(nl_fmod(INF, 2) != nl_fmod(INF, 2), "fmod(inf, 2) is NaN");
  CHECK(nl_fmod(NAN_, 2) != nl_fmod(NAN_, 2), "fmod(nan, 2) is NaN");
  CHECK(nl_fmod(2, NAN_) != nl_fmod(2, NAN_), "fmod(2, nan) is NaN");
  CHECK(same(nl_fmod(3.5, INF), 3.5) && same(nl_fmod(-3.5, -INF), -3.5),
        "fmod(x, inf) is x");

  // what it is for: a phase accumulator stays in [0, 1) however long it runs
  double ph = 0;
  int inside = 1;
  for (long i = 0; i < 1000000; i++) {
    ph = nl_fmod(ph + 0.1, 1.0);
    if (ph < 0 || ph >= 1) {
      inside = 0;
    }
  }

  CHECK(inside, "phase stays in [0, 1)");
  CHECK(ph < 1e-6 || ph > 1 - 1e-6,
        "a million steps of 0.1 is a whole number of turns: %.12g", ph);

  // Same in float, where plain sum would have lost its low bits
  float pf = 0;
  for (long i = 0; i < 1000000; i++) {
    pf = (float)nl_fmod((double)pf + 0.01, 1.0);
  }

  CHECK(pf >= 0 && pf < 1, "float phase stays in [0, 1)");

  // Property: r has sign of x, |r| < |y|, and (x - r) / y is whole
  unsigned long long s = 88172645463325252ull;
  int bad = 0;
  for (int i = 0; i < 20000; i++) {
    s ^= s << 13u;
    s ^= s >> 7u;
    s ^= s << 17u;
    double x = ((double)(long long)(s % 2000001) - 1000000) / 64.0;
    s ^= s << 13u;
    s ^= s >> 7u;
    s ^= s << 17u;
    double y = (double)(long long)(s % 100000 + 1) / 128.0;
    double r = nl_fmod(x, y);
    double q = (x - r) / y;
    if (nl_fabs(r) >= y || (r != 0 && (r < 0) != (x < 0)) ||
        nl_fabs(q - nl_floor(q + 0.5)) > 1e-6) {
      bad++;
    }
  }

  CHECK(bad == 0, "fmod property failed %d times", bad);
}

static void test_rounding(void) {
  static const struct {
    double x, fl, ce;
  } tab[] = {
    {0x1.0000000000000p-1, 0x0.0p+0, 0x1.0000000000000p+0},
    {-0x1.0000000000000p-1, -0x1.0000000000000p+0, -0x0.0p+0},
    {0x1.8000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+1},
    {-0x1.8000000000000p+0, -0x1.0000000000000p+1, -0x1.0000000000000p+0},
    {0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1},
    {-0x1.0000000000000p+1, -0x1.0000000000000p+1, -0x1.0000000000000p+1},
    {0x1.56e1fc2f8f359p-997, 0x0.0p+0, 0x1.0000000000000p+0},
    {-0x1.56e1fc2f8f359p-997, -0x1.0000000000000p+0, -0x0.0p+0},
    {0x1.fffffffffffffp-1, 0x0.0p+0, 0x1.0000000000000p+0},
    {-0x1.fffffffffffffp-1, -0x1.0000000000000p+0, -0x0.0p+0},
    {0x1.fffffffffffffp+51, 0x1.ffffffffffffep+51, 0x1.0000000000000p+52},
    {-0x1.fffffffffffffp+51, -0x1.0000000000000p+52, -0x1.ffffffffffffep+51},
    {0x1.0000000000000p+52, 0x1.0000000000000p+52, 0x1.0000000000000p+52},
    {0x1.5af1d78b58c40p+66, 0x1.5af1d78b58c40p+66, 0x1.5af1d78b58c40p+66},
    {-0x1.5af1d78b58c40p+66, -0x1.5af1d78b58c40p+66, -0x1.5af1d78b58c40p+66},
    {0x1.effef9db22d0ep+6, 0x1.ec00000000000p+6, 0x1.f000000000000p+6},
    {-0x1.effef9db22d0ep+6, -0x1.f000000000000p+6, -0x1.ec00000000000p+6},
    {-0x0.0p+0, -0x0.0p+0, -0x0.0p+0},
  };
  for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
    CHECK(same(nl_floor(tab[i].x), tab[i].fl), "floor case %u: got %.17g", i,
          nl_floor(tab[i].x));
    CHECK(same(nl_ceil(tab[i].x), tab[i].ce), "ceil case %u: got %.17g", i,
          nl_ceil(tab[i].x));
  }

  CHECK(nl_floor(INF) == INF && nl_ceil(-INF) == -INF, "floor, ceil of infinity");
  CHECK(nl_floor(NAN_) != nl_floor(NAN_), "floor of NaN is NaN");
  CHECK(same(nl_fabs(-0.0), 0.0) && nl_fabs(-3.5) == 3.5 && nl_fabs(INF) == INF,
        "fabs");
}

static void test_series(void) {
  CHECK(near(nl_exp(1.0), 2.718281828459045, 1e-14), "exp 1");
  CHECK(near(nl_exp(-30.5), 5.675e-14, 2e-3), "exp -30.5");
  CHECK(near(nl_exp(-0.5), 0.6065306597126334, 1e-14), "exp -.5");
  CHECK(near(nl_exp(10.0), 22026.465794806718, 1e-14), "exp 10");
  CHECK(nl_exp(-800) == 0 && nl_exp(800) > 1e300, "exp range");
  CHECK(near(nl_log(2.0), 0.6931471805599453, 1e-14), "log 2");
  CHECK(near(nl_log(1e-300), -690.7755278982137, 1e-14), "log tiny");
  CHECK(near(nl_log(123456.789), 11.723638, 1e-6), "log big");
  CHECK(nl_log(0) < -700 && nl_log(-1) < -700, "log nonpositive");
  CHECK(near(nl_sin(1.0), 0.8414709848078965, 1e-14), "sin 1");
  CHECK(near(nl_cos(1.0), 0.5403023058681398, 1e-14), "cos 1");
  CHECK(near(nl_sin(100.0), -0.5063656411097588, 1e-14), "sin 100");
  CHECK(near(nl_cos(-100.0), 0.8623188722876839, 1e-14), "cos -100");
  CHECK(near(nl_sin(NL_PI), 0.0, 1e-15) && near(nl_cos(NL_PI / 2), 0.0, 1e-15),
        "sin pi, cos pi/2");
  CHECK(near(nl_erf(0.5), 0.5204998778130465, 1e-14), "erf .5");
  CHECK(near(nl_erf(-2.0), -0.9953222650189527, 1e-14), "erf -2");
  CHECK(near(nl_erf(3.0), 0.9999779095030014, 1e-14), "erf 3");
  CHECK(nl_erf(7.0) == 1.0 && nl_erf(0) == 0.0, "erf ends");
  CHECK(near(nl_asinh(0.75), 0.6931471805599453, 1e-13), "asinh .75");
  CHECK(near(nl_asinh(-1000.0), -7.600902709541988, 1e-13), "asinh -1000");
  CHECK(near(nl_asinh(1e-9), 1e-9, 1e-13), "asinh small");
  CHECK(near(nl_tanh(0.5), 0.46211715726000974, 1e-14), "tanh");
  CHECK(near(nl_sqrt(2.0), 1.4142135623730951, 1e-15), "sqrt 2");
  CHECK(nl_sqrt(0) == 0 && nl_sqrt(-4) == 0, "sqrt nonpositive");
}

int main(void) {
  test_fmod();
  test_rounding();
  test_series();
  fprintf(stderr, "math: %d passed, %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}

