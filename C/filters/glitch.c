#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* glitch - digital corruption bursts: bands of rows slide sideways, repeat or
 * flip
 * Bursts last a few frames, then picture heals
 * Parameter: 0 never glitches .. 255 constantly (128 default) */

#define GLITCH_BANDS 6

typedef struct {
  int y0, y1; // rows [y0, y1)
  int shift;  // px
  int mode;   // 0 slide, 1 repeat first row, 2 invert
} glitch_band_t;

static uint32_t glitch_seed = 0xC0FFEE11u;
static glitch_band_t glitch_bands[GLITCH_BANDS];
static int glitch_nbands;
static int glitch_hold; // frames current burst has left
static uint8_t glitch_row[FX_MAX_SIDE];

static void glitch_start(int w, int h, int p) {
  (void)w;
  glitch_nbands = 1 + fx_rand_n(&glitch_seed, 1 + p / 48);
  if (glitch_nbands > GLITCH_BANDS) {
    glitch_nbands = GLITCH_BANDS;
  }

  for (int i = 0; i < glitch_nbands; i++) {
    glitch_band_t *b = &glitch_bands[i];
    int bh = 1 + fx_rand_n(&glitch_seed, h / 8 + 1);
    b->y0 = fx_rand_n(&glitch_seed, h);
    b->y1 = b->y0 + bh;
    b->shift = fx_rand_n(&glitch_seed, w / 3 + 1) - w / 6;
    b->mode = fx_rand_n(&glitch_seed, 3);
  }

  glitch_hold = 1 + fx_rand_n(&glitch_seed, 4);
}

void glitch_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  if (glitch_hold > 0) {
    glitch_hold--;
  } else if (p > 0 && fx_rand_n(&glitch_seed, 256) < p / 4 + 1) {
    glitch_start(w, h, p);
  } else {
    glitch_nbands = 0;
  }

  if (p == 0 || glitch_hold == 0 || glitch_nbands == 0) {
    return;
  }

  for (int i = 0; i < glitch_nbands; i++) {
    const glitch_band_t *b = &glitch_bands[i];
    int y1 = b->y1 < h ? b->y1 : h;
    for (int y = b->y0; y < y1; y++) {
      uint8_t *row = gray + y * w;
      if (b->mode == 0) {
        nl_memcpy(glitch_row, row, (size_t)w);
        for (int x = 0; x < w; x++) {
          int sx = (x + b->shift) % w;
          row[x] = glitch_row[sx < 0 ? sx + w : sx];
        }
      } else if (b->mode == 1) {
        if (y > b->y0) {
          nl_memcpy(row, gray + b->y0 * w, (size_t)w);
        }
      } else {
        for (int x = 0; x < w; x++) {
          row[x] = (uint8_t)(255 - row[x]);
        }
      }
    }
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = glitch_process, .name = "glitch"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif
