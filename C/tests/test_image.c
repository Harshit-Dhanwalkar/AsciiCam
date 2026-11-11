/*
 * tests/test_image.c - src/image.c: PNG (every colour type, bit depth and
 * filter, stored / fixed / dynamic deflate blocks), PGM/PPM, bad input, scaler
 * Vectors are written by tests/gen_image_vectors.py
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_image.c src/image.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c lib/nl_mem.c lib/nl_gc.c \
 *     -Iinclude -Ilib -o /tmp/ti
 */

#include "platform.h"

#include "../lib/nolibc.h"
#include "image.h"

static int pass_n, fail_n;
#define CHECK(c, fmt, ...)                                                     \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: " fmt "\n", __LINE__, ##__VA_ARGS__);     \
    }                                                                          \
  } while (0)

typedef struct {
  const char *name;
  const uint8_t *png;
  size_t len;
  int w, h;
  const uint8_t *rgb; // NULL: big picture, see bigpx
} vec_t;

#include "image_vectors.inc"

#define NVEC ((int)(sizeof(vectors) / sizeof(vectors[0])))
// #define BW 48
// #define BH 32

static void bigpx(int x, int y, uint8_t *o) {
  o[0] = (uint8_t)((x * 5 + y) % 256);
  o[1] = (uint8_t)((y * 7 + x * x / 8) % 256);
  o[2] = (uint8_t)((x * y / 3) % 256);
}

static uint32_t rng_state = 0x2545f491;
static uint32_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;

  return rng_state;
}

static int matches(const vec_t *v, const image_t *img) {
  if (img->w != v->w || img->h != v->h) {
    return 0;
  }

  for (int y = 0; y < v->h; y++) {
    for (int x = 0; x < v->w; x++) {
      uint8_t want[3];
      if (v->rgb) {
        for (int k = 0; k < 3; k++) {
          want[k] = v->rgb[(y * v->w + x) * 3 + k];
        }
      } else {
        bigpx(x, y, want);
      }

      const uint8_t *got = img->rgb + (y * v->w + x) * 3;
      if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2]) {
        fprintf(stderr, "  %s: pixel %d,%d is %d,%d,%d, want %d,%d,%d\n",
                v->name, x, y, got[0], got[1], got[2], want[0], want[1],
                want[2]);

        return 0;
      }
    }
  }

  return 1;
}

static int luma_at(const uint8_t *yuyv, int stride, int x, int y) {
  return yuyv[y * stride + x * 2];
}

int main(void) {
  // Written PNGs decode to exactly RGB generator computed
  for (int i = 0; i < NVEC; i++) {
    image_t img;
    const char *err = NULL;
    int rc = image_decode(vectors[i].png, vectors[i].len, &img, &err);
    CHECK(rc == 0 && matches(&vectors[i], &img), "%s decodes (%s)",
          vectors[i].name, err ? err : "ok");
    if (rc == 0) {
      image_free(&img);
      CHECK(img.rgb == NULL && img.w == 0, "image_free clears");
    }
  }

  // Damage: cut at every length, and flip random bytes
  // Never a crash, and a cut that loses image data is an error
  const vec_t *big = &vectors[NVEC - 1];
  int cut_ok = 1;
  // (Last 12 bytes are IEND, a file without it still has its picture)
  for (size_t cut = 0; cut < big->len - 12; cut += 7) {
    image_t img;
    const char *err = NULL;
    int rc = image_decode(big->png, cut, &img, &err);
    if (rc == 0) {
      image_free(&img);
      cut_ok = 0;
    } else if (!err || !err[0]) {
      cut_ok = 0;
    }
  }

  CHECK(cut_ok, "every truncation of a PNG is refused with a message");

  static uint8_t work[4096];
  int survived = 1, bad_accepted = 0;
  for (int round = 0; round < 6000; round++) {
    const vec_t *v = &vectors[rnd() % (uint32_t)NVEC];
    size_t n = v->len < sizeof(work) ? v->len : sizeof(work);
    nl_memcpy(work, v->png, n);
    int flips = 1 + (int)(rnd() % 4);
    for (int f = 0; f < flips; f++) {
      work[rnd() % n] = (uint8_t)rnd();
    }

    image_t img;
    if (image_decode(work, n, &img, NULL) == 0) {
      if (img.w <= 0 || img.h <= 0 || !img.rgb) {
        survived = 0;
      }

      image_free(&img);
    } else {
      bad_accepted++;
    }
  }

  CHECK(survived, "6000 damaged PNGs: no crash, results sane");
  CHECK(bad_accepted > 1000, "and most damage is noticed (%d refused)",
        bad_accepted);

  // Headers
  {
    image_t img;
    const char *err = NULL;
    uint8_t il[sizeof(png_rgb8)];
    nl_memcpy(il, png_rgb8, sizeof(il));
    il[8 + 8 + 12] = 1; // IHDR: signature, length + type, interlace is byte 12
    CHECK(image_decode(il, sizeof(il), &img, &err) == -1 && err &&
              err[0] == 'i',
          "interlaced PNG: says so (%s)", err ? err : "?");
    uint8_t huge[sizeof(png_rgb8)];
    nl_memcpy(huge, png_rgb8, sizeof(huge));
    huge[8 + 8 + 1] = 0x7f; // width ~ 8 million
    err = NULL;
    CHECK(image_decode(huge, sizeof(huge), &img, &err) == -1 && err,
          "absurd dimensions refused: %s", err ? err : "?");
    CHECK(image_decode((const uint8_t *)"\xff\xd8\xff\xe0 jpeg", 9, &img,
                       &err) == -1 &&
              err && err[0] == 'd',
          "truncated JPEG: damaged, no crash");
    CHECK(image_decode((const uint8_t *)"GIF89a", 6, &img, &err) == -1 && err,
          "other formats refused");
    CHECK(image_decode((const uint8_t *)"", 0, &img, &err) == -1 && err,
          "empty input refused");
    CHECK(image_decode((const uint8_t *)"", 0, &img, NULL) == -1,
          "no message buffer is fine");
  }

  // PGM / PPM
  {
    image_t img;
    static const uint8_t p6[] =
        "P6\n# made by hand\n2 2\n255\n"
        "\xff\x00\x00\x00\xff\x00\x00\x00\xff\x10\x20\x30";
    CHECK(image_decode(p6, sizeof(p6) - 1, &img, NULL) == 0 && img.w == 2 &&
              img.h == 2 && img.rgb[0] == 255 && img.rgb[4] == 255 &&
              img.rgb[8] == 255 && img.rgb[9] == 0x10 && img.rgb[11] == 0x30,
          "P6 with a comment");
    image_free(&img);
    static const uint8_t p5[] = "P5 3 1 255\n\x00\x80\xff";
    CHECK(image_decode(p5, sizeof(p5) - 1, &img, NULL) == 0 && img.w == 3 &&
              img.rgb[3] == 0x80 && img.rgb[4] == 0x80 && img.rgb[8] == 255,
          "P5 gray becomes r = g = b");
    image_free(&img);
    static const uint8_t p5b[] = "P5\n2 1\n65535\n\xff\xff\x80\x00";
    CHECK(image_decode(p5b, sizeof(p5b) - 1, &img, NULL) == 0 &&
              img.rgb[0] == 255 && img.rgb[3] == 128,
          "16 bit PGM");
    image_free(&img);
    static const uint8_t p5c[] = "P5\n2 1\n100\n\x32\x64";
    CHECK(image_decode(p5c, sizeof(p5c) - 1, &img, NULL) == 0 &&
              img.rgb[0] == 128 && img.rgb[3] == 255,
          "maxval 100 is scaled to 255 (%d)", img.rgb[0]);
    image_free(&img);
    static const char *const bad[] = {"P6\n2 2\n255\n\x01\x02",
                                      "P6\n0 2\n255\n",
                                      "P6\n2 2\n0\n",
                                      "P6\n2 2\n70000\n",
                                      "P6\n2\n",
                                      "P6\n99999 99999\n255\n",
                                      "P7\n2 2\n255\n",
                                      "P5"};
    int refused = 1;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      if (image_decode((const uint8_t *)bad[i], nl_strlen(bad[i]), &img,
                       NULL) == 0) {
        refused = 0;
        image_free(&img);
      }
    }

    CHECK(refused, "bad PNM headers and short data refused");
  }

  // Inflate itself
  {
    uint8_t out[64];
    static const uint8_t stored[] = {0x78, 0x01, 0x01, 0x03, 0x00, 0xfc, 0xff,
                                     'a',  'b',  'c',  0,    0,    0,    0};
    CHECK(image_inflate(stored, sizeof(stored), out, sizeof(out)) == 3 &&
              out[0] == 'a' && out[2] == 'c',
          "stored block");
    CHECK(image_inflate(stored, sizeof(stored), out, 2) == -1,
          "output buffer too small");
    static const uint8_t badlen[] = {0x78, 0x01, 0x01, 0x03, 0x00,
                                     0xfd, 0xff, 'a',  'b',  'c'};
    CHECK(image_inflate(badlen, sizeof(badlen), out, sizeof(out)) == -1,
          "stored block with a wrong complement");
    static const uint8_t notz[] = {0x00, 0x00, 0x01};
    CHECK(image_inflate(notz, sizeof(notz), out, sizeof(out)) == -1,
          "not a zlib header");
    static const uint8_t reserved[] = {0x78, 0x01, 0x07};
    CHECK(image_inflate(reserved, sizeof(reserved), out, sizeof(out)) == -1,
          "reserved block type");
    CHECK(image_inflate(stored, 0, out, sizeof(out)) == -1, "nothing");
    // A zip bomb shape: long run into a small buffer must stop at end
    static const uint8_t runs_z[] = {0x78, 0xda, 0x63, 0x60, 0x18, 0x05, 0xc4,
                                     0x02, 0x00, 0x01, 0x2c, 0x00, 0x01};
    uint8_t big[300];
    CHECK(image_inflate(runs_z, sizeof(runs_z), big, sizeof(big)) == 300 &&
              big[0] == 0 && big[299] == 0,
          "300 zero bytes in 13");
    CHECK(image_inflate(runs_z, sizeof(runs_z), out, 8) == -1,
          "a run longer than the buffer is refused");
    uint8_t junk[200];
    int safe = 1;
    for (int round = 0; round < 20000; round++) {
      size_t n = rnd() % sizeof(junk);
      for (size_t i = 0; i < n; i++) {
        junk[i] = (uint8_t)rnd();
      }

      junk[0] = 0x78;
      junk[1] = 0x01;
      long got = image_inflate(junk, n, out, sizeof(out));
      if (got > (long)sizeof(out)) {
        safe = 0;
      }
    }

    CHECK(safe, "20000 random streams: never writes past the buffer");
  }

  // Device string
  CHECK(image_device_path("image:/tmp/a.png") &&
            image_device_path("image:/tmp/a.png")[0] == '/',
        "image: device");
  CHECK(!image_device_path("image:") && !image_device_path("demo") &&
            !image_device_path("/dev/video0") && !image_device_path(NULL) &&
            !image_device_path("imag"),
        "other devices");

  // File loading
  {
    image_t img;
    const char *err = NULL;
    CHECK(image_load("/nonexistent/x.png", &img, &err) == -1 && err,
          "missing file: %s", err ? err : "?");
    CHECK(image_load("/dev/null", &img, &err) == -1 && err, "empty file");
  }

  // Scaler: a white picture is white (Y 235) and neutral (U = V = 128)
  {
    uint8_t px[2 * 2 * 3];
    for (int i = 0; i < 12; i++) {
      px[i] = 255;
    }

    image_t white = {2, 2, px};
    uint8_t yuyv[16 * 8 * 2];
    image_fill_yuyv(&white, yuyv, 16, 8, 32);
    CHECK(luma_at(yuyv, 32, 8, 4) == 235 && luma_at(yuyv, 32, 4, 0) == 235 &&
              luma_at(yuyv, 32, 11, 7) == 235 &&
              luma_at(yuyv, 32, 3, 4) == 16 && luma_at(yuyv, 32, 12, 4) == 16 &&
              yuyv[4 * 32 + 8 * 2 + 1] == 128,
          "white square on a 16x8 frame: 8x8 white in the middle, black bars, "
          "neutral chroma");
  }

  // Aspect ratio kept: a wide white picture leaves black bars top and bottom
  {
    uint8_t px[8 * 2 * 3];
    for (int i = 0; i < 48; i++) {
      px[i] = 255;
    }

    image_t wide = {8, 2, px};
    uint8_t yuyv[16 * 16 * 2];
    image_fill_yuyv(&wide, yuyv, 16, 16, 32);
    CHECK(luma_at(yuyv, 32, 8, 0) == 16 && luma_at(yuyv, 32, 8, 15) == 16 &&
              luma_at(yuyv, 32, 8, 8) == 235 &&
              luma_at(yuyv, 32, 0, 8) == 235 && luma_at(yuyv, 32, 15, 8) == 235,
          "wide picture: black bars above and below, white across the middle");
    uint8_t tall_px[2 * 8 * 3];
    for (int i = 0; i < 48; i++) {
      tall_px[i] = 255;
    }

    image_t tall = {2, 8, tall_px};
    image_fill_yuyv(&tall, yuyv, 16, 16, 32);
    CHECK(luma_at(yuyv, 32, 0, 8) == 16 && luma_at(yuyv, 32, 15, 8) == 16 &&
              luma_at(yuyv, 32, 8, 8) == 235,
          "tall picture: bars at the sides");
    CHECK(luma_at(yuyv, 32, 3, 8) == 16 && luma_at(yuyv, 32, 12, 8) == 16,
          "bars as wide as they should be (4 cells each side)");
  }

  // Averaging: a black and white checkerboard shrunk to one pixel is mid grey
  {
    uint8_t px[4 * 4 * 3];
    for (int y = 0; y < 4; y++) {
      for (int x = 0; x < 4; x++) {
        for (int k = 0; k < 3; k++) {
          px[(y * 4 + x) * 3 + k] = (uint8_t)(((x + y) & 1) ? 255 : 0);
        }
      }
    }

    image_t chk = {4, 4, px};
    uint8_t yuyv[2 * 2 * 2];
    image_fill_yuyv(&chk, yuyv, 2, 2, 4);
    int l = luma_at(yuyv, 4, 0, 0);
    CHECK(l > 115 && l < 135, "4x4 checkerboard on 2x2: grey (%d)", l);
  }

  // Colours: pure red keeps its chroma
  {
    uint8_t px[2 * 2 * 3] = {255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0};
    image_t red = {2, 2, px};
    uint8_t yuyv[2 * 2 * 2];
    image_fill_yuyv(&red, yuyv, 2, 2, 4);
    CHECK(yuyv[0] == 82 && yuyv[1] == 90 && yuyv[3] == 240,
          "red: Y 82, U 90, V 240 (%d %d %d)", yuyv[0], yuyv[1], yuyv[3]);
  }

  // No picture: black frame, no crash
  {
    uint8_t yuyv[4 * 2 * 2];
    image_fill_yuyv(NULL, yuyv, 4, 2, 8);
    CHECK(yuyv[0] == 16 && yuyv[1] == 128, "no picture: black");
  }

  // A big source onto a small frame and reverse never read out of bounds
  {
    uint8_t *px = nl_malloc(1000 * 700 * 3);
    for (int i = 0; i < 1000 * 700 * 3; i++) {
      px[i] = (uint8_t)(i * 7);
    }

    image_t big_img = {1000, 700, px};
    uint8_t yuyv[6 * 4 * 2];
    image_fill_yuyv(&big_img, yuyv, 6, 4, 12);
    uint8_t one[1 * 1 * 3] = {9, 9, 9};
    image_t tiny = {1, 1, one};
    uint8_t big_frame[640 * 480 * 2];
    image_fill_yuyv(&tiny, big_frame, 640, 480, 1280);
    CHECK(luma_at(big_frame, 1280, 320, 240) > 16, "1x1 picture upscaled");
    int odd_ok = 1;
    for (int w = 2; w <= 40; w += 2) {
      for (int h = 1; h <= 30; h++) {
        uint8_t small[40 * 30 * 2];
        image_fill_yuyv(&big_img, small, w, h, w * 2);
        image_fill_yuyv(&tiny, small, w, h, w * 2);
      }
    }

    CHECK(odd_ok,
          "every frame size 2..40 x 1..30 with a big and a tiny source");
    nl_free(px);
  }

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
