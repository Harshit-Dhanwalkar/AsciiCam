#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* optflow - which way things move: Lucas-Kanade optical flow on a grid
 *
 * Picture is cut in 16 x 16 pixel cells. For each cell, brightness gradients
 * of this frame and previous one give a 2 x 2 least-squares system whose
 * solution is cell's motion (u, v) in pixels per frame
 * A cell is trusted only when it has texture in both directions (its smaller
 * eigenvalue is large enough); flat walls and sky stay silent, since there
 * aperture problem makes answer meaningless
 *
 * Flow is drawn as a short white stroke from each moving cell, pointing where
 * it goes; rest of picture is dimmed. Arithmetic is integer fixed point (flow
 * in 1/16 pixel) with int64 accumulators: no floating point, no libm, nothing
 * allocated
 *
 * Parameter: 0 off .. 255 strongest dimming and longest strokes (128 default)
 */

#define OF_CELL 16
#define OF_MAX_CELLS 16384
#define OF_SUBPX 16     // flow units per pixel
#define OF_MAX_FLOW 128 // +-8 px per frame; faster motion is clamped
#define OF_MIN_MOVE 8   // strokes only for at least half a pixel of motion

static uint8_t of_prev[FX_MAX_PIXELS];
static int of_w, of_h, of_cw, of_ch;
static int16_t of_fu[OF_MAX_CELLS], of_fv[OF_MAX_CELLS];
static uint8_t of_ok[OF_MAX_CELLS];

static int of_at(const uint8_t *img, int w, int h, int x, int y) {
  x = fx_clamp(x, 0, w - 1);
  y = fx_clamp(y, 0, h - 1);

  return img[(size_t)y * (size_t)w + (size_t)x];
}

/* Flow of every cell from prev -> cur.
 * Let gx, gy be central differences summed over both frames (each is 2 x
 * brightness gradient, times 2 frames) and gt = 4 * (cur - prev)
 * Then gx*u + gy*v + gt = 0 is brightness constancy equation scaled by 4
 * everywhere, so solution is true flow */
static void of_estimate(const uint8_t *prev, const uint8_t *cur, int w, int h) {
  for (int cy = 0; cy < of_ch; cy++) {
    for (int cx = 0; cx < of_cw; cx++) {
      int i = cy * of_cw + cx;
      int x0 = cx * OF_CELL;
      int y0 = cy * OF_CELL;
      int x1 = fx_clamp(x0 + OF_CELL, 0, w);
      int y1 = fx_clamp(y0 + OF_CELL, 0, h);

      int64_t sxx = 0, syy = 0, sxy = 0, sxt = 0, syt = 0;
      int64_t npix = 0;
      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          int gx = of_at(cur, w, h, x + 1, y) - of_at(cur, w, h, x - 1, y) +
                   of_at(prev, w, h, x + 1, y) - of_at(prev, w, h, x - 1, y);
          int gy = of_at(cur, w, h, x, y + 1) - of_at(cur, w, h, x, y - 1) +
                   of_at(prev, w, h, x, y + 1) - of_at(prev, w, h, x, y - 1);
          int gt = 4 * (of_at(cur, w, h, x, y) - of_at(prev, w, h, x, y));

          sxx += (int64_t)gx * gx;
          syy += (int64_t)gy * gy;
          sxy += (int64_t)gx * gy;
          sxt += (int64_t)gx * gt;
          syt += (int64_t)gy * gt;
          npix++;
        }
      }

      of_ok[i] = 0;
      of_fu[i] = 0;
      of_fv[i] = 0;

      int64_t det = sxx * syy - sxy * sxy;
      int64_t tr = sxx + syy;
      if (npix == 0 || det <= 0 || tr <= 0) {
        continue;
      }

      // Smaller eigenvalue is about det / tr. Need it to average at least 4
      // per pixel in gx units (gradient of about one brightness level along
      // each axis), otherwise cell is flat or a single edge
      if (det / tr < 4 * npix) {
        continue;
      }

      // Cramer's rule for [sxx sxy; sxy syy] [u; v] = -[sxt; syt]
      int64_t u = -(syy * sxt - sxy * syt) * OF_SUBPX / det;
      int64_t v = -(sxx * syt - sxy * sxt) * OF_SUBPX / det;

      of_fu[i] = (int16_t)fx_clamp((int)u, -OF_MAX_FLOW, OF_MAX_FLOW);
      of_fv[i] = (int16_t)fx_clamp((int)v, -OF_MAX_FLOW, OF_MAX_FLOW);
      of_ok[i] = 1;
    }
  }
}

static void of_plot(uint8_t *gray, int w, int h, int x, int y) {
  if (x >= 0 && y >= 0 && x < w && y < h) {
    gray[(size_t)y * (size_t)w + (size_t)x] = 255;
  }
}

// Bresenham line, clipped per pixel
static void of_line(uint8_t *gray, int w, int h, int x0, int y0, int x1,
                    int y1) {
  int dx = fx_abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -fx_abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  for (;;) {
    of_plot(gray, w, h, x0, y0);
    if (x0 == x1 && y0 == y1) {
      break;
    }

    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void optflow_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  int cw = (w + OF_CELL - 1) / OF_CELL;
  int ch = (h + OF_CELL - 1) / OF_CELL;
  if (cw * ch > OF_MAX_CELLS) {
    return; // too many cells for tables: pass through
  }

  size_t n = (size_t)w * (size_t)h;
  int first = (of_w != w || of_h != h);
  if (first) {
    of_w = w;
    of_h = h;
    of_cw = cw;
    of_ch = ch;
    nl_memset(of_ok, 0, sizeof(of_ok));
  } else {
    of_estimate(of_prev, gray, w, h);
  }

  // Keep untouched frame as next "previous"; drawing below must not leak into
  // flow estimate
  nl_memcpy(of_prev, gray, n);

  if (p == 0 || first) {
    return;
  }

  for (size_t k = 0; k < n; k++) {
    gray[k] = fx_u8(fx_mix(gray[k], gray[k] / 4, p));
  }

  int stretch = 2 + p / 32;
  for (int cy = 0; cy < ch; cy++) {
    for (int cx = 0; cx < cw; cx++) {
      int i = cy * cw + cx;
      if (!of_ok[i]) {
        continue;
      }

      int fu = of_fu[i];
      int fv = of_fv[i];
      if (fx_abs(fu) + fx_abs(fv) < OF_MIN_MOVE) {
        continue;
      }

      int x0 = cx * OF_CELL + OF_CELL / 2;
      int y0 = cy * OF_CELL + OF_CELL / 2;
      int x1 = x0 + (fu * stretch) / OF_SUBPX;
      int y1 = y0 + (fv * stretch) / OF_SUBPX;
      of_line(gray, w, h, x0, y0, x1, y1);
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = optflow_process, .name = "optflow"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
