#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* halftone - newspaper print: picture is cut into square cells and each one
 * becomes a dark dot on white, bigger where picture is darker Parameter: 0 off
 * .. 255 cell size (128 default = 8 pixels, 255 = 13) */

void halftone_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  int c = 3 + p / 24; // Cell side in pixels
  for (int cy = 0; cy < h; cy += c) {
    int ch = cy + c <= h ? c : h - cy;
    for (int cx = 0; cx < w; cx += c) {
      int cw = cx + c <= w ? c : w - cx;
      int sum = 0;
      for (int y = 0; y < ch; y++) {
        const uint8_t *row = gray + (size_t)(cy + y) * (size_t)w + cx;
        for (int x = 0; x < cw; x++) {
          sum += row[x];
        }
      }

      int mean = sum / (cw * ch);
      // Dot area is cell area times darkness: 4 * r^2 = c^2 * dark / (pi * 255)
      // * 4 (everything below is in half pixels)
      int limit = c * c * (255 - mean) * 1273 / (255 * 1000);
      for (int y = 0; y < ch; y++) {
        uint8_t *row = gray + (size_t)(cy + y) * (size_t)w + cx;
        int dy = 2 * y + 1 - c;
        for (int x = 0; x < cw; x++) {
          int dx = 2 * x + 1 - c;
          row[x] = (uint8_t)(dx * dx + dy * dy < limit ? 0 : 255);
        }
      }
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = halftone_process, .name = "halftone"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
