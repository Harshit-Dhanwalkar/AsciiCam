#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* thermal - heat-camera look, as far as luma alone goes: brightness is folded
 * into repeating up/down ramps, so every temperature band becomes a contour
 * ring. (Real thermal palettes need colour, which plugins don't receive)
 * Parameter: 0 off .. 255 blended in fully; the number of bands is fixed at 4
 * (128 default is a half blend) */

#define THERMAL_BANDS 4

void thermal_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  uint8_t lut[256];
  for (int v = 0; v < 256; v++) {
    int t = v * THERMAL_BANDS % 256; // position inside the band
    int tri = t < 128 ? t * 2 : (255 - t) * 2;
    // keep the brightest band brightest overall so the subject still reads
    int envelope = 96 + v * 159 / 255;
    lut[v] = fx_u8(fx_mix(v, tri * envelope / 255, p));
  }

  int n = w * h;
  for (int i = 0; i < n; i++) {
    gray[i] = lut[gray[i]];
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = thermal_process, .name = "thermal"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
