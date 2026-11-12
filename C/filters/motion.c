#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* motion - motion detector: frames what moves and shows how much
 *
 * Picture is split in 8x8 pixel cells
 * Each cell's mean brightness is compared with a slowly adapting background;
 * cells that differ are motion
 * Change of whole picture (auto exposure, a lamp switched on) is taken out
 * first, and a cell counts only with a moving neighbour, so sensor noise does
 * not trigger
 * Moving region gets a dashed frame, and a meter at bottom shows how much of
 * picture moves
 * Without motion picture is left alone
 * Something that stops moving fades into background after a second or two
 *
 * Parameter: 0 off .. 255 most sensitive (128 default)
 */

#define MO_CELL 8
#define MO_CELLS_1080P ((1920 / MO_CELL) * (1080 / MO_CELL))

static int mo_bg[MO_CELLS_1080P]; // background mean, 4 fractional bits
static uint8_t mo_hit[MO_CELLS_1080P];
static int mo_w, mo_h;

static int mo_mean(const uint8_t *gray, int w, int cx, int cy) {
  int sum = 0;
  for (int y = 0; y < MO_CELL; y++) {
    const uint8_t *row = gray + (cy * MO_CELL + y) * w + cx * MO_CELL;
    for (int x = 0; x < MO_CELL; x++) {
      sum += row[x];
    }
  }

  return sum / 4; // mean of 64 pixels with 4 fractional bits
}

static void mo_dot(uint8_t *gray, int w, int x, int y) {
  gray[y * w + x] = (uint8_t)(((x + y) / 3) % 2 == 0 ? 255 : 0);
}

void motion_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (p == 0) {
    mo_w = 0; // history starts over when switched on again
    mo_h = 0;

    return;
  }

  int cw = w / MO_CELL;
  int ch = h / MO_CELL;
  if (cw < 2 || ch < 2) {
    return;
  }

  int n = cw * ch;
  if (mo_w != w || mo_h != h) {
    for (int cy = 0; cy < ch; cy++) {
      for (int cx = 0; cx < cw; cx++) {
        mo_bg[cy * cw + cx] = mo_mean(gray, w, cx, cy);
      }
    }

    mo_w = w;
    mo_h = h;

    return;
  }

  // Difference of every cell to background and what whole picture moved
  // (auto exposure, lights): that part is not motion
  static int cur[MO_CELLS_1080P];
  long shift = 0;
  for (int cy = 0; cy < ch; cy++) {
    for (int cx = 0; cx < cw; cx++) {
      int i = cy * cw + cx;
      cur[i] = mo_mean(gray, w, cx, cy);
      shift += cur[i] - mo_bg[i];
    }
  }

  int common = (int)(shift / n);
  int thr = (44 - p * 40 / 255) * 16; // 44 at p=1 .. 4 at p=255, in 1/16
  for (int i = 0; i < n; i++) {
    mo_hit[i] = (uint8_t)(fx_abs(cur[i] - mo_bg[i] - common) > thr);
  }

  // Background follows picture slowly
  for (int i = 0; i < n; i++) {
    mo_bg[i] += (cur[i] - mo_bg[i]) / 32;
  }

  // A cell counts when a neighbour moves too
  int kept = 0;
  int x0 = cw;
  int y0 = ch;
  int x1 = -1;
  int y1 = -1;
  for (int cy = 0; cy < ch; cy++) {
    for (int cx = 0; cx < cw; cx++) {
      int i = cy * cw + cx;
      if (!mo_hit[i]) {
        continue;
      }

      int near = 0;
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          int nx = cx + dx;
          int ny = cy + dy;
          if ((dx || dy) && nx >= 0 && nx < cw && ny >= 0 && ny < ch &&
              mo_hit[ny * cw + nx]) {
            near = 1;
          }
        }
      }

      if (near) {
        kept++;
        x0 = cx < x0 ? cx : x0;
        x1 = cx > x1 ? cx : x1;
        y0 = cy < y0 ? cy : y0;
        y1 = cy > y1 ? cy : y1;
      }
    }
  }

  if (kept == 0) {
    return;
  }

  // Frame round moving cells, two pixels thick
  int bx0 = x0 * MO_CELL;
  int by0 = y0 * MO_CELL;
  int bx1 = (x1 + 1) * MO_CELL - 1;
  int by1 = (y1 + 1) * MO_CELL - 1;
  for (int t = 0; t < 2; t++) {
    for (int x = bx0; x <= bx1; x++) {
      mo_dot(gray, w, x, by0 + t);
      mo_dot(gray, w, x, by1 - t);
    }

    for (int y = by0; y <= by1; y++) {
      mo_dot(gray, w, bx0 + t, y);
      mo_dot(gray, w, bx1 - t, y);
    }
  }

  // Meter: 1% of picture moving fills 8% of it, 12.5% and more fills it all
  int level = kept * 800 / n;
  level = level > 100 ? 100 : level;
  int bar_h = h / 48 < 2 ? 2 : h / 48;
  int fill = w * level / 100;
  for (int y = h - bar_h; y < h; y++) {
    uint8_t *row = gray + y * w;
    for (int x = 0; x < w; x++) {
      row[x] = x < fill ? 255 : (uint8_t)(row[x] / 4);
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = motion_process, .name = "motion"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
