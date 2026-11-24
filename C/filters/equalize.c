#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* equalize - histogram equalisation: spreads brightness actually present in
 * frame over whole 0..255 range, so a dim room or a washed-out picture gets
 * full contrast, which is what an ASCII ramp needs most
 * Parameter: 0 off .. 255 fully equalised (128 default is a half blend)
 * A frame with one brightness only has nothing to spread and is left alone */

void equalize_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  int n = w * h;
  int hist[256] = {0};
  for (int i = 0; i < n; i++) {
    hist[gray[i]]++;
  }

  // First occupied level: everything below it has an empty cumulative count
  int first = 0;
  while (first < 255 && hist[first] == 0) {
    first++;
  }

  if (hist[first] == n) {
    return;
  }

  uint8_t lut[256];
  int cum = 0;
  int span = n - hist[first]; // Pixels above first level
  for (int v = 0; v < 256; v++) {
    if (v < first) {
      lut[v] = (uint8_t)v;

      continue;
    }

    cum += hist[v];
    // (cdf - cdf_min) / (n - cdf_min): first level -> 0, brightest -> 255
    int eq = (int)((int64_t)(cum - hist[first]) * 255 / span);
    lut[v] = fx_u8(fx_mix(v, eq, p));
  }

  for (int i = 0; i < n; i++) {
    gray[i] = lut[gray[i]];
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = equalize_process, .name = "equalize"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
