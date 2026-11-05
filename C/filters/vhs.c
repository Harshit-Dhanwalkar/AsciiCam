#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* vhs - worn tape: wobbling lines, a tracking band rolling down picture,
 * horizontal smear and speckle
 * Parameter: 0 off .. 255 wrecked (128 default) */

static uint32_t vhs_seed = 0x51ED270Bu;
static int vhs_frame;
static uint8_t vhs_row[FX_MAX_SIDE];

void vhs_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  vhs_frame++;
  int band_h = h / 12 + 1;
  int band_y = (vhs_frame * 3) % (h + 2 * band_h) - band_h;
  int wobble = w * p / (255 * 90); // px, gentle line wobble
  int tear = w * p / (255 * 12);   // px, inside tracking band
  int smear = 1 + p / 100;         // blur radius in px

  for (int y = 0; y < h; y++) {
    uint8_t *row = gray + y * w;
    int in_band = y >= band_y && y < band_y + band_h;

    int shift = fx_sin(y * 11 + vhs_frame * 37) * wobble / 256;
    if (in_band) {
      shift += fx_rand_n(&vhs_seed, 2 * tear + 1) - tear;
    }

    for (int x = 0; x < w; x++) {
      int sx = fx_clamp(x - shift, 0, w - 1);
      vhs_row[x] = row[sx];
    }

    for (int x = 0; x < w; x++) {
      int lo = fx_clamp(x - smear, 0, w - 1);
      int hi = fx_clamp(x + smear, 0, w - 1);
      int v = (vhs_row[lo] + 2 * vhs_row[x] + vhs_row[hi]) / 4;
      if (in_band) {
        v += 30 * p / 255;
        if (fx_rand_n(&vhs_seed, 3) == 0) {
          v += fx_rand_n(&vhs_seed, 120) * p / 255;
        }
      } else if (fx_rand_n(&vhs_seed, 64) == 0) {
        v += (fx_rand_n(&vhs_seed, 61) - 30) * p / 255; // speckle
      }

      row[x] = fx_u8(v);
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = vhs_process, .name = "vhs"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
