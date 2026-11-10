/*
 * tests/test_curve.c - src/curve.c: the LUT built from control points (exact
 * at the points, no overshoot, monotone where the points are), presets, point
 * edits, "x,y x,y" parsing with its errors, format round trip, junk input
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_curve.c src/curve.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c lib/nl_mem.c -Iinclude \
 *     -Ilib -o /tmp/tc
 */

#include "platform.h"

#include "../lib/nolibc.h"
#include "curve.h"

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

static int monotone_up(const curve_t *c) {
  for (int i = 1; i < 256; i++) {
    if (c->lut[i] < c->lut[i - 1]) {
      return 0;
    }
  }

  return 1;
}

static int through_points(const curve_t *c) {
  for (int k = 0; k < c->n; k++) {
    if (c->lut[c->p[k].x] != c->p[k].y) {
      return 0;
    }
  }

  return 1;
}

static int same_points(const curve_t *a, const curve_t *b) {
  if (a->n != b->n) {
    return 0;
  }

  for (int k = 0; k < a->n; k++) {
    if (a->p[k].x != b->p[k].x || a->p[k].y != b->p[k].y) {
      return 0;
    }
  }

  return 1;
}

static int same_lut(const curve_t *a, const curve_t *b) {
  for (int i = 0; i < 256; i++) {
    if (a->lut[i] != b->lut[i]) {
      return 0;
    }
  }

  return 1;
}

static uint32_t rng = 0x2545f491;
static uint32_t rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;

  return rng;
}

int main(void) {
  curve_t c;
  curve_reset(&c);
  int linear_ok = 1;
  for (int i = 0; i < 256; i++) {
    linear_ok &= c.lut[i] == i;
  }

  CHECK(linear_ok && c.identity && c.enabled, "reset is the identity");
  CHECK(!curve_active(&c), "identity is not applied");
  c.enabled = 0;
  CHECK(!curve_active(&c), "disabled is not applied");

  // Presets
  CHECK(curve_preset_count() == 7, "7 presets (%d)", curve_preset_count());
  CHECK(nl_strcmp(curve_preset_name(0), "linear") == 0 &&
            nl_strcmp(curve_preset_name(99), "custom") == 0,
        "preset names");
  for (int i = 0; i < curve_preset_count(); i++) {
    curve_reset(&c);
    curve_set_preset(&c, i);
    CHECK(c.preset == i && through_points(&c), "preset %s passes its points",
          curve_preset_name(i));
    CHECK(c.lut[0] == c.p[0].y && c.lut[255] == c.p[c.n - 1].y,
          "preset %s ends", curve_preset_name(i));
    CHECK(c.identity == (i == 0), "preset %s identity flag",
          curve_preset_name(i));
    if (i != 4 && i != 6) {
      CHECK(monotone_up(&c), "preset %s is monotone", curve_preset_name(i));
    }
  }

  curve_set_preset(&c, 4);
  CHECK(c.lut[0] == 255 && c.lut[255] == 0 && c.lut[100] > c.lut[101],
        "invert falls");
  curve_set_preset(&c, 6);
  CHECK(c.lut[128] == 255 && c.lut[127] >= 250 && c.lut[129] >= 250 &&
            c.lut[64] > 100 && c.lut[192] > 100,
        "solarize peaks at 128 and stays flat there (%d %d %d)", c.lut[127],
        c.lut[128], c.lut[129]);
  curve_set_preset(&c, 1);
  CHECK(c.lut[32] < 32 && c.lut[224] > 224 && c.lut[64] == 46,
        "s-curve darkens shadows, lifts highlights (%d %d)", c.lut[32],
        c.lut[224]);
  curve_set_preset(&c, 2);
  CHECK(c.lut[16] == 50 && c.lut[128] == 197, "asinh lifts shadows (%d %d)",
        c.lut[16], c.lut[128]);
  curve_set_preset(&c, 5);
  CHECK(c.lut[100] < 5 && c.lut[150] > 250, "threshold is a step (%d %d)",
        c.lut[100], c.lut[150]);

  // Monotone interpolation never overshoots, even next to a plateau
  curve_reset(&c);
  CHECK(curve_parse(&c, "0,0 10,200 20,210 255,255", NULL, 0) == 0 &&
            monotone_up(&c) && through_points(&c),
        "steep then flat: no dip");
  CHECK(curve_parse(&c, "0,0 100,0 101,255 255,255", NULL, 0) == 0 &&
            monotone_up(&c) && c.lut[50] == 0 && c.lut[200] == 255,
        "plateau - jump - plateau");
  int ok = 1;
  for (int round = 0; round < 500; round++) {
    curve_t r;
    curve_reset(&r);
    int n = 2 + (int)(rnd() % 7);
    int xs[8], ys[8];
    xs[0] = 0;
    xs[n - 1] = 255;
    int prev = 0;
    for (int k = 1; k < n - 1; k++) {
      int room = 255 - prev - (n - 1 - k);
      xs[k] = prev + 1 + (int)(rnd() % (uint32_t)(room > 1 ? room : 1));
      prev = xs[k];
    }

    int y = (int)(rnd() % 40);
    for (int k = 0; k < n; k++) {
      ys[k] = y;
      y += (int)(rnd() % 60);
      if (y > 255) {
        y = 255;
      }
    }

    r.n = n;
    for (int k = 0; k < n; k++) {
      r.p[k].x = (uint8_t)xs[k];
      r.p[k].y = (uint8_t)ys[k];
    }

    curve_build(&r);
    if (!monotone_up(&r) || !through_points(&r)) {
      ok = 0;
    }

    for (int x = 0; x < 256; x++) {
      if (r.lut[x] < r.p[0].y || r.lut[x] > r.p[n - 1].y) {
        ok = 0;
      }
    }
  }

  CHECK(ok, "500 random rising curves: monotone, exact at points, in range");

  // Edits
  curve_reset(&c);
  CHECK(curve_add(&c, 100, 50) == 1 && c.n == 3 && c.p[1].x == 100 &&
            c.preset == -1,
        "add keeps order and marks the curve edited");
  CHECK(curve_add(&c, 100, 10) == -1, "no second point at the same x");
  CHECK(curve_add(&c, 0, 10) == -1 && curve_add(&c, 255, 10) == -1,
        "no point on the end x values");
  CHECK(curve_add(&c, 50, 70) == 1 && c.p[2].x == 100, "insert before");
  while (c.n < CURVE_MAX_POINTS) {
    curve_add(&c, 150 + c.n, 150);
  }

  CHECK(c.n == 8 && curve_add(&c, 30, 30) == -1, "at most 8 points");
  CHECK(curve_remove(&c, 0) == -1 && curve_remove(&c, c.n - 1) == -1 &&
            curve_remove(&c, 99) == -1,
        "end points cannot be removed");
  CHECK(curve_remove(&c, 1) == 0 && c.n == 7, "inner point removed");

  curve_reset(&c);
  curve_add(&c, 100, 100);
  curve_add(&c, 200, 200);
  curve_move(&c, 1, 300, 40);
  CHECK(c.p[1].x == 199 && c.p[1].y == 40, "x stops short of the next point");
  curve_move(&c, 1, -5, 999);
  CHECK(c.p[1].x == 1 && c.p[1].y == 255, "x stops after the previous point");
  curve_move(&c, 0, 77, 128);
  CHECK(c.p[0].x == 0 && c.p[0].y == 128, "end point keeps x, y moves");
  CHECK(curve_move(&c, 9, 1, 1) == -1, "bad index");
  curve_reset(&c);
  curve_add(&c, 100, 100);
  CHECK(curve_nearest(&c, 90) == 1 && curve_nearest(&c, 3) == 0 &&
            curve_nearest(&c, 250) == 2,
        "nearest point by x");

  // Parsing
  curve_reset(&c);
  char err[96];
  CHECK(curve_parse(&c, "0,0 64,40;192,220 255,255", err, sizeof(err)) == 0 &&
            c.n == 4 && c.p[1].x == 64 && c.p[2].y == 220 && c.preset == -1,
        "spaces and ; separate points (%s)", err);
  CHECK(curve_parse(&c, "asinh", NULL, 0) == 0 && c.preset == 2,
        "a preset name");
  curve_t before = c;
  static const char *const bad[] = {
      "",
      "0,0",
      "0,0 255",
      "0,0 100,50",
      "5,0 255,255",
      "0,0 100,50 100,60 255,255",
      "0,0 100,50 50,60 255,255",
      "0,0 300,5 255,255",
      "0,0 a,b",
      "0,0,0 255,255",
      "0,0 255,255x",
      "0,0 10,10 20,20 30,30 40,40 50,50 60,60 70,70 255,255",
      "nonsense"};
  int refused = 1;
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    err[0] = 0;
    int rc = curve_parse(&c, bad[i], err, sizeof(err));
    if (rc == 0 || !err[0] || !same_points(&c, &before)) {
      refused = 0;
      fprintf(stderr, "  not refused cleanly: \"%s\"\n", bad[i]);
    }
  }

  CHECK(refused, "bad specs are refused with a message and change nothing");
  CHECK(curve_parse(&c, "0,0 255,255", NULL, 0) == 0 && c.identity,
        "plain 0,0 255,255 is the identity");

  // Format round trip
  char buf[128];
  curve_set_preset(&c, 3);
  int len = curve_format(&c, buf, sizeof(buf));
  curve_t back;
  curve_reset(&back);
  CHECK(len > 0 && curve_parse(&back, buf, NULL, 0) == 0 &&
            same_points(&back, &c) && same_lut(&back, &c),
        "format -> parse gives the same curve (%s)", buf);
  CHECK(curve_format(&c, buf, 6) <= 5, "format respects a small buffer");

  // Junk never crashes or corrupts
  int sane = 1;
  for (int round = 0; round < 5000; round++) {
    char junk[64];
    size_t n = rnd() % 60;
    for (size_t i = 0; i < n; i++) {
      static const char alpha[] = "0123456789,; x-a";
      junk[i] = alpha[rnd() % (sizeof(alpha) - 1)];
    }

    junk[n] = 0;
    curve_t t;
    curve_reset(&t);
    if (curve_parse(&t, junk, NULL, 0) == 0) {
      if (t.n < 2 || t.n > CURVE_MAX_POINTS || t.p[0].x != 0 ||
          t.p[t.n - 1].x != 255 || !through_points(&t)) {
        sane = 0;
      }
    }
  }

  CHECK(sane, "5000 random specs: accepted ones are valid curves");

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}

