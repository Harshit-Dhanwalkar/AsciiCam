#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* crt - old television tube: barrel-curved screen, scanlines, vignette
 * Parameter: 0 off .. 255 strongest (128 default) */

static uint8_t crt_src[FX_MAX_PIXELS];
static int crt_nx[FX_MAX_SIDE]; // column position, -512..512

void crt_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  nl_memcpy(crt_src, gray, (size_t)w * (size_t)h);

  int curve = p * 2 / 3; // 0..170 barrel strength
  int dark = p * 2 / 5;  // scanline depth, up to 40%
  int period = h / 60;   // about 30 visible lines
  if (period < 2) {
    period = 2;
  }

  for (int x = 0; x < w; x++) {
    crt_nx[x] = (2 * x - (w - 1)) * 512 / w;
  }

  for (int y = 0; y < h; y++) {
    int ny = (2 * y - (h - 1)) * 512 / h;
    int line_dark = (y % period) >= (period + 1) / 2 ? dark : 0;
    uint8_t *row = gray + y * w;

    for (int x = 0; x < w; x++) {
      int nx = crt_nx[x];
      int r2 = (nx * nx + ny * ny) / 512; // 0..1024
      int f = 1024 + r2 * curve / 512;
      int ux = (2 * x - (w - 1)) * f / 1024 + (w - 1);
      int uy = (2 * y - (h - 1)) * f / 1024 + (h - 1);
      int v = 0;
      if (ux >= 0 && uy >= 0 && ux / 2 < w && uy / 2 < h) {
        v = crt_src[uy / 2 * w + ux / 2];
      }

      v = v * (255 - line_dark) / 255;
      v = v * (1024 - r2 * p / 510) / 1024;
      row[x] = fx_u8(v);
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = crt_process, .name = "crt"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
