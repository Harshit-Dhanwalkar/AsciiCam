#include "plugins.h"
#include <stdint.h>

void do_invert(uint8_t *gray, int w, int h, void *ctx) {
  int strength = ctx ? *(int *)ctx : 255;
  int total_pixels = w * h;
  for (int i = 0; i < total_pixels; i++) {
    int inverted = 255 - gray[i];
    // Linear blend: output = original + strength/255 * (inverted - original)
    gray[i] = (uint8_t)(gray[i] + (inverted - gray[i]) * strength / 255);
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = do_invert, .name = "invert"};

filter_plugin_t *plugin_get(void) { return &self; }
#endif
