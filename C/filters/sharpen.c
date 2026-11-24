#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* sharpen - unsharp mask: each pixel moves away from average of its 3x3
 * neighbourhood, so edges and fine texture stand out Parameter: 0 off .. 255
 * amount (128 default = add back whole difference, 255 = twice that) */

// Rows of original picture: one above and one being worked on
static uint8_t sharpen_up[FX_MAX_SIDE];
static uint8_t sharpen_cur[FX_MAX_SIDE];

void sharpen_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0 || w < 3 || h < 3) {
    return;
  }

  nl_memcpy(sharpen_up, gray, (size_t)w); // Row above row 0 is row 0
  for (int y = 0; y < h; y++) {
    uint8_t *row = gray + (size_t)y * (size_t)w;
    nl_memcpy(sharpen_cur, row, (size_t)w);
    // Rows below are still original: only current one is rewritten
    const uint8_t *dn = y + 1 < h ? row + w : sharpen_cur;
    for (int x = 0; x < w; x++) {
      int l = x > 0 ? x - 1 : 0;
      int r = x + 1 < w ? x + 1 : w - 1;
      int blur = (sharpen_up[l] + 2 * sharpen_up[x] + sharpen_up[r] +
                  2 * sharpen_cur[l] + 4 * sharpen_cur[x] + 2 * sharpen_cur[r] +
                  dn[l] + 2 * dn[x] + dn[r]) /
                 16;
      int v = sharpen_cur[x];
      row[x] = fx_u8(v + (v - blur) * p / 128);
    }

    nl_memcpy(sharpen_up, sharpen_cur, (size_t)w);
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = sharpen_process, .name = "sharpen"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
