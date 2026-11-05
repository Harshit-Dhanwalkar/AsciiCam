#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* fisheye - wide-angle bulge: middle of picture is magnified, edges squeezed
 * Parameter: 0 off .. 255 strongest bulge (128 default) */

static uint8_t fish_src[FX_MAX_PIXELS];
static int fish_nx[FX_MAX_SIDE];

void fisheye_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  nl_memcpy(fish_src, gray, (size_t)w * (size_t)h);

  int k = p * 3 / 2; // 0..382; sampling radius shrinks by k/512 at corners
  for (int x = 0; x < w; x++) {
    fish_nx[x] = (2 * x - (w - 1)) * 512 / w;
  }

  for (int y = 0; y < h; y++) {
    int ny = (2 * y - (h - 1)) * 512 / h;
    for (int x = 0; x < w; x++) {
      int nx = fish_nx[x];
      int r2 = (nx * nx + ny * ny) / 512; // 0..1024
      int f = 1024 - r2 * k / 1024;       // 1024 in middle, ~650 at worst
      // doubled coordinates keep f == 1024 an exact identity
      int sx = ((2 * x - (w - 1)) * f / 1024 + (w - 1)) / 2;
      int sy = ((2 * y - (h - 1)) * f / 1024 + (h - 1)) / 2;
      gray[y * w + x] =
          fish_src[fx_clamp(sy, 0, h - 1) * w + fx_clamp(sx, 0, w - 1)];
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = fisheye_process, .name = "fisheye"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
