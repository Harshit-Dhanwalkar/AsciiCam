#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* solarize - Sabattier effect: tones above a threshold flip, so bright areas
 * turn dark again and a halo of reversed tones follows every edge
 * In colour mode (-C) every channel is flipped above same threshold Parameter:
 * 0 off .. 255 (128 default): how far down threshold moves
 * 128 flips upper half of tones, 255 flips everything but black */

// Tone table for parameter p: values at or above 256 - p are inverted
static void solarize_lut(int p, uint8_t *lut) {
  int t = 256 - p;
  for (int v = 0; v < 256; v++) {
    lut[v] = (uint8_t)(v >= t ? 255 - v : v);
  }
}

void solarize_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  uint8_t lut[256];
  solarize_lut(p, lut);
  int n = w * h;
  for (int i = 0; i < n; i++) {
    gray[i] = lut[gray[i]];
  }
}

// Colour hook: same table on red, green and blue
void solarize_rgb(uint8_t *rgb, int w, int h, void *ctx) {
  if (!fx_ok(rgb, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  uint8_t lut[256];
  solarize_lut(p, lut);
  size_t n = (size_t)w * (size_t)h * 3u;
  for (size_t i = 0; i < n; i++) {
    rgb[i] = lut[rgb[i]];
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = solarize_process, .name = "solarize"};

void plugin_rgb_process(uint8_t *rgb, int w, int h, void *ctx) {
  solarize_rgb(rgb, w, h, ctx);
}

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
