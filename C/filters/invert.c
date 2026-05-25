#include "plugins.h"
#include <stdint.h>

static void do_invert(uint8_t *gray, int w, int h, void *ctx) {
  (void)ctx;
  int total_pixels = w * h;
  for (int i = 0; i < total_pixels; i++) {
    gray[i] = 255 - gray[i]; // Flip brightness
  }
}

static filter_plugin_t self = {.process = do_invert, .name = "Invert"};

filter_plugin_t *plugin_get(void) { return &self; }
