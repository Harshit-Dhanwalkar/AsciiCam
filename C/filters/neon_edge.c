#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* neon_edge - glowing outlines on a darkened picture (Sobel edges plus a soft
 * halo)
 * Parameter: 0 off .. 255 outlines only (128 default) */

static uint8_t neon_src[FX_MAX_PIXELS];
static uint8_t neon_edge[FX_MAX_PIXELS];

void neon_edge_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  size_t n = (size_t)w * (size_t)h;
  nl_memcpy(neon_src, gray, n);
  nl_memset(neon_edge, 0, n);

  for (int y = 1; y < h - 1; y++) {
    for (int x = 1; x < w - 1; x++) {
      const uint8_t *s = neon_src + y * w + x;
      int gx = (s[-w + 1] + 2 * s[1] + s[w + 1]) -
               (s[-w - 1] + 2 * s[-1] + s[w - 1]);
      int gy = (s[w - 1] + 2 * s[w] + s[w + 1]) -
               (s[-w - 1] + 2 * s[-w] + s[-w + 1]);
      neon_edge[y * w + x] = fx_u8((fx_abs(gx) + fx_abs(gy)) / 2);
    }
  }

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      // halo: mean of 3x3 neighbourhood of edge map
      int sum = 0;
      int cnt = 0;
      for (int dy = -1; dy <= 1; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= h) {
          continue;
        }

        for (int dx = -1; dx <= 1; dx++) {
          int xx = x + dx;
          if (xx < 0 || xx >= w) {
            continue;
          }

          sum += neon_edge[yy * w + xx];
          cnt++;
        }
      }

      int i = y * w + x;
      int hi = neon_src[i] / 8 + neon_edge[i] * 2 + sum / cnt;
      gray[i] = fx_u8(fx_mix(neon_src[i], hi, p));
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = neon_edge_process,
                               .name = "neon_edge"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
