/*
 * tests/test_nl_sgr.c - lib/nl_sgr.h against printf formats it replaces
 *
 * Every single value 0..255 is compared in each field position, then random
 * colour pairs, then out-of-range inputs (clamped, never malformed)
 * Each writer sits between guard bytes to prove it writes exactly reported
 * length and no NUL
 */

#include "../lib/nl_sgr.h"
#include "../lib/nolibc.h"

static int pass_n, fail_n;

#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      if (fail_n < 30) {                                                       \
        fprintf(stderr, "FAIL line %d: ", __LINE__);                           \
        fprintf(stderr, __VA_ARGS__);                                          \
        fprintf(stderr, "\n");                                                 \
      }                                                                        \
    }                                                                          \
  } while (0)

static unsigned long long rng = 0x9E3779B97F4A7C15ULL;
static unsigned rnd8(void) {
  rng ^= rng >> 12;
  rng ^= rng << 25;
  rng ^= rng >> 27;

  return (unsigned)((rng * 0x2545F4914F6CDD1DULL) >> 56);
}

#define GUARD 0xA5

static int same(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i]) {
      return 0;
    }
  }

  return 1;
}

// Run writer into a guarded buffer; compare with want
// Returns nothing: it records verdict itself
static void expect(const char *what, size_t got, const char *buf,
                   const char *want) {
  size_t wl = nl_strlen(want);
  CHECK(got == wl, "%s: length %zu want %zu", what, got, wl);
  CHECK(same(buf + 8, want, wl), "%s: bytes differ (want %s)", what, want);
  CHECK((unsigned char)buf[8 + wl] == GUARD, "%s: wrote past end", what);
  CHECK((unsigned char)buf[7] == GUARD, "%s: wrote before start", what);
}

static void fresh(char *buf) { nl_memset(buf, GUARD, 8 + NL_SGR_MAX + 8); }

static void check_fg256(int v) {
  char buf[8 + NL_SGR_MAX + 8], want[64];
  fresh(buf);
  size_t n = nl_sgr_fg256(buf + 8, v);
  int c = v < 0 ? 0 : v > 255 ? 255 : v;
  nl_snprintf(want, sizeof(want), "\033[38;5;%dm", c);
  expect("fg256", n, buf, want);
}

static void check_fgbg256(int a, int b) {
  char buf[8 + NL_SGR_MAX + 8], want[64];
  fresh(buf);
  size_t n = nl_sgr_fgbg256(buf + 8, a, b);
  nl_snprintf(want, sizeof(want), "\033[38;5;%dm\033[48;5;%dm", a, b);
  expect("fgbg256", n, buf, want);
}

static void check_rgb(int r, int g, int b) {
  char buf[8 + NL_SGR_MAX + 8], want[64];
  fresh(buf);
  size_t n = nl_sgr_fg_rgb(buf + 8, r, g, b);
  nl_snprintf(want, sizeof(want), "\033[38;2;%d;%d;%dm", r, g, b);
  expect("fg_rgb", n, buf, want);
}

static void check_pair(const uint8_t *f, const uint8_t *b) {
  char buf[8 + NL_SGR_MAX + 8], want[96];
  fresh(buf);
  size_t n = nl_sgr_fgbg_rgb(buf + 8, f, b);
  nl_snprintf(want, sizeof(want), "\033[38;2;%d;%d;%dm\033[48;2;%d;%d;%dm",
              f[0], f[1], f[2], b[0], b[1], b[2]);
  expect("fgbg_rgb", n, buf, want);
  CHECK(n <= NL_SGR_MAX, "NL_SGR_MAX too small: %zu", n);
}

int main(void) {
  for (int v = 0; v < 256; v++) {
    check_fg256(v);
    check_rgb(v, 0, 0);
    check_rgb(0, v, 0);
    check_rgb(0, 0, v);
    check_fgbg256(v, 255 - v);
    check_fgbg256(v, v);
  }

  // Widest and narrowest possible sequences
  uint8_t lo[3] = {0, 0, 0}, hi[3] = {255, 255, 255};
  check_pair(hi, hi);
  check_pair(lo, lo);
  check_pair(lo, hi);

  for (int i = 0; i < 20000; i++) {
    uint8_t f[3] = {(uint8_t)rnd8(), (uint8_t)rnd8(), (uint8_t)rnd8()};
    uint8_t b[3] = {(uint8_t)rnd8(), (uint8_t)rnd8(), (uint8_t)rnd8()};
    check_pair(f, b);
    check_rgb(f[0], f[1], f[2]);
  }

  // Out of range input clamps rather than emitting a bad sequence
  check_fg256(-1);
  check_fg256(-100000);
  check_fg256(256);
  check_fg256(100000);
  {
    char buf[8 + NL_SGR_MAX + 8];
    fresh(buf);
    size_t n = nl_sgr_fg_rgb(buf + 8, -5, 300, 7);
    expect("clamp rgb", n, buf, "\033[38;2;0;255;7m");
  }

  fprintf(stderr, "nl_sgr: %d passed, %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
