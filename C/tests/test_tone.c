/*
 * tests/test_tone.c - tone curve inside grayscale_to_ascii: a LUT changes
 * characters picked, colour channels follow it, histogram counts cells before
 * curve, and an identity LUT equals no LUT
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static -msse4.1 tests/test_tone.c src/ascii.c lib/nl_gc.c \
 *     lib/nl_printf.c lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c \
 *     lib/nl_mem.c -Iinclude -Ilib -o /tmp/tt
 */

#include "platform.h"

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

#define SW 64
#define SH 32
#define DW 32
#define DH 16

static uint8_t gray[SW * SH];
static uint8_t rgb[SW * SH * 3];
static char out[1 << 17];

static void flat(int v) {
  for (int i = 0; i < SW * SH; i++) {
    gray[i] = (uint8_t)v;
    rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = (uint8_t)v;
  }
}

static int render(const ascii_opts_t *o) {
  return grayscale_to_ascii(gray, rgb, SW, SH, DW, DH, out, sizeof(out), o);
}

static int count_ch(int len, char ch) {
  int n = 0;
  for (int i = 0; i < len; i++) {
    n += out[i] == ch;
  }

  return n;
}

static int has(int len, const char *needle) {
  int nl = (int)nl_strlen(needle);
  for (int i = 0; i + nl <= len; i++) {
    int k = 0;
    while (k < nl && out[i + k] == needle[k]) {
      k++;
    }

    if (k == nl) {
      return 1;
    }
  }

  return 0;
}

static ascii_opts_t base(void) {
  ascii_opts_t o;
  nl_memset(&o, 0, sizeof(o));
  o.contrast = 100;
  o.gamma = 100;
  o.charset = " @";
  o.render_mode = RENDER_ASCII_RAMP;

  return o;
}

int main(void) {
  uint8_t invert[256], ident[256], step[256];
  for (int i = 0; i < 256; i++) {
    invert[i] = (uint8_t)(255 - i);
    ident[i] = (uint8_t)i;
    step[i] = i < 128 ? 0 : 255;
  }

  ascii_opts_t o = base();
  flat(40);
  int len = render(&o);
  int dark_plain = count_ch(len, '@');
  flat(220);
  len = render(&o);
  int bright_plain = count_ch(len, '@');
  int full = bright_plain; // '@' count of a fully bright picture
  CHECK(dark_plain == 0 && full > 0,
        "no curve: dark picture has no '@' (%d), bright has some (%d)",
        dark_plain, bright_plain);

  o.tone_lut = invert;
  flat(40);
  len = render(&o);
  int dark_inv = count_ch(len, '@');
  flat(220);
  len = render(&o);
  int bright_inv = count_ch(len, '@');
  CHECK(dark_inv == full && bright_inv == 0, "invert curve swaps them (%d, %d)",
        dark_inv, bright_inv);

  o.tone_lut = step;
  flat(100);
  len = render(&o);
  int a = count_ch(len, '@');
  flat(150);
  len = render(&o);
  int b = count_ch(len, '@');
  CHECK(a == 0 && b == full, "step curve at 128 (%d, %d)", a, b);

  // Identity table == no table, byte for byte, for a busy picture
  for (int y = 0; y < SH; y++) {
    for (int x = 0; x < SW; x++) {
      int v = (x * 4 + y * 3) & 255;
      gray[y * SW + x] = (uint8_t)v;
      rgb[(y * SW + x) * 3] = (uint8_t)v;
      rgb[(y * SW + x) * 3 + 1] = (uint8_t)(255 - v);
      rgb[(y * SW + x) * 3 + 2] = (uint8_t)(v / 2);
    }
  }

  for (int color = 0; color < 2; color++) {
    for (int mode = 0; mode < 5; mode++) {
      ascii_opts_t p = base();
      p.charset = NULL;
      p.render_mode = (render_mode_t)mode;
      p.color = color;
      p.gamma = 150;
      len = render(&p);
      static char ref[1 << 17];
      nl_memcpy(ref, out, (size_t)len);
      p.tone_lut = ident;
      int len2 = render(&p);
      int same = len == len2;
      for (int i = 0; same && i < len; i++) {
        same = ref[i] == out[i];
      }

      CHECK(len > 0 && same, "identity LUT = no LUT (mode %d, colour %d)", mode,
            color);
    }
  }

  // Colour channels go through curve
  flat(100);
  ascii_opts_t c = base();
  c.color = 1;
  len = render(&c);
  CHECK(has(len, "38;2;100;100;100"), "colour without curve keeps 100");
  c.tone_lut = invert;
  len = render(&c);
  CHECK(has(len, "38;2;155;155;155") && !has(len, "38;2;100;100;100"),
        "colour with invert curve is 155");

  // Histogram: before curve, one count per cell
  static uint32_t hist[256];
  nl_memset(hist, 0, sizeof(hist));
  ascii_opts_t h = base();
  h.tone_lut = invert;
  h.tone_hist = hist;
  flat(100);
  (void)render(&h);
  uint32_t total = 0;
  for (int i = 0; i < 256; i++) {
    total += hist[i];
  }

  CHECK(hist[100] > 0 && hist[100] == total && hist[155] == 0,
        "histogram counts the value before the curve (%u of %u)", hist[100],
        total);
  nl_memset(hist, 0, sizeof(hist));
  h.tone_lut = NULL;
  (void)render(&h);
  total = 0;
  for (int i = 0; i < 256; i++) {
    total += hist[i];
  }

  CHECK(total > 0 && hist[100] == total, "histogram works without a curve");

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
