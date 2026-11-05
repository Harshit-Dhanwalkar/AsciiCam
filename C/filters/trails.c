#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* trails - motion ghosting: each frame keeps a fading copy of what was brighter
 * before
 * Parameter: 0 off .. 255 very long trails (128 default keeps about 3/4 of
 * previous frame per step) */

static uint8_t trails_prev[FX_MAX_PIXELS];
static int trails_w, trails_h;

void trails_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  size_t n = (size_t)w * (size_t)h;
  if (p == 0 || trails_w != w || trails_h != h) {
    // off, or first frame / new size: start a fresh history
    nl_memcpy(trails_prev, gray, n);
    trails_w = w;
    trails_h = h;

    return;
  }

  int keep = 255 - (255 - p) * (255 - p) / 255; // 0 .. 255, curved
  for (size_t i = 0; i < n; i++) {
    int decayed = trails_prev[i] * keep / 256;
    int v = gray[i] > decayed ? gray[i] : decayed;
    gray[i] = (uint8_t)v;
    trails_prev[i] = (uint8_t)v;
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = trails_process, .name = "trails"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
