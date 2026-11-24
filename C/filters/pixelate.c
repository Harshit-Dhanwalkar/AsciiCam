#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* pixelate - big square pixels: every block becomes its average
 * Parameter: 0 off .. 255 block size (128 default = 10 pixels, 255 = 17) */

void pixelate_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  int c = 2 + p / 16; // Block side in pixels
  for (int by = 0; by < h; by += c) {
    int bh = by + c <= h ? c : h - by;
    for (int bx = 0; bx < w; bx += c) {
      int bw = bx + c <= w ? c : w - bx;
      int sum = 0;
      for (int y = 0; y < bh; y++) {
        const uint8_t *row = gray + (size_t)(by + y) * (size_t)w + bx;
        for (int x = 0; x < bw; x++) {
          sum += row[x];
        }
      }

      uint8_t mean = (uint8_t)(sum / (bw * bh));
      for (int y = 0; y < bh; y++) {
        uint8_t *row = gray + (size_t)(by + y) * (size_t)w + bx;
        for (int x = 0; x < bw; x++) {
          row[x] = mean;
        }
      }
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = pixelate_process, .name = "pixelate"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
