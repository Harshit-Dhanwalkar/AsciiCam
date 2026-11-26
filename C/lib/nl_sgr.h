/*
 * lib/nl_sgr.h - ANSI colour escape writers that skip printf
 *
 *   size_t nl_sgr_fg256(char *dst, int idx);
 *   size_t nl_sgr_fgbg256(char *dst, int fg, int bg);
 *   size_t nl_sgr_fg_rgb(char *dst, int r, int g, int b);
 *   size_t nl_sgr_fgbg_rgb(char *dst, const uint8_t *fg, const uint8_t *bg);
 *
 * ("\033[38;5;%dm", "\033[38;5;%dm\033[48;5;%dm", "\033[38;2;%d;%d;%dm" and
 * its fg+bg pair) and returns byte count
 * No NUL is written
 * Values are clamped to 0..255, so a caller can never produce a malformed
 * sequence
 *
 * NOTE: Why: colour mode emits one escape per cell, so a 200 x 60 frame at 30
 * fps is 360 000 printf calls per second, each re-parsing its format string
 * Here a number is a few multiply-shifts (division by constants) and byte
 * stores
 *
 * Caller guarantees room: NL_SGR_MAX bytes is enough for any of writers
 */
#ifndef NL_SGR_H
#define NL_SGR_H

#include "nl_types.h"

#define NL_SGR_MAX 40 // "\033[38;2;255;255;255m\033[48;2;255;255;255m" = 38

// Write decimal text of v (clamped to 0..255) without leading zeros; returns
// digit count (1..3)
static inline size_t nl_sgr_put(char *p, int v) {
  if (v < 0) {
    v = 0;
  }
  if (v > 255) {
    v = 255;
  }

  if (v >= 100) {
    int h = v / 100;
    int t = (v - h * 100) / 10;
    p[0] = (char)('0' + h);
    p[1] = (char)('0' + t);
    p[2] = (char)('0' + (v - h * 100 - t * 10));

    return 3;
  }
  if (v >= 10) {
    int t = v / 10;
    p[0] = (char)('0' + t);
    p[1] = (char)('0' + (v - t * 10));

    return 2;
  }
  p[0] = (char)('0' + v);

  return 1;
}

static inline size_t nl_sgr_lit(char *p, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    p[i] = s[i];
  }

  return n;
}

static inline size_t nl_sgr_fg256(char *dst, int idx) {
  size_t n = nl_sgr_lit(dst, "\033[38;5;", 7);
  n += nl_sgr_put(dst + n, idx);
  dst[n++] = 'm';

  return n;
}

static inline size_t nl_sgr_fgbg256(char *dst, int fg, int bg) {
  size_t n = nl_sgr_fg256(dst, fg);
  n += nl_sgr_lit(dst + n, "\033[48;5;", 7);
  n += nl_sgr_put(dst + n, bg);
  dst[n++] = 'm';

  return n;
}

static inline size_t nl_sgr_rgb_body(char *dst, int r, int g, int b) {
  size_t n = nl_sgr_put(dst, r);
  dst[n++] = ';';
  n += nl_sgr_put(dst + n, g);
  dst[n++] = ';';
  n += nl_sgr_put(dst + n, b);
  dst[n++] = 'm';

  return n;
}

static inline size_t nl_sgr_fg_rgb(char *dst, int r, int g, int b) {
  size_t n = nl_sgr_lit(dst, "\033[38;2;", 7);

  return n + nl_sgr_rgb_body(dst + n, r, g, b);
}

static inline size_t nl_sgr_fgbg_rgb(char *dst, const uint8_t *fg,
                                     const uint8_t *bg) {
  size_t n = nl_sgr_fg_rgb(dst, fg[0], fg[1], fg[2]);
  n += nl_sgr_lit(dst + n, "\033[48;2;", 7);

  return n + nl_sgr_rgb_body(dst + n, bg[0], bg[1], bg[2]);
}

#endif
