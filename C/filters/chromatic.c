#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* chromatic - lens colour fringing, as far as luma alone can show it: picture
 * is split into a left and a right copy ("red" and "blue" planes drifting
 * apart) and every edge grows a bright fringe
 * In colour mode (-C) the red and blue channels really do slide apart in
 * opposite directions
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

static uint8_t chroma_rgb_row[FX_MAX_SIDE * 3];

// Colour hook: red moves left, blue moves right, green stays
void chromatic_rgb(uint8_t *rgb, int w, int h, void *ctx) {
  if (!fx_ok(rgb, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  int d = 1 + p * w / (255 * 80);
  for (int y = 0; y < h; y++) {
    uint8_t *row = rgb + (size_t)y * (size_t)w * 3u;
    nl_memcpy(chroma_rgb_row, row, (size_t)w * 3u);
    for (int x = 0; x < w; x++) {
      row[x * 3 + 0] = chroma_rgb_row[fx_clamp(x + d, 0, w - 1) * 3 + 0];
      row[x * 3 + 2] = chroma_rgb_row[fx_clamp(x - d, 0, w - 1) * 3 + 2];
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = chromatic_process,
                               .name = "chromatic"};

void plugin_rgb_process(uint8_t *rgb, int w, int h, void *ctx) {
  chromatic_rgb(rgb, w, h, ctx);
}

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
