#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* vignette - darken toward corners, like a fast lens wide open
 * Parameter: 0 off .. 255 near-black corners (128 default) */

static int vig_nx2[FX_MAX_SIDE]; // (column position)^2 / 512

void vignette_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  for (int x = 0; x < w; x++) {
    int nx = (2 * x - (w - 1)) * 512 / w;
    vig_nx2[x] = nx * nx / 512;
  }

  for (int y = 0; y < h; y++) {
    int ny = (2 * y - (h - 1)) * 512 / h;
    int ny2 = ny * ny / 512;
    uint8_t *row = gray + y * w;

    for (int x = 0; x < w; x++) {
      int r2 = vig_nx2[x] + ny2; // 0..1024
      int r4 = r2 * r2 / 1024;   // quartic falloff: flat centre
      int f = 1024 - r4 * p / 280;
      row[x] = fx_u8(row[x] * fx_clamp(f, 0, 1024) / 1024);
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = vignette_process, .name = "vignette"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
