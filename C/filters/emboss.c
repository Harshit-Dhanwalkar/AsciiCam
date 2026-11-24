#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* emboss - picture as a relief lit from top left: flat areas go mid grey, edges
 * facing light turn bright and those facing away dark Parameter: 0 off .. 255
 * relief depth (128 default) */

// Rows of original picture: one above and one being worked on
static uint8_t emboss_up[FX_MAX_SIDE];
static uint8_t emboss_cur[FX_MAX_SIDE];

void emboss_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0 || w < 3 || h < 3) {
    return;
  }

  nl_memcpy(emboss_up, gray, (size_t)w); // Row above row 0 is row 0
  for (int y = 0; y < h; y++) {
    uint8_t *row = gray + (size_t)y * (size_t)w;
    nl_memcpy(emboss_cur, row, (size_t)w);
    // Rows below are still original: only current one is rewritten
    const uint8_t *dn = y + 1 < h ? row + w : emboss_cur;
    for (int x = 0; x < w; x++) {
      int l = x > 0 ? x - 1 : 0;
      int r = x + 1 < w ? x + 1 : w - 1;
      int d = dn[r] - emboss_up[l]; // Lower right against upper left
      row[x] = fx_u8(128 + d * p / 128);
    }

    nl_memcpy(emboss_up, emboss_cur, (size_t)w);
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = emboss_process, .name = "emboss"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
