#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* swirl - twirl: picture is wound around its centre, twist fading to nothing at
 * edge of circle that fits inside frame (corners outside it stay as they are)
 * Parameter: 0 off .. 255 winds centre by one and a half turns (128 default is
 * about three quarters of a turn) */

static uint8_t swirl_src[FX_MAX_PIXELS]; // Frame as it came in

void swirl_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  int radius = (w < h ? w : h) / 2;
  if (radius < 2) {
    return;
  }

  size_t n = (size_t)w * (size_t)h;
  nl_memcpy(swirl_src, gray, n);
  int cx2 = w - 1; // Centre, doubled so even sizes get a half pixel centre
  int cy2 = h - 1;
  int max_phase = p * 6; // 1024 is one full turn

  for (int y = 0; y < h; y++) {
    int dy2 = 2 * y - cy2;
    for (int x = 0; x < w; x++) {
      int dx2 = 2 * x - cx2;
      int r2 = fx_isqrt(dx2 * dx2 + dy2 * dy2); // Twice real distance
      if (r2 >= 2 * radius) {
        continue;
      }

      int t = (2 * radius - r2) * 256 / (2 * radius); // 256 at centre, 0 at rim
      int phase = max_phase * t / 256 * t / 256;      // Eases out to rim
      int s = fx_sin(phase);
      int c = fx_sin(phase + 256);
      int sx2 = cx2 + (dx2 * c - dy2 * s) / 256;
      int sy2 = cy2 + (dx2 * s + dy2 * c) / 256;
      int sx = fx_clamp((sx2 + 1) / 2, 0, w - 1);
      int sy = fx_clamp((sy2 + 1) / 2, 0, h - 1);
      gray[y * w + x] = swirl_src[sy * w + sx];
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = swirl_process, .name = "swirl"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
