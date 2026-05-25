#include "plugins.h"
#include <stdint.h>

#define DEFAULT_THRESH 128

static void thresh_process(uint8_t *gray, int w, int h, void *ctx) {
  uint8_t thresh = (uint8_t)(ctx ? *(int *)ctx : DEFAULT_THRESH);
  int total = w * h;
  for (int i = 0; i < total; i++)
    gray[i] = (gray[i] > thresh) ? 255 : 0;
}

static filter_plugin_t self = {
    .process = thresh_process,
    .name = "threshold",
};

filter_plugin_t *plugin_get(void) { return &self; }
