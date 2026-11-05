#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* tv_static - snow from a detuned television, with a slow rolling bar
 * Parameter: 0 off .. 255 pure static (128 default) */

static uint32_t static_seed = 0x2545F491u;
static int static_frame;

void tv_static_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  static_frame++;
  int bar_h = h / 10 + 1;
  int bar_y = (static_frame * 5) % (h + bar_h) - bar_h;

  for (int y = 0; y < h; y++) {
    int amount = p;
    if (y >= bar_y && y < bar_y + bar_h) {
      amount = fx_clamp(p + 70, 0, 255); // rolling bar is snowier
    }

    uint8_t *row = gray + y * w;
    uint32_t bits = 0;
    for (int x = 0; x < w; x++) {
      if (x % 4 == 0) {
        bits = fx_rand(&static_seed);
      }

      int noise = (int)(bits % 256u);
      bits /= 256u;
      row[x] = fx_u8(fx_mix(row[x], noise, amount));
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = tv_static_process,
                               .name = "tv_static"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
