#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* posterize - fewer tones: picture is cut into flat bands, like a screen print
 * with a handful of inks
 * In colour mode (-C) every channel gets same bands
 * Parameter: 0 off .. 255 (128 default = 9 tones, 255 = 2, black and white) */

// Table for parameter p: 16 tones at 1, down to 2 at 255
static void posterize_lut(int p, uint8_t *lut) {
  int levels = 16 - p * 14 / 255;
  for (int v = 0; v < 256; v++) {
    lut[v] = (uint8_t)((v * levels / 256) * 255 / (levels - 1));
  }
}

void posterize_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  uint8_t lut[256];
  posterize_lut(p, lut);
  int n = w * h;
  for (int i = 0; i < n; i++) {
    gray[i] = lut[gray[i]];
  }
}

void posterize_rgb(uint8_t *rgb, int w, int h, void *ctx) {
  if (!fx_ok(rgb, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  uint8_t lut[256];
  posterize_lut(p, lut);
  size_t n = (size_t)w * (size_t)h * 3u;
  for (size_t i = 0; i < n; i++) {
    rgb[i] = lut[rgb[i]];
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = posterize_process,
                               .name = "posterize"};

void plugin_rgb_process(uint8_t *rgb, int w, int h, void *ctx) {
  posterize_rgb(rgb, w, h, ctx);
}

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
