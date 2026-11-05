#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* dither_bayer - ordered dithering with an 8x8 Bayer matrix
 * Pattern is  drawn at roughly Braille-cell resolution (width / 160 px per dot)
 * so it stays visible after ASCII conversion
 * Parameter: number of grey levels, 2 (p=0) .. 17 (p=255); 128 gives 10.
 */

static const uint8_t bayer8[64] = {
    0,  32, 8,  40, 2,  34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
    12, 44, 4,  36, 14, 46, 6,  38, 60, 28, 52, 20, 62, 30, 54, 22,
    3,  35, 11, 43, 1,  33, 9,  41, 51, 19, 59, 27, 49, 17, 57, 25,
    15, 47, 7,  39, 13, 45, 5,  37, 63, 31, 55, 23, 61, 29, 53, 21};

void dither_bayer_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  int levels = 2 + p / 16;
  int step = 255 / (levels - 1); // distance between output levels
  int cell = w / 160 > 1 ? w / 160 : 1;

  for (int y = 0; y < h; y++) {
    uint8_t *row = gray + y * w;
    int by = (y / cell) % 8;
    for (int x = 0; x < w; x++) {
      int t = bayer8[by * 8 + (x / cell) % 8]; // 0..63
      int v = row[x] + (t - 32) * step / 64;
      int q = (v + step / 2) / step;
      row[x] = fx_u8(q * step);
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = dither_bayer_process,
                               .name = "dither_bayer"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
