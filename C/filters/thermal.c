#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* thermal - heat-camera look, as far as luma alone goes: brightness is folded
 * into repeating up/down ramps, so every temperature band becomes a contour
 * ring
 * In colour mode (-C) it also paints a real heat palette: black, blue, magenta,
 * red, orange, yellow, white by brightness
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
    int t = v * THERMAL_BANDS % 256; // position inside band
    int tri = t < 128 ? t * 2 : (255 - t) * 2;
    // keep brightest band brightest overall so subject still reads
    int envelope = 96 + v * 159 / 255;
    lut[v] = fx_u8(fx_mix(v, tri * envelope / 255, p));
  }

  int n = w * h;
  for (int i = 0; i < n; i++) {
    gray[i] = lut[gray[i]];
  }
}

// Colour hook: map each pixel's brightness onto the heat palette and blend it
// with the real colours by the parameter
void thermal_rgb(uint8_t *rgb, int w, int h, void *ctx) {
  if (!fx_ok(rgb, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  // 7 anchors from cold to hot, 255 / 6 brightness apart; the table is built
  // once per call (256 entries), not per pixel
  static const uint8_t HEAT[7][3] = {
      {0, 0, 0},     {0, 0, 160},   {160, 0, 160},  {230, 20, 20},
      {255, 140, 0}, {255, 240, 0}, {255, 255, 255}};
  uint8_t pal[256][3];
  for (int v = 0; v < 256; v++) {
    int pos = v * 6 * 256 / 255; // 0 .. 6*256, fixed point 8 bits
    int seg = pos / 256;
    if (seg > 5) {
      seg = 5;
    }

    int frac = pos - seg * 256;
    if (frac > 255) {
      frac = 255;
    }

    for (int c = 0; c < 3; c++) {
      pal[v][c] = fx_u8(fx_mix(HEAT[seg][c], HEAT[seg + 1][c], frac));
    }
  }

  int n = w * h;
  for (int i = 0; i < n; i++) {
    uint8_t *px = rgb + (size_t)i * 3u;
    int lum = (px[0] * 77 + px[1] * 151 + px[2] * 28) / 256;
    for (int c = 0; c < 3; c++) {
      px[c] = fx_u8(fx_mix(px[c], pal[lum][c], p));
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = thermal_process, .name = "thermal"};

void plugin_rgb_process(uint8_t *rgb, int w, int h, void *ctx) {
  thermal_rgb(rgb, w, h, ctx);
}

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
