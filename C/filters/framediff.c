#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* framediff - highlight what moved: pixels that changed since previous frame
 * glow, still background fades away.
 *
 * Parameter: 0 off .. 255 only motion is visible (128 default) */

static uint8_t diff_prev[FX_MAX_PIXELS];
static int diff_w, diff_h;

void framediff_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  size_t n = (size_t)w * (size_t)h;
  if (diff_w != w || diff_h != h) {
    nl_memcpy(diff_prev, gray, n);
    diff_w = w;
    diff_h = h;

    if (p > 0) {
      // nothing to compare with yet: show still background only
      for (size_t i = 0; i < n; i++) {
        gray[i] = (uint8_t)fx_mix(gray[i], gray[i] / 8, p);
      }
    }

    return;
  }

  int gain = 2 + p / 16;
  for (size_t i = 0; i < n; i++) {
    int cur = gray[i];
    int moved = fx_abs(cur - diff_prev[i]);
    diff_prev[i] = (uint8_t)cur;
    if (p > 0) {
      int hi = cur / 8 + moved * gain;
      gray[i] = fx_u8(fx_mix(cur, hi, p));
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = framediff_process,
                               .name = "framediff"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
