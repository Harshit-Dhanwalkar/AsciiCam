#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* sepia - old photograph tone, as far as luma alone goes: lifted blacks,
 * softened whites, a gentle S-curve and a little film grain
 * In colour mode (-C) the picture also gets the brown sepia tint
 * Parameter: 0 off .. 255 fully faded (128 default) */

static uint32_t sepia_seed = 0x5E91A001u;

void sepia_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  uint8_t lut[256];
  for (int v = 0; v < 256; v++) {
    int faded = 30 + v * 190 / 255; // lifted blacks, softened whites
    int curve = faded + (faded - 128) * (255 - fx_abs(faded - 128) * 2) / 1024;
    lut[v] = fx_u8(fx_mix(v, curve, p));
  }

  int grain = p / 16; // +/- levels
  int n = w * h;
  for (int i = 0; i < n; i++) {
    int v = lut[gray[i]];
    if (grain > 0) {
      v += fx_rand_n(&sepia_seed, 2 * grain + 1) - grain;
    }

    gray[i] = fx_u8(v);
  }
}

// Colour hook: classic sepia matrix, blended with the real colours
void sepia_rgb(uint8_t *rgb, int w, int h, void *ctx) {
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
    int r = px[0];
    int g = px[1];
    int b = px[2];
    int sr = (r * 393 + g * 769 + b * 189) / 1000;
    int sg = (r * 349 + g * 686 + b * 168) / 1000;
    int sb = (r * 272 + g * 534 + b * 131) / 1000;
    px[0] = fx_u8(fx_mix(r, fx_clamp(sr, 0, 255), p));
    px[1] = fx_u8(fx_mix(g, fx_clamp(sg, 0, 255), p));
    px[2] = fx_u8(fx_mix(b, fx_clamp(sb, 0, 255), p));
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = sepia_process, .name = "sepia"};

void plugin_rgb_process(uint8_t *rgb, int w, int h, void *ctx) {
  sepia_rgb(rgb, w, h, ctx);
}

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
