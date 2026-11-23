/*
 * tests/test_downscale.c - area-average downscale inside grayscale_to_ascii
 * (nl_sumspan_u8 / nl_addrow_u8 in assembly build)
 *
 * Grayscale: opts.tone_hist counts cells per brightness, so histogram of what
 * grayscale_to_ascii made of a picture must equal histogram of cells worked out
 * here pixel by pixel, for sizes that divide evenly, do not, are narrower than
 * grid, and wide enough for 16-pixel steps. Colour: a flat picture must come
 * out as exactly that colour in every cell (a wrong sum or a wrong pixel count
 * changes average)
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_downscale.c src/ascii.c lib/nl_gc.c \
 *     lib/nl_mem.c lib/nl_printf.c lib/nl_errno.c lib/nl_alloc.c \
 *     lib/nl_start.c -Iinclude -Ilib -o /tmp/td
 */
#include "../lib/nolibc.h"

#include "ascii.h"

static int pass_n, fail_n;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static uint32_t st = 0xBADC0DEu;
static uint32_t rnd(void) {
  st ^= st << 13;
  st ^= st >> 17;
  st ^= st << 5;

  return st;
}

enum { MAXW = 1920, MAXH = 1080 };
static uint8_t gray[MAXW * MAXH];
static uint8_t rgb[MAXW * MAXH * 3];
static char out[2 * 1024 * 1024];

// Same cell edges as grayscale_to_ascii, one pixel at a time
static void reference_hist(int sw, int sh, int dw, int dh, uint32_t hist[256]) {
  for (int i = 0; i < 256; i++) {
    hist[i] = 0;
  }

  double bw = (double)sw / dw, bh = (double)sh / dh;
  for (int y = 0; y < dh; y++) {
    int ys = (int)(y * bh), ye = (int)((y + 1) * bh);
    if (ye <= ys) {
      ye = ys + 1;
    }

    for (int x = 0; x < dw; x++) {
      int xs = (int)(x * bw), xe = (int)((x + 1) * bw);
      if (xe <= xs) {
        xe = xs + 1;
      }

      long sum = 0;
      int n = 0;
      for (int sy = ys; sy < ye && sy < sh; sy++) {
        for (int sx = xs; sx < xe && sx < sw; sx++) {
          sum += gray[sy * sw + sx];
          n++;
        }
      }

      hist[sum / (n ? n : 1)]++;
    }
  }
}

static void run_hist(int sw, int sh, int dw, int dh) {
  uint32_t want[256], got[256];
  for (int i = 0; i < sw * sh; i++) {
    gray[i] = (uint8_t)(rnd() >> 11);
  }

  for (int i = 0; i < 256; i++) {
    got[i] = 0;
  }

  ascii_opts_t o;
  nl_memset(&o, 0, sizeof(o));
  o.contrast = 100;
  o.gamma = 100;
  o.render_mode = RENDER_ASCII_RAMP;
  o.tone_hist = got;
  int r = grayscale_to_ascii(gray, NULL, sw, sh, dw, dh, out, sizeof(out), &o);
  reference_hist(sw, sh, dw - dw % 2, dh - dh % 4, want);
  int bad = 0;
  for (int i = 0; i < 256; i++) {
    bad += want[i] != got[i];
  }

  CHECK(r >= 0 && bad == 0, "%dx%d -> %dx%d: ret %d, %d brightness bins differ",
        sw, sh, dw, dh, r, bad);
}

// Every \033[38;2;R;G;Bm in out must be flat colour
static void run_flat_colour(int sw, int sh, int dw, int dh, int r, int g,
                            int b) {
  for (int i = 0; i < sw * sh; i++) {
    gray[i] = (uint8_t)((r * 299 + g * 587 + b * 114) / 1000);
    rgb[i * 3] = (uint8_t)r;
    rgb[i * 3 + 1] = (uint8_t)g;
    rgb[i * 3 + 2] = (uint8_t)b;
  }

  ascii_opts_t o;
  nl_memset(&o, 0, sizeof(o));
  o.contrast = 100;
  o.gamma = 100;
  o.color = 1;
  o.color_mode = COLOR_TRUECOLOR;
  o.render_mode = RENDER_BLOCKS;
  int ret = grayscale_to_ascii(gray, rgb, sw, sh, dw, dh, out, sizeof(out), &o);
  char want[32];
  int wl = nl_snprintf(want, sizeof(want), "\033[38;2;%d;%d;%dm", r, g, b);
  int found = 0;
  int other = 0;
  for (const char *p = out; *p; p++) {
    if (p[0] == '\033' && p[1] == '[' && p[2] == '3' && p[3] == '8' &&
        p[4] == ';' && p[5] == '2') {
      int same = 1;
      for (int k = 0; k < wl; k++) {
        same &= p[k] == want[k];
      }

      if (same) {
        found++;
      } else {
        other++;
      }
    }
  }

  CHECK(ret >= 0 && found > 0 && other == 0,
        "flat %d,%d,%d at %dx%d -> %dx%d: ret %d, %d right, %d other colours",
        r, g, b, sw, sh, dw, dh, ret, found, other);
}

int main(void) {
  static const int sz[][4] = {
      {640, 480, 80, 40},   {641, 479, 80, 40},  {1920, 1080, 160, 60},
      {1279, 721, 120, 52}, {333, 77, 80, 40},   {81, 41, 80, 40},
      {80, 40, 80, 40},     {37, 13, 40, 12},    {1000, 3, 64, 4},
      {16, 16, 80, 40},     {1920, 1080, 20, 8}, {4, 4, 10, 8},
  };
  for (unsigned i = 0; i < sizeof(sz) / sizeof(sz[0]); i++) {
    run_hist(sz[i][0], sz[i][1], sz[i][2], sz[i][3]);
  }

  run_flat_colour(640, 480, 80, 40, 200, 100, 50);
  run_flat_colour(641, 479, 80, 40, 255, 255, 255);
  run_flat_colour(1920, 1080, 160, 60, 3, 250, 128);
  run_flat_colour(333, 77, 80, 40, 17, 34, 51);

  fprintf(stderr, "%d passed %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
