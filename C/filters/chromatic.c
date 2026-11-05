#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* chromatic - lens colour fringing, as far as luma alone can show it: picture
 * is split into a left and a right copy ("red" and "blue" planes drifting
 * apart) and every edge grows a bright fringe
 * Parameter: 0 off .. 255 widest split (128 default) */

static uint8_t chroma_row[FX_MAX_SIDE];

void chromatic_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  int d = 1 + p * w / (255 * 80); // px each channel moves

  for (int y = 0; y < h; y++) {
    uint8_t *row = gray + y * w;
    nl_memcpy(chroma_row, row, (size_t)w);

    for (int x = 0; x < w; x++) {
      int l = chroma_row[fx_clamp(x - d, 0, w - 1)];
      int r = chroma_row[fx_clamp(x + d, 0, w - 1)];
      int c = chroma_row[x];
      int split = (l + r) / 2;
      int fringe = fx_abs(l - r) * p / 400;
      row[x] = fx_u8(fx_mix(c, split, p) + fringe);
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = chromatic_process,
                               .name = "chromatic"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
