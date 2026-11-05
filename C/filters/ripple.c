#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* ripple - rings spreading from middle of picture, like a stone dropped in a
 * pond
 * Parameter: 0 off .. 255 deepest waves (128 default) */

static uint8_t ripple_src[FX_MAX_PIXELS];
static int ripple_frame;

void ripple_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  ripple_frame++;
  nl_memcpy(ripple_src, gray, (size_t)w * (size_t)h);

  int cx = w / 2;
  int cy = h / 2;
  int wave = (w < h ? w : h) / 6 + 1; // px between crests
  // displacement gradient = amp * 2pi / wave; keep it near 1 at most so picture
  // ripples instead of folding over itself
  int amp = 1 + wave * p / (255 * 6);
  int phase0 = ripple_frame * 24;

  for (int y = 0; y < h; y++) {
    int dy = y - cy;
    for (int x = 0; x < w; x++) {
      int dx = x - cx;
      int r = fx_isqrt(dx * dx + dy * dy);
      // radial displacement, rounded to nearest pixel (plain integer
      // division would flatten displacements below one pixel to nothing)
      int mag = fx_sin(r * 1024 / wave - phase0) * amp; // 256ths of a pixel
      int den = (r + 1) * 256;
      int ox = mag * dx;
      int oy = mag * dy;
      ox = (ox + (ox < 0 ? -den / 2 : den / 2)) / den;
      oy = (oy + (oy < 0 ? -den / 2 : den / 2)) / den;
      int sx = fx_clamp(x + ox, 0, w - 1);
      int sy = fx_clamp(y + oy, 0, h - 1);
      gray[y * w + x] = ripple_src[sy * w + sx];
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = ripple_process, .name = "ripple"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
