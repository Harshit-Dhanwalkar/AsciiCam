#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* nightvision - image intensifier: dark scenes are lifted hard (a concave
 * curve, so shadows gain most), a little sensor grain is added and picture is
 * clipped into a round lens
 * In colour mode (-C) picture is painted in green of a phosphor tube
 * Parameter: 0 off .. 255 strongest lift, grain and tube (128 default) */

static uint32_t nv_seed = 0x1234ABCDu;

void nightvision_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  uint8_t lift[256];
  for (int v = 0; v < 256; v++) {
    // sqrt curve: 64 -> 127, 16 -> 63; 255 stays 255
    lift[v] = fx_u8(fx_mix(v, fx_isqrt(v * 255), p));
  }

  int grain = 2 + p / 12; // Peak grain, about +-7 at default
  for (int y = 0; y < h; y++) {
    int ny = (2 * y - (h - 1)) * 512 / h;
    uint8_t *row = gray + y * w;
    for (int x = 0; x < w; x++) {
      int nx = (2 * x - (w - 1)) * 512 / w;
      int d2 = (nx * nx + ny * ny) / 512; // 0 centre .. 1024 corner
      int v = lift[row[x]] + fx_rand_n(&nv_seed, 2 * grain + 1) - grain;
      // Lens: full brightness inside, dark beyond 90% of half-diagonal
      int lens =
          d2 < 640 ? 256 : fx_clamp(256 - (d2 - 640) * 256 / 384, 0, 256);
      row[x] = fx_u8(fx_mix(row[x], v * lens / 256, p));
    }
  }
}

// Colour hook: brightness becomes phosphor green; parameter blends it with real
// colours
void nightvision_rgb(uint8_t *rgb, int w, int h, void *ctx) {
  if (!fx_ok(rgb, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  int n = w * h;
  for (int i = 0; i < n; i++) {
    uint8_t *px = rgb + (size_t)i * 3u;
    int lum = (px[0] * 77 + px[1] * 151 + px[2] * 28) / 256;
    px[0] = fx_u8(fx_mix(px[0], lum / 6, p));
    px[1] = fx_u8(fx_mix(px[1], lum, p));
    px[2] = fx_u8(fx_mix(px[2], lum / 4, p));
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = nightvision_process,
                               .name = "nightvision"};

void plugin_rgb_process(uint8_t *rgb, int w, int h, void *ctx) {
  nightvision_rgb(rgb, w, h, ctx);
}

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
