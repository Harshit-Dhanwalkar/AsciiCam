#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* kaleidoscope - picture folded onto itself around its middle
 * Parameter: 0 off, 1..85 two-fold mirror, 86..170 four-fold, 171..255
 * eight-fold (128 default: four-fold)
 */

static uint8_t kal_src[FX_MAX_PIXELS];

void kaleidoscope_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  nl_memcpy(kal_src, gray, (size_t)w * (size_t)h);

  int folds = p <= 85 ? 2 : p <= 170 ? 4 : 8;
  int cx = w / 2;
  int cy = h / 2;

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int dx = fx_abs(x - cx);
      int dy = fx_abs(y - cy);
      if (folds == 2) {
        dy = y - cy;
      } else if (folds == 8 && dy > dx) {
        int t = dx;
        dx = dy;
        dy = t;
      }

      int sx = fx_clamp(cx + dx, 0, w - 1);
      int sy = fx_clamp(cy + dy, 0, h - 1);
      gray[y * w + x] = kal_src[sy * w + sx];
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = kaleidoscope_process,
                               .name = "kaleidoscope"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
