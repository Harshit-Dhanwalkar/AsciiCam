#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* matrix_rain - falling streaks over a darkened picture
 *  Streaks are as wide as one text cell of an 80 column terminal so they
 * survive ASCII conversion
 * Parameter: 0 off .. 255 heaviest rain (128 default) */

#define RAIN_COLS 4096

static int rain_head[RAIN_COLS]; // y of bright head (may be above frame)
static int rain_speed[RAIN_COLS];
static int rain_w, rain_h;
static uint32_t rain_seed = 0xBADC0DE5u;

static void rain_spawn(int c, int h, int tail) {
  rain_head[c] = -fx_rand_n(&rain_seed, h + tail);
  rain_speed[c] = 1 + h / 80 + fx_rand_n(&rain_seed, h / 30 + 1);
}

void matrix_rain_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  int cw = w / 80 > 2 ? w / 80 : 2;
  int ncols = (w + cw - 1) / cw;
  if (ncols > RAIN_COLS) {
    return;
  }

  int tail = h / 3 + 1;
  if (rain_w != w || rain_h != h) {
    rain_w = w;
    rain_h = h;
    for (int c = 0; c < ncols; c++) {
      rain_spawn(c, h, tail);
    }
  }

  // darken picture so rain reads against it
  int n = w * h;
  for (int i = 0; i < n; i++) {
    gray[i] = (uint8_t)fx_mix(gray[i], gray[i] / 4, p);
  }

  for (int c = 0; c < ncols; c++) {
    rain_head[c] += rain_speed[c];
    if (rain_head[c] - tail > h) {
      rain_spawn(c, h, tail);
    }

    // sparser rain at low parameter: some columns stay empty
    if (c * 37 % 256 > p) {
      continue;
    }

    int x0 = c * cw;
    int x1 = x0 + cw < w ? x0 + cw : w;
    int y0 = rain_head[c] - tail < 0 ? 0 : rain_head[c] - tail;
    int y1 = rain_head[c] < h ? rain_head[c] + 1 : h;
    for (int y = y0; y < y1; y++) {
      int age = rain_head[c] - y; // 0 at head
      int v = 255 - age * 255 / tail;
      for (int x = x0; x < x1; x++) {
        if (v > gray[y * w + x]) {
          gray[y * w + x] = (uint8_t)v;
        }
      }
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = matrix_rain_process,
                               .name = "matrix_rain"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
