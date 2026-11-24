#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* bloom - glow:  bright parts of picture bleed into their surroundings like
 * light through a soft lens or a foggy night
 * Brightest half is blurred (box blur, horizontal then vertical, radius a
 * fortieth of frame's short side) and screened over original, so highlights
 * spread and shadows stay put Parameter: 0 off .. 255 strong glow (128 default)
 */

static uint8_t bloom_tmp[FX_MAX_PIXELS];    // Horizontally blurred highlights
static uint32_t bloom_sum[FX_MAX_SIDE + 1]; // Prefix sums of one row / column

void bloom_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  int short_side = w < h ? w : h;
  int r = 1 + short_side / 40;

  // Pass 1: highlights (0 below mid-grey, doubled above it), blurred along rows
  for (int y = 0; y < h; y++) {
    const uint8_t *row = gray + y * w;
    bloom_sum[0] = 0;
    for (int x = 0; x < w; x++) {
      int hl = row[x] > 127 ? (row[x] - 127) * 2 : 0;
      bloom_sum[x + 1] = bloom_sum[x] + (uint32_t)(hl > 255 ? 255 : hl);
    }

    uint8_t *out = bloom_tmp + y * w;
    for (int x = 0; x < w; x++) {
      int lo = x - r;
      if (lo < 0) {
        lo = 0;
      }
      int hi = x + r;
      if (hi >= w) {
        hi = w - 1;
      }
      if (hi < 0) {
        hi = 0;
      }
      out[x] = (uint8_t)((bloom_sum[hi + 1] - bloom_sum[lo]) /
                         (uint32_t)(hi - lo + 1));
    }
  }

  // Pass 2: blur down columns, screen result over picture
  for (int x = 0; x < w; x++) {
    bloom_sum[0] = 0;
    for (int y = 0; y < h; y++) {
      bloom_sum[y + 1] = bloom_sum[y] + bloom_tmp[y * w + x];
    }

    for (int y = 0; y < h; y++) {
      int lo = y - r;
      if (lo < 0) {
        lo = 0;
      }
      int hi = y + r;
      if (hi >= h) {
        hi = h - 1;
      }
      if (hi < 0) {
        hi = 0;
      }
      int glow =
          (int)((bloom_sum[hi + 1] - bloom_sum[lo]) / (uint32_t)(hi - lo + 1));
      glow = glow * p / 255;
      uint8_t *px = gray + y * w + x;
      *px = fx_u8(255 - (255 - *px) * (255 - glow) / 255); // screen blend
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = bloom_process, .name = "bloom"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
