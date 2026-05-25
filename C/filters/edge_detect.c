#include "plugins.h"
#include <stdint.h>
#include <stdlib.h>

static inline int clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void do_edge_boost(uint8_t *gray, int w, int h, void *ctx) {
  (void)ctx;
  // Unsharp mask: sharpened = original + (original - blurred) * strength
  uint8_t *tmp = malloc(w * h);
  if (!tmp)
    return;

  // 3x3 box blur
  for (int y = 1; y < h - 1; y++) {
    for (int x = 1; x < w - 1; x++) {
      int sum = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
          sum += gray[(y + dy) * w + (x + dx)];
      tmp[y * w + x] = (uint8_t)(sum / 9);
    }
  }

  // Sharpen: original + 0.5 * (original - blur)
  for (int i = w + 1; i < w * (h - 1) - 1; i++)
    gray[i] = (uint8_t)clamp(gray[i] + (gray[i] - tmp[i]) / 2);

  free(tmp);
}

static filter_plugin_t self = {do_edge_boost, "edge_boost"};
filter_plugin_t *plugin_get(void) { return &self; }
