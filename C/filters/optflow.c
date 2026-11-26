#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* optflow - which way things move: coarse-to-fine Lucas-Kanade on a grid
 *
 * Picture is cut in 16 x 16 pixel cells. For each cell, brightness gradients
 * of this frame and previous one give a 2 x 2 least-squares system whose
 * solution is cell's motion (u, v) in pixels per frame
 * A cell is trusted only when it has texture in both directions (its smaller
 * eigenvalue is large enough); flat walls and single edges stay silent, since
 * there aperture problem makes answer meaningless
 *
 * Single-scale LK is accurate only for about 2 px per frame, so flow is
 * estimated on a 3-level pyramid (full, half, quarter resolution), coarse
 * first
 * Each finer level starts from coarser flow, rounded to whole pixels and used
 * to shift previous frame; remaining sub-pixel residual is solved as usual
 * Motion of about 8 px per frame at full size is then within reach
 *
 * Flow is drawn as a short white stroke from each moving cell, pointing where
 * it goes; rest of picture is dimmed
 * Arithmetic is integer fixed point (flow
 * in 1/16 pixel) with int64 accumulators: no floating point (flow in 1/16 pixel
 * of the level it belongs to) with int64 accumulators: no floating point, no
 * libm, nothing allocated
 *
 * Parameter: 0 off .. 255 strongest dimming and longest strokes (128 default)
 */

#define OF_CELL 16
#define OF_LEVELS 3
#define OF_MAX_CELLS 16384
#define OF_SUBPX 16       // flow units per pixel
#define OF_MAX_RESID 128  // sub-pixel residual per level: +-8 px
#define OF_MAX_TOTAL 2048 // total flow per level: +-128 px at that level
#define OF_MIN_MOVE 8     // strokes only for at least half a pixel of motion

// Level sizes: ceil(w / 2^l) x ceil(h / 2^l)
// Bounds hold for any frame that fx_ok() accepts (w*h <= FX_MAX_PIXELS, sides
// <= FX_MAX_SIDE)
static uint8_t of_p1[1 << 19], of_c1[1 << 19];
static uint8_t of_p2[1 << 18], of_c2[1 << 18];
static uint8_t of_prev[FX_MAX_PIXELS];
static int of_w, of_h;
static int16_t of_lu[OF_LEVELS][OF_MAX_CELLS], of_lv[OF_LEVELS][OF_MAX_CELLS];
static uint8_t of_lok[OF_LEVELS][OF_MAX_CELLS];
static int of_lw[OF_LEVELS], of_lh[OF_LEVELS], of_lcw[OF_LEVELS],
    of_lch[OF_LEVELS];

static int of_at(const uint8_t *img, int w, int h, int x, int y) {
  x = fx_clamp(x, 0, w - 1);
  y = fx_clamp(y, 0, h - 1);

  return img[(size_t)y * (size_t)w + (size_t)x];
}

static int of_ceil_shift(int v, int l) { return (v + (1 << l) - 1) >> l; }

// Half-size image by 2 x 2 mean (rounded). Odd sizes repeat the last column
static void of_halve(const uint8_t *src, int sw, int sh, uint8_t *dst) {
  int dw = (sw + 1) / 2, dh = (sh + 1) / 2;
  for (int y = 0; y < dh; y++) {
    for (int x = 0; x < dw; x++) {
      int s = of_at(src, sw, sh, 2 * x, 2 * y) +
              of_at(src, sw, sh, 2 * x + 1, 2 * y) +
              of_at(src, sw, sh, 2 * x, 2 * y + 1) +
              of_at(src, sw, sh, 2 * x + 1, 2 * y + 1);
      dst[(size_t)y * (size_t)dw + (size_t)x] = (uint8_t)((s + 2) / 4);
    }
  }
}

// Whole pixels nearest to a flow value in 1/16 pixel (round half away)
static int of_round_px(int g) {
  return g >= 0 ? (g + OF_SUBPX / 2) / OF_SUBPX
                : -((-g + OF_SUBPX / 2) / OF_SUBPX);
}

/* Flow of every cell of one level, prev -> cur.
 * Parent (coarser) flow, if given, is doubled and rounded to whole pixels;
 * prev is read at that offset, so the solution is the residual only. Gradients
 * are summed over the two (shifted) frames: gx, gy are 2 x the brightness
 * gradient times 2 frames, gt = 4 * (cur - shifted prev). Then gx*u + gy*v +
 * gt = 0 is the constancy equation scaled by 4, and the solution is the flow */
static void of_level(const uint8_t *prev, const uint8_t *cur, int w, int h,
                     int l, const int16_t *pu, const int16_t *pv,
                     const uint8_t *pok) {
  int cw = of_lcw[l];
  int ch = of_lch[l];
  int pcw = l + 1 < OF_LEVELS ? of_lcw[l + 1] : 0;
  for (int cy = 0; cy < ch; cy++) {
    for (int cx = 0; cx < cw; cx++) {
      int i = cy * cw + cx;
      int gu = 0;
      int gv = 0;
      if (pu != NULL) {
        int pi = (cy / 2) * pcw + cx / 2;
        if (pok[pi]) {
          gu = pu[pi] * 2;
          gv = pv[pi] * 2;
        }
      }
      int sx = of_round_px(gu);
      int sy = of_round_px(gv);

      int x0 = cx * OF_CELL;
      int y0 = cy * OF_CELL;
      int x1 = fx_clamp(x0 + OF_CELL, 0, w);
      int y1 = fx_clamp(y0 + OF_CELL, 0, h);

      int64_t sxx = 0;
      int64_t syy = 0;
      int64_t sxy = 0;
      int64_t sxt = 0;
      int64_t syt = 0;
      int64_t npix = 0;
      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          // Warped prev: cur(x) ~ prev(x - s), so the flow convention holds
          int px = x - sx;
          int py = y - sy;
          int gx = of_at(cur, w, h, x + 1, y) - of_at(cur, w, h, x - 1, y) +
                   of_at(prev, w, h, px + 1, py) -
                   of_at(prev, w, h, px - 1, py);
          int gy = of_at(cur, w, h, x, y + 1) - of_at(cur, w, h, x, y - 1) +
                   of_at(prev, w, h, px, py + 1) -
                   of_at(prev, w, h, px, py - 1);
          int gt = 4 * (of_at(cur, w, h, x, y) - of_at(prev, w, h, px, py));

          sxx += (int64_t)gx * gx;
          syy += (int64_t)gy * gy;
          sxy += (int64_t)gx * gy;
          sxt += (int64_t)gx * gt;
          syt += (int64_t)gy * gt;
          npix++;
        }
      }

      of_lok[l][i] = 0;
      of_lu[l][i] = 0;
      of_lv[l][i] = 0;

      int64_t det = sxx * syy - sxy * sxy;
      int64_t tr = sxx + syy;
      if (npix == 0 || det <= 0 || tr <= 0) {
        continue;
      }

      // Smaller eigenvalue is about det / tr
      // Need it to average at least 4 per pixel in gx units (gradient of about
      // one brightness level along each axis), otherwise cell is flat or a
      // single edge
      if (det / tr < 4 * npix) {
        continue;
      }

      // Cramer's rule for [sxx sxy; sxy syy] [u; v] = -[sxt; syt]
      int64_t ur = -(syy * sxt - sxy * syt) * OF_SUBPX / det;
      int64_t vr = -(sxx * syt - sxy * sxt) * OF_SUBPX / det;
      ur = fx_clamp((int)ur, -OF_MAX_RESID, OF_MAX_RESID);
      vr = fx_clamp((int)vr, -OF_MAX_RESID, OF_MAX_RESID);

      int tu = sx * OF_SUBPX + (int)ur;
      int tv = sy * OF_SUBPX + (int)vr;
      of_lu[l][i] = (int16_t)fx_clamp(tu, -OF_MAX_TOTAL, OF_MAX_TOTAL);
      of_lv[l][i] = (int16_t)fx_clamp(tv, -OF_MAX_TOTAL, OF_MAX_TOTAL);
      of_lok[l][i] = 1;
    }
  }
}

// Full flow pyramid from prev (level 0 = prev) to cur (level 0 = cur), coarse
// level first. Result: level 0 in of_lu[0], of_lv[0], of_lok[0]
static void of_flow(const uint8_t *prev, const uint8_t *cur, int w, int h) {
  const uint8_t *pp[OF_LEVELS] = {prev, of_p1, of_p2};
  const uint8_t *pc[OF_LEVELS] = {cur, of_c1, of_c2};
  uint8_t *w1p = of_p1;
  uint8_t *w1c = of_c1;
  uint8_t *w2p = of_p2;
  uint8_t *w2c = of_c2;

  for (int l = 0; l < OF_LEVELS; l++) {
    of_lw[l] = of_ceil_shift(w, l);
    of_lh[l] = of_ceil_shift(h, l);
    of_lcw[l] = (of_lw[l] + OF_CELL - 1) / OF_CELL;
    of_lch[l] = (of_lh[l] + OF_CELL - 1) / OF_CELL;
  }
  of_halve(prev, w, h, w1p);
  of_halve(cur, w, h, w1c);
  of_halve(w1p, of_lw[1], of_lh[1], w2p);
  of_halve(w1c, of_lw[1], of_lh[1], w2c);

  for (int l = OF_LEVELS - 1; l >= 0; l--) {
    if (l == OF_LEVELS - 1) {
      of_level(pp[l], pc[l], of_lw[l], of_lh[l], l, NULL, NULL, NULL);
    } else {
      of_level(pp[l], pc[l], of_lw[l], of_lh[l], l, of_lu[l + 1], of_lv[l + 1],
               of_lok[l + 1]);
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
  } else {
    of_flow(of_prev, gray, w, h);
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
      if (!of_lok[0][i]) {
        continue;
      }

      int fu = of_lu[0][i];
      int fv = of_lv[0][i];
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
