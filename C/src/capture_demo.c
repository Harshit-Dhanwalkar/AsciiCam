#include "nolibc.h"

#include "demo.h"

// \sin(2 * \pi * i / 256) * 127, i in 0..255, from a parabola approximation
// (error < 1 level, plenty for a picture)
static int isin(int i) {
  i &= 255;
  int neg = i >= 128;
  int x = i & 127; // 0..127: half period
  // 4x * (128 - x) / 128^2 scaled to 0..127: parabola through 0, 127 at 64, 0
  int v = (x * (128 - x) * 127) / (64 * 64);
  v = v > 127 ? 127 : v;

  return neg ? -v : v;
}

int demo_is_device(const char *device) {
  return device && nl_strcmp(device, DEMO_DEVICE) == 0;
}

void demo_fill_yuyv(uint8_t *yuyv, int w, int h, int stride, long t) {
  int ph = (int)(t % DEMO_PERIOD);
  int cx = w / 2 + (isin(ph) * w) / 400; // a moving centre of interest
  int cy = h / 2 + (isin(ph + 64) * h) / 400;

  for (int y = 0; y < h; y++) {
    uint8_t *row = yuyv + (size_t)y * (size_t)stride;
    for (int x = 0; x < w; x += 2) {
      uint8_t yy[2];
      int u = 0, v = 0;
      for (int k = 0; k < 2; k++) {
        int px = x + k;
        int dx = px - cx, dy = (y - cy) * 2; // cells are about twice as tall
        int d2 = (dx * dx + dy * dy) / 16;
        int s = isin(px * 3 + ph * 2) + isin(y * 5 - ph * 3) +
                isin((px + y) * 2 + ph) + isin(d2 - ph * 4);
        int l = 128 + s / 4 + (isin(d2 / 4 - ph * 2) / 4);
        yy[k] = (uint8_t)(l < 16 ? 16 : l > 235 ? 235 : l);
        if (k == 0) {
          u = 128 + isin(px + ph * 2) / 2;
          v = 128 + isin(y * 2 - ph) / 2;
        }
      }

      row[x * 2 + 0] = yy[0];
      row[x * 2 + 1] = (uint8_t)u;
      row[x * 2 + 2] = yy[1];
      row[x * 2 + 3] = (uint8_t)v;
    }
  }
}
