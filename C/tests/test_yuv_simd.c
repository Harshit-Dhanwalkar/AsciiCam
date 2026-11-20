/*
 * tests/test_yuv_simd.c - yuyv_to_rgb_ex / yuyv_to_rgb_mirror (src/ascii.c)
 *
 * Vectorised paths must give exactly bytes of scalar code they replace (kept
 * here as reference):
 *
 *   1. every (Y, U, V) triple, through both functions
 *   2. random pictures: even and odd widths 1..70, heights 1..3, stride equal
 *      to row, padded, or too small, with and without a luma plane
 *   3. nothing is written outside rgb buffer (guard bytes both sides)
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static -msse4.1 tests/test_yuv_simd.c src/ascii.c lib/nl_mem.c
 * \
 *     lib/nl_printf.c lib/nl_errno.c lib/nl_alloc.c lib/nl_gc.c lib/nl_start.c
 * \ -Iinclude -Ilib -o /tmp/ys
 */

#include "../lib/nolibc.h"

#include "ascii.h"

static int fails, total;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (!(c)) {                                                                \
      fails++;                                                                 \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static unsigned long long rng = 0x9E3779B97F4A7C15ULL;
static unsigned rnd(void) {
  rng ^= rng >> 12;
  rng ^= rng << 25;
  rng ^= rng >> 27;

  return (unsigned)((rng * 0x2545F4914F6CDD1DULL) >> 32);
}

static uint8_t clamp(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// Scalar code vector paths replaced, verbatim

static void ref_ex(const uint8_t *yuyv, int stride, const uint8_t *luma,
                   uint8_t *rgb, int width, int height) {
  if (stride < width * 2) {
    stride = width * 2;
  }

  int pairs = width / 2;
  for (int y = 0; y < height; y++) {
    const uint8_t *row = yuyv + (size_t)y * (size_t)stride;
    const uint8_t *lrow = luma ? luma + (size_t)y * (size_t)width : NULL;
    uint8_t *out = rgb + (size_t)y * (size_t)width * 3;
    for (int i = 0; i < pairs; i++) {
      int y0 = lrow ? lrow[i * 2] : row[i * 4 + 0];
      int u = row[i * 4 + 1];
      int y1 = lrow ? lrow[i * 2 + 1] : row[i * 4 + 2];
      int v = row[i * 4 + 3];
      int d = u - 128;
      int e = v - 128;
      for (int p = 0; p < 2; p++) {
        int c = ((p == 0) ? y0 : y1) - 16;
        uint8_t *px = out + (i * 2 + p) * 3;
        px[0] = clamp((298 * c + 409 * e + 128) >> 8);
        px[1] = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
        px[2] = clamp((298 * c + 516 * d + 128) >> 8);
      }
    }
  }
}

static void ref_mirror(const uint8_t *yuyv, int stride, const uint8_t *luma,
                       uint8_t *rgb, int width, int height) {
  if (stride < width * 2) {
    stride = width * 2;
  }

  int pairs = width / 2;
  int last = pairs * 2 - 1;
  for (int y = 0; y < height; y++) {
    const uint8_t *row = yuyv + (size_t)y * (size_t)stride;
    const uint8_t *lrow = luma + (size_t)y * (size_t)width;
    uint8_t *out = rgb + (size_t)y * (size_t)width * 3;
    for (int x = 0; x < pairs * 2; x++) {
      int sx = width - 1 - x;
      if (sx > last) {
        sx = last;
      }

      int u = row[(sx / 2) * 4 + 1];
      int v = row[(sx / 2) * 4 + 3];
      int d = u - 128;
      int e = v - 128;
      int c = lrow[x] - 16;
      uint8_t *px = out + x * 3;
      px[0] = clamp((298 * c + 409 * e + 128) / 256);
      px[1] = clamp((298 * c - 100 * d - 208 * e + 128) / 256);
      px[2] = clamp((298 * c + 516 * d + 128) / 256);
    }
  }
}

// 1. Every Y, U, V

#define EW 256 // 128 pairs: Y0 = 2k, Y1 = 2k+1 covers all 256 luma values

static uint8_t ex_in[EW * 2];
static uint8_t ex_luma[EW];
static uint8_t ex_got[EW * 3], ex_want[EW * 3];

static void exhaustive(void) {
  long bad_ex = 0, bad_ex_luma = 0, bad_mir = 0;
  for (int u = 0; u < 256; u++) {
    for (int v = 0; v < 256; v++) {
      for (int k = 0; k < EW / 2; k++) {
        ex_in[k * 4 + 0] = (uint8_t)(2 * k);
        ex_in[k * 4 + 1] = (uint8_t)u;
        ex_in[k * 4 + 2] = (uint8_t)(2 * k + 1);
        ex_in[k * 4 + 3] = (uint8_t)v;
      }

      // Y from YUYV
      ref_ex(ex_in, EW * 2, NULL, ex_want, EW, 1);
      yuyv_to_rgb_ex(ex_in, EW * 2, NULL, ex_got, EW, 1);
      for (int i = 0; i < EW * 3; i++) {
        bad_ex += ex_got[i] != ex_want[i];
      }

      // Y from a luma plane (reversed, so it is not YUYV's own)
      for (int i = 0; i < EW; i++) {
        ex_luma[i] = (uint8_t)(255 - i);
      }

      ref_ex(ex_in, EW * 2, ex_luma, ex_want, EW, 1);
      yuyv_to_rgb_ex(ex_in, EW * 2, ex_luma, ex_got, EW, 1);
      for (int i = 0; i < EW * 3; i++) {
        bad_ex_luma += ex_got[i] != ex_want[i];
      }

      // mirror: chroma comes from far end, luma is given
      ref_mirror(ex_in, EW * 2, ex_luma, ex_want, EW, 1);
      yuyv_to_rgb_mirror(ex_in, EW * 2, ex_luma, ex_got, EW, 1);
      for (int i = 0; i < EW * 3; i++) {
        bad_mir += ex_got[i] != ex_want[i];
      }
    }
  }

  CHECK(bad_ex == 0, "yuyv_to_rgb_ex: %ld wrong bytes over all Y/U/V", bad_ex);
  CHECK(bad_ex_luma == 0, "yuyv_to_rgb_ex with luma: %ld wrong bytes",
        bad_ex_luma);
  CHECK(bad_mir == 0, "yuyv_to_rgb_mirror: %ld wrong bytes over all Y/U/V",
        bad_mir);
}

// 2 + 3. Random pictures, odd sizes, strides, guards

#define MAXW 70
#define MAXH 3
#define GUARD 64
static uint8_t pic[(MAXW * 2 + 70) * MAXH];
static uint8_t lum[MAXW * MAXH];
static uint8_t out_got[GUARD + MAXW * MAXH * 3 + GUARD];
static uint8_t out_want[MAXW * MAXH * 3];

static void random_pictures(long rounds) {
  long bad = 0;
  int guard_bad = 0;
  for (long r = 0; r < rounds; r++) {
    int w = 1 + (int)(rnd() % MAXW);
    int h = 1 + (int)(rnd() % MAXH);
    static const int pads[] = {0, 0, 1, 3, 16, 64, -5};
    int stride = w * 2 + pads[rnd() % (sizeof(pads) / sizeof(*pads))];
    size_t picbytes = (size_t)(stride > w * 2 ? stride : w * 2) * (size_t)h;
    for (size_t i = 0; i < picbytes + 16 && i < sizeof(pic); i++) {
      unsigned m = rnd();
      pic[i] =
          (m & 7) == 0 ? (uint8_t)((m >> 8) & 1 ? 255 : 0) : (uint8_t)(m >> 8);
    }

    for (int i = 0; i < w * h; i++) {
      unsigned m = rnd();
      lum[i] =
          (m & 7) == 0 ? (uint8_t)((m >> 8) & 1 ? 255 : 0) : (uint8_t)(m >> 8);
    }

    int use_luma = (int)(rnd() & 1);
    for (size_t i = 0; i < sizeof(out_got); i++) {
      out_got[i] = 0xC3;
    }

    size_t n = (size_t)w * (size_t)h * 3;
    uint8_t *got = out_got + GUARD;

    // _ex
    for (size_t i = 0; i < n; i++) {
      out_want[i] = 0xC3;
    }

    ref_ex(pic, stride, use_luma ? lum : NULL, out_want, w, h);
    yuyv_to_rgb_ex(pic, stride, use_luma ? lum : NULL, got, w, h);
    for (size_t i = 0; i < n; i++) {
      bad += got[i] != out_want[i];
    }

    for (int i = 0; i < GUARD; i++) {
      guard_bad += out_got[i] != 0xC3 || got[n + (size_t)i] != 0xC3;
    }

    // mirror (luma is required)
    for (size_t i = 0; i < sizeof(out_got); i++) {
      out_got[i] = 0xC3;
    }

    for (size_t i = 0; i < n; i++) {
      out_want[i] = 0xC3;
    }

    ref_mirror(pic, stride, lum, out_want, w, h);
    yuyv_to_rgb_mirror(pic, stride, lum, got, w, h);
    for (size_t i = 0; i < n; i++) {
      bad += got[i] != out_want[i];
    }

    for (int i = 0; i < GUARD; i++) {
      guard_bad += out_got[i] != 0xC3 || got[n + (size_t)i] != 0xC3;
    }
  }

  CHECK(bad == 0, "%ld wrong bytes over %ld random pictures", bad, rounds);
  CHECK(guard_bad == 0, "%d guard bytes overwritten", guard_bad);
}

// widths around vector block sizes, one by one: 1..70, both functions
static void every_width(void) {
  long bad = 0;
  for (int w = 1; w <= MAXW; w++) {
    for (int h = 1; h <= MAXH; h++) {
      for (size_t i = 0; i < sizeof(pic); i++) {
        pic[i] = (uint8_t)rnd();
      }

      for (size_t i = 0; i < sizeof(lum); i++) {
        lum[i] = (uint8_t)rnd();
      }

      size_t n = (size_t)w * (size_t)h * 3;
      for (int mode = 0; mode < 3; mode++) {
        for (size_t i = 0; i < n; i++) {
          out_want[i] = 0;
          out_got[GUARD + i] = 0;
        }

        if (mode == 0) {
          ref_ex(pic, w * 2, NULL, out_want, w, h);
          yuyv_to_rgb_ex(pic, w * 2, NULL, out_got + GUARD, w, h);
        } else if (mode == 1) {
          ref_ex(pic, w * 2, lum, out_want, w, h);
          yuyv_to_rgb_ex(pic, w * 2, lum, out_got + GUARD, w, h);
        } else {
          ref_mirror(pic, w * 2, lum, out_want, w, h);
          yuyv_to_rgb_mirror(pic, w * 2, lum, out_got + GUARD, w, h);
        }

        for (size_t i = 0; i < n; i++) {
          bad += out_got[GUARD + i] != out_want[i];
        }
      }
    }
  }

  CHECK(bad == 0, "%ld wrong bytes for widths 1..%d", bad, MAXW);
}

int main(int argc, char **argv) {
  long rounds = 200000;
  if (argc > 1) {
    rounds = 0;
    for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++) {
      rounds = rounds * 10 + (*s - '0');
    }
  }

  exhaustive();
  every_width();
  random_pictures(rounds);

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
