#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* pixel_sort - glitch-art smearing: in every row, each run of pixels brighter
 * than a threshold is sorted dark to bright
 * Parameter: 0 off .. 255 sorts nearly everything (128 default: pixels above
 * mid-grey) */

static void sort_run(uint8_t *a, int n) {
  if (n < 2) {
    return;
  }

  if (n < 32) {
    for (int i = 1; i < n; i++) {
      uint8_t v = a[i];
      int j = i - 1;
      while (j >= 0 && a[j] > v) {
        a[j + 1] = a[j];
        j--;
      }

      a[j + 1] = v;
    }

    return;
  }

  int count[256] = {0};
  for (int i = 0; i < n; i++) {
    count[a[i]]++;
  }

  int k = 0;
  for (int v = 0; v < 256; v++) {
    for (int c = count[v]; c > 0; c--) {
      a[k++] = (uint8_t)v;
    }
  }
}

void pixel_sort_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    return;
  }

  int lo = 256 - p; // pixels >= lo take part

  for (int y = 0; y < h; y++) {
    uint8_t *row = gray + y * w;
    int x = 0;
    while (x < w) {
      if (row[x] < lo) {
        x++;

        continue;
      }

      int start = x;
      while (x < w && row[x] >= lo) {
        x++;
      }

      sort_run(row + start, x - start);
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = pixel_sort_process,
                               .name = "pixel_sort"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
