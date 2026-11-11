/*
 * tests/test_jpeg.c - src/jpeg.c: the IDCT against a float reference, files
 * written by libjpeg (4:4:4, 4:2:2, 4:2:0, gray, restart intervals, optimised
 * Huffman tables, odd sizes), refused kinds, and damaged input. Vectors are
 * written by tests/gen_jpeg_vectors.py
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_jpeg.c src/jpeg.c src/image.c \
 *     lib/nl_printf.c lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c \
 *     lib/nl_mem.c lib/nl_gc.c -Iinclude -Ilib -o /tmp/tj
 */

#include "platform.h"

#include "../lib/nolibc.h"
#include "image.h"

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

typedef struct {
  const char *name;
  const uint8_t *data;
  size_t len;
  int w, h;
  const uint8_t *rgb;
  int maxerr;     // largest allowed difference to libjpeg, any channel
  int meanerr100; // mean difference allowed, x100
} jvec_t;

typedef struct {
  const char *name;
  const uint8_t *data;
  size_t len;
} jbad_t;

#include "jpeg_vectors.inc"

#define NJ ((int)(sizeof(jvectors) / sizeof(jvectors[0])))
#define NB ((int)(sizeof(jbads) / sizeof(jbads[0])))

static uint32_t rng_state = 0x1234567;
static uint32_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;

  return rng_state;
}

// float reference of the 8x8 inverse DCT (no libm: cos table from a recurrence
// is overkill, so the 64 basis values are built from a Taylor series)
static double cosv(double x) {
  const double two_pi = 6.28318530717958647692;
  x -= two_pi * (double)(long)(x / two_pi); // the series needs a small angle
  if (x > 3.14159265358979323846) {
    x -= two_pi;
  }

  double t = 1, s = 1;
  for (int i = 1; i < 14; i++) {
    t *= -x * x / ((2.0 * i - 1) * (2.0 * i));
    s += t;
  }

  return s;
}

static void ref_idct(const int *coef, double *out) {
  const double pi = 3.14159265358979323846;
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      double sum = 0;
      for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
          double cu = u == 0 ? 0.70710678118654752 : 1.0;
          double cv = v == 0 ? 0.70710678118654752 : 1.0;
          sum += cu * cv * coef[v * 8 + u] * cosv((2 * x + 1) * u * pi / 16) *
                 cosv((2 * y + 1) * v * pi / 16);
        }
      }

      out[y * 8 + x] = sum / 4.0 + 128.0;
    }
  }
}

int main(void) {
  // IDCT: random blocks against the float transform, within 1 level
  {
    int worst = 0;
    for (int round = 0; round < 300; round++) {
      int coef[64];
      for (int i = 0; i < 64; i++) {
        int mag = i == 0 ? 1000 : (i < 10 ? 300 : 60);
        coef[i] = (rnd() % 3 == 0) ? (int)(rnd() % (2u * mag)) - mag : 0;
      }

      uint8_t got[8 * 8];
      double want[64];
      jpeg_idct(coef, got, 8);
      ref_idct(coef, want);
      for (int i = 0; i < 64; i++) {
        double w = want[i] < 0 ? 0 : want[i] > 255 ? 255 : want[i];
        int e = (int)(got[i] - w + (got[i] > w ? 0.5 : -0.5));
        e = e < 0 ? -e : e;
        worst = e > worst ? e : worst;
      }
    }

    CHECK(worst <= 1, "IDCT within 1 of the float transform (worst %d)", worst);
    int flat[64] = {0};
    flat[0] = 8 * 40; // DC only: every sample 128 + 40
    uint8_t got[64];
    jpeg_idct(flat, got, 8);
    int same = 1;
    for (int i = 0; i < 64; i++) {
      same = same && got[i] == 168;
    }

    CHECK(same, "DC only block is flat (%d)", got[0]);
    int zero[64] = {0};
    jpeg_idct(zero, got, 8);
    CHECK(got[0] == 128 && got[63] == 128, "all zero block is mid grey");
    flat[0] = 8 * 1000;
    jpeg_idct(flat, got, 8);
    CHECK(got[10] == 255, "clamped high");
    flat[0] = -8 * 1000;
    jpeg_idct(flat, got, 8);
    CHECK(got[10] == 0, "clamped low");
  }

  // Files libjpeg wrote
  for (int i = 0; i < NJ; i++) {
    const jvec_t *v = &jvectors[i];
    image_t img;
    const char *err = "ok";
    int rc = image_decode(v->data, v->len, &img, &err);
    CHECK(rc == 0 && img.w == v->w && img.h == v->h, "%s decodes (%s)", v->name,
          err);
    if (rc != 0) {
      continue;
    }

    long sum = 0;
    int worst = 0;
    for (int k = 0; k < v->w * v->h * 3; k++) {
      int e = img.rgb[k] - v->rgb[k];
      e = e < 0 ? -e : e;
      sum += e;
      worst = e > worst ? e : worst;
    }

    int mean100 = (int)(sum * 100 / (v->w * v->h * 3));
    CHECK(
        worst <= v->maxerr && mean100 <= v->meanerr100,
        "%s matches libjpeg: worst %d (limit %d), mean %d.%02d (limit %d.%02d)",
        v->name, worst, v->maxerr, mean100 / 100, mean100 % 100,
        v->meanerr100 / 100, v->meanerr100 % 100);
    image_free(&img);
  }

  // kinds that are refused say so
  {
    image_t img;
    const char *err = NULL;
    CHECK(image_decode(jbads[0].data, jbads[0].len, &img, &err) == -1 && err &&
              err[0] == 'p',
          "progressive JPEG: %s", err ? err : "?");
    err = NULL;
    CHECK(image_decode(jbads[1].data, jbads[1].len, &img, &err) == -1 && err,
          "CMYK JPEG refused: %s", err ? err : "?");
  }

  // damage: every truncation, then random byte flips. Never a crash
  {
    const jvec_t *v = &jvectors[0];
    int refused = 1;
    for (size_t cut = 3; cut < v->len - 2; cut += 5) {
      image_t img;
      const char *err = NULL;
      if (image_decode(v->data, cut, &img, &err) == 0) {
        // a cut that keeps every scan byte can still be a picture
        image_free(&img);
      } else if (!err || !err[0]) {
        refused = 0;
      }
    }

    CHECK(refused, "truncated files fail with a message, never silently");
    static uint8_t work[4096];
    int sane = 1, noticed = 0;

    for (int round = 0; round < 6000; round++) {
      const jvec_t *w = &jvectors[rnd() % (uint32_t)NJ];
      size_t n = w->len < sizeof(work) ? w->len : sizeof(work);
      nl_memcpy(work, w->data, n);
      int flips = 1 + (int)(rnd() % 4);
      for (int f = 0; f < flips; f++) {
        work[2 + rnd() % (n - 2)] = (uint8_t)rnd();
      }

      image_t img;
      if (image_decode(work, n, &img, NULL) == 0) {
        if (img.w <= 0 || img.h <= 0 || !img.rgb) {
          sane = 0;
        }

        image_free(&img);
      } else {
        noticed++;
      }
    }

    CHECK(sane, "6000 damaged JPEGs: no crash, results sane");
    CHECK(noticed > 500, "and a good part of the damage is noticed (%d)",
          noticed);
  }

  // header only garbage
  {
    image_t img;
    static const uint8_t soi[] = {0xff, 0xd8, 0xff};
    CHECK(image_decode(soi, sizeof(soi), &img, NULL) == -1, "just a header");
    static const uint8_t bad_len[] = {0xff, 0xd8, 0xff, 0xdb, 0xff, 0xff, 0, 0};
    CHECK(image_decode(bad_len, sizeof(bad_len), &img, NULL) == -1,
          "segment longer than the file");
  }

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
