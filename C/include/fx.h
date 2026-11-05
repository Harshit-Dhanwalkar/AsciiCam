#ifndef FX_H
#define FX_H

/*
 * Helpers shared by the effect plugins in filters/ (crt, vhs, tv_static, ...).
 *
 * Plugins get the camera's luma plane (one byte per pixel, w*h bytes) and one
 * int parameter 0..255 (the `[128]` shown in the panel). The conventions the
 * effect plugins follow:
 *   - parameter 0 means "off" (the frame is left alone) wherever that makes
 *     sense; 128, the default every plugin starts at, is a pleasant setting
 *   - nothing here uses floating point, libm or malloc: tables and scratch
 *     space are static, so there is nothing to leak on hot reload
 *   - a frame larger than FX_MAX_PIXELS (or with a non-positive side) is
 *     passed through untouched rather than risking an overrun
 *
 * Only luma is available, so "colour" effects (thermal, sepia, chromatic
 * aberration) are tone-curve approximations of the real thing.
 */

#include "nl_types.h"

#define FX_MAX_PIXELS 2073600 // 1920 x 1080
#define FX_MAX_SIDE 8192

// 1 when the frame can be processed
static inline int fx_ok(const uint8_t *gray, int w, int h) {
  return gray != NULL && w > 0 && h > 0 && w <= FX_MAX_SIDE &&
         h <= FX_MAX_SIDE && (uint64_t)w * (uint64_t)h <= FX_MAX_PIXELS;
}

// the plugin parameter, clamped to 0..255; def when the host gave none
static inline int fx_param(const void *ctx, int def) {
  int v = ctx ? *(const int *)ctx : def;

  return v < 0 ? 0 : v > 255 ? 255 : v;
}

static inline int fx_clamp(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

static inline uint8_t fx_u8(int v) { return (uint8_t)fx_clamp(v, 0, 255); }

static inline int fx_abs(int v) { return v < 0 ? -v : v; }

// a + (b - a) * t / 255, t in 0..255
static inline int fx_mix(int a, int b, int t) { return a + (b - a) * t / 255; }

// xorshift32; never leaves 0 once seeded non-zero
static inline uint32_t fx_rand(uint32_t *state) {
  uint32_t x = *state ? *state : 0x9E3779B9u;
  x ^= x << 13u;
  x ^= x >> 17u;
  x ^= x << 5u;
  *state = x;

  return x;
}

// uniform-ish value 0..n-1 (n > 0)
static inline int fx_rand_n(uint32_t *state, int n) {
  return (int)(fx_rand(state) % (uint32_t)n);
}

/*
 * Sine without libm: phase 0..1023 is one full turn, result -256..256.
 * A parabola refined with the usual 0.775/0.225 blend; within about 1% of the
 * real thing, which is far more than a visual effect needs.
 */
static inline int fx_sin(int phase) {
  int p = phase % 1024;
  if (p < 0) {
    p += 1024;
  }

  int neg = p >= 512;
  int half = neg ? p - 512 : p;
  int y = half * (512 - half) / 256; // parabola 0..256
  int s = (198 * y + 58 * y * y / 256) / 256;

  return neg ? -s : s;
}

// Integer square root (floor) of a non-negative 32 bit value
static inline int fx_isqrt(int v) {
  if (v <= 0) {
    return 0;
  }

  int r = 0;
  int bit = 1073741824; // 4^15
  while (bit > v) {
    bit /= 4;
  }
  while (bit != 0) {
    if (v >= r + bit) {
      v -= r + bit;
      r = r / 2 + bit;
    } else {
      r /= 2;
    }

    bit /= 4;
  }

  return r;
}

#endif /* FX_H */
