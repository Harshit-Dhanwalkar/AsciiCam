/*
 * tests/test_gfx.c - kitty and sixel encoders, checked by decoding their
 * output back to pixels; resampler; terminal detection
 */

#include "../lib/nolibc.h"
#include "gfx.h"

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

static int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = a, *y = b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) {
      return x[i] < y[i] ? -1 : 1;
    }
  }

  return 0;
}

static uint32_t rng = 12345u;
static unsigned rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;

  return rng;
}

#define MAXPIX (300 * 200 * 3)
static uint8_t src[MAXPIX], dec[MAXPIX], rs[MAXPIX];
static char buf[MAXPIX * 8];

// Kitty decoder
static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }

  return -1;
}

typedef struct {
  int chunks, bad, w, h, cols, rows, ended;
  size_t n;
} kinfo_t;

static kinfo_t kitty_decode(const char *s, size_t len, uint8_t *dst,
                            size_t dcap) {
  kinfo_t k = {0};
  size_t i = 0;
  int more = 1;
  while (i < len && more) {
    if (len - i < 3 || s[i] != '\033' || s[i + 1] != '_' || s[i + 2] != 'G') {
      k.bad = 1;
      return k;
    }

    i += 3;
    size_t ctl = i;
    while (i < len && s[i] != ';') {
      i++;
    }

    // control keys
    for (size_t j = ctl; j < i;) {
      char key = s[j];
      size_t e = j;
      while (e < i && s[e] != ',') {
        e++;
      }

      long v = 0;
      if (e > j + 2 && s[j + 1] == '=' && s[j + 2] != '-') {
        for (size_t t = j + 2; t < e; t++) {
          if (s[t] >= '0' && s[t] <= '9') {
            v = v * 10 + (s[t] - '0');
          }
        }
      }

      if (key == 's') {
        k.w = (int)v;
      } else if (key == 'v') {
        k.h = (int)v;
      } else if (key == 'c') {
        k.cols = (int)v;
      } else if (key == 'r') {
        k.rows = (int)v;
      } else if (key == 'm') {
        more = (int)v;
      }

      j = e + 1;
    }

    i++; // ';'
    size_t pay = 0;
    while (i < len && s[i] != '\033') {
      int a = b64v(s[i]), b = b64v(s[i + 1]);
      if (a < 0 || b < 0) {
        k.bad = 1;
        return k;
      }

      unsigned v = ((unsigned)a << 18) | ((unsigned)b << 12);
      int pad = 0;
      if (s[i + 2] == '=') {
        pad = 2;
      } else {
        v |= (unsigned)b64v(s[i + 2]) << 6;
        if (s[i + 3] == '=') {
          pad = 1;
        } else {
          v |= (unsigned)b64v(s[i + 3]);
        }
      }

      for (int q = 0; q < 3 - pad; q++) {
        if (k.n < dcap) {
          dst[k.n] = (uint8_t)(v >> (16 - 8 * q));
        }
        k.n++;
      }

      i += 4;
      pay += 4;
    }

    if (pay > 4096) {
      k.bad = 1;
    }

    if (i + 1 >= len || s[i] != '\033' || s[i + 1] != '\\') {
      k.bad = 1;
      return k;
    }

    i += 2;
    k.chunks++;
  }

  k.ended = (i == len && !more);

  return k;
}

static void test_kitty(void) {
  static const int sizes[][2] = {{1, 1},  {2, 1},   {1, 2},        {3, 3},
                                 {16, 9}, {64, 48}, {1024 / 8, 3}, {101, 37}};
  for (size_t t = 0; t < sizeof(sizes) / sizeof(sizes[0]); t++) {
    int w = sizes[t][0], h = sizes[t][1];
    for (int i = 0; i < w * h * 3; i++) {
      src[i] = (uint8_t)rnd();
    }

    int n = gfx_kitty_encode(src, w, h, 20, 10, buf, gfx_kitty_cap(w, h));
    CHECK(n > 0, "%dx%d encodes", w, h);
    nl_memset(dec, 0, sizeof(dec));
    kinfo_t k = kitty_decode(buf, (size_t)n, dec, sizeof(dec));
    CHECK(!k.bad && k.ended, "%dx%d well-formed (bad=%d ended=%d)", w, h, k.bad,
          k.ended);
    CHECK(k.n == (size_t)(w * h * 3), "%dx%d payload size %zu", w, h, k.n);
    CHECK(memcmp(src, dec, (size_t)(w * h * 3)) == 0, "%dx%d pixels round-trip",
          w, h);
    CHECK(k.w == w && k.h == h && k.cols == 20 && k.rows == 10,
          "%dx%d geometry keys (%d %d %d %d)", w, h, k.w, k.h, k.cols, k.rows);
    size_t want = ((size_t)w * h * 3 + 3071) / 3072;
    CHECK((size_t)k.chunks == want, "%dx%d %d chunks, want %zu", w, h, k.chunks,
          want);
  }

  // output room is checked, never overrun
  int w = 40, h = 30;
  size_t cap = gfx_kitty_cap(w, h);
  CHECK(gfx_kitty_encode(src, w, h, 4, 4, buf, cap) > 0, "cap is enough");
  CHECK((size_t)gfx_kitty_encode(src, w, h, 4, 4, buf, cap) <= cap,
        "never above cap");
  CHECK(gfx_kitty_encode(src, w, h, 4, 4, buf, cap / 2) == -1,
        "too small buffer refused");
  CHECK(gfx_kitty_encode(src, 0, h, 4, 4, buf, cap) == -1, "zero width");
  CHECK(gfx_kitty_encode(src, w, h, 0, 4, buf, cap) == -1, "zero cols");
  CHECK(gfx_kitty_encode(NULL, w, h, 4, 4, buf, cap) == -1, "NULL pixels");
  CHECK(gfx_kitty_encode(src, GFX_MAX_W + 1, 1, 4, 4, buf, cap) == -1,
        "too wide");

  char d[64];
  int dn = gfx_kitty_delete(d, sizeof(d));
  CHECK(dn > 0 && d[0] == '\033', "delete sequence");
  CHECK(gfx_kitty_delete(d, 3) == -1, "delete into tiny buffer");
}

// Sixel decoder: returns decoded RGB (palette percentages -> 0..255)
typedef struct {
  int w, h, bad;
  int colors_defined;
} sinfo_t;

static sinfo_t sixel_decode(const char *s, size_t len, uint8_t *img, int maxw,
                            int maxh) {
  sinfo_t r = {0};
  uint8_t pal[256][3];
  nl_memset(pal, 0, sizeof(pal));
  nl_memset(img, 0, (size_t)maxw * (size_t)maxh * 3u);
  size_t i = 0;
  if (len < 6 || memcmp(s, "\033P0;1;q\"1;1;", 12) != 0) {
    r.bad = 1;
    return r;
  }

  i = 12;
  long dims[2] = {0, 0};
  for (int d = 0; d < 2; d++) {
    while (i < len && s[i] >= '0' && s[i] <= '9') {
      dims[d] = dims[d] * 10 + (s[i++] - '0');
    }

    i++; // ';' between, then first command char for second
    if (d == 1) {
      i--;
    }
  }

  r.w = (int)dims[0];
  r.h = (int)dims[1];
  int cur = 0, x = 0, band = 0;
  while (i < len) {
    char c = s[i];
    if (c == '\033') {
      break;
    }

    if (c == '#') {
      i++;
      long v[5] = {0};
      int nv = 0;
      int have = 0;
      while (i < len && ((s[i] >= '0' && s[i] <= '9') || s[i] == ';')) {
        if (s[i] == ';') {
          nv++;
        } else {
          v[nv] = v[nv] * 10 + (s[i] - '0');
          have = 1;
        }
        i++;
      }

      if (nv == 0 && have) {
        cur = (int)v[0];
      } else if (nv == 4) {
        if (v[1] != 2 || v[0] > 255) {
          r.bad = 1;
          return r;
        }

        cur = (int)v[0];
        for (int k = 0; k < 3; k++) {
          pal[cur][k] = (uint8_t)((v[2 + k] * 255 + 50) / 100);
        }
        r.colors_defined++;
      } else {
        r.bad = 1;
        return r;
      }
    } else if (c == '$') {
      x = 0;
      i++;
    } else if (c == '-') {
      x = 0;
      band += 6;
      i++;
    } else if (c == '!') {
      i++;
      long n = 0;
      while (i < len && s[i] >= '0' && s[i] <= '9') {
        n = n * 10 + (s[i++] - '0');
      }

      char ch = s[i++];
      for (long k = 0; k < n; k++, x++) {
        for (int b = 0; b < 6; b++) {
          if (((ch - 63) >> b) & 1) {
            int yy = band + b;
            if (x < maxw && yy < maxh) {
              nl_memcpy(img + ((size_t)yy * (size_t)maxw + (size_t)x) * 3u,
                        pal[cur], 3);
            }
          }
        }
      }
    } else if (c >= 63 && c <= 126) {
      for (int b = 0; b < 6; b++) {
        if (((c - 63) >> b) & 1) {
          int yy = band + b;
          if (x < maxw && yy < maxh) {
            nl_memcpy(img + ((size_t)yy * (size_t)maxw + (size_t)x) * 3u,
                      pal[cur], 3);
          }
        }
      }

      x++;
      i++;
    } else {
      r.bad = 1;
      return r;
    }
  }

  if (!(i + 1 < len && s[i] == '\033' && s[i + 1] == '\\')) {
    r.bad = 1;
  }

  return r;
}

static int iabs(int v) { return v < 0 ? -v : v; }

// Expected encoder to show for a pixel
static void expect(const uint8_t *p, int mono, uint8_t *o) {
  if (mono) {
    unsigned q = p[0] >> 2;
    unsigned pc = (q * 100u + 31u) / 63u;
    o[0] = o[1] = o[2] = (uint8_t)((pc * 255u + 50u) / 100u);
    return;
  }

  for (int k = 0; k < 3; k++) {
    unsigned q = (p[k] * 5u + 127u) / 255u;
    unsigned pc = (q * 100u + 2u) / 5u;
    o[k] = (uint8_t)((pc * 255u + 50u) / 100u);
  }
}

static void check_sixel(int w, int h, int mono, int flat) {
  for (int i = 0; i < w * h; i++) {
    uint8_t v = (uint8_t)rnd();
    if (flat) {
      v = (uint8_t)(i / 7 * 40);
    }

    if (mono) {
      src[i * 3] = src[i * 3 + 1] = src[i * 3 + 2] = v;
    } else if (flat) {
      src[i * 3] = v;
      src[i * 3 + 1] = (uint8_t)(255 - v);
      src[i * 3 + 2] = (uint8_t)(v / 2);
    } else {
      src[i * 3] = v;
      src[i * 3 + 1] = (uint8_t)rnd();
      src[i * 3 + 2] = (uint8_t)rnd();
    }
  }

  int n = gfx_sixel_encode(src, w, h, mono, buf, gfx_sixel_cap(w, h));
  CHECK(n > 0, "sixel %dx%d mono=%d flat=%d encodes", w, h, mono, flat);
  if (n <= 0) {
    return;
  }

  sinfo_t s = sixel_decode(buf, (size_t)n, dec, w, h);
  CHECK(!s.bad && s.w == w && s.h == h, "sixel %dx%d well-formed (%d, %dx%d)",
        w, h, s.bad, s.w, s.h);
  int worst = 0;
  for (int i = 0; i < w * h; i++) {
    uint8_t e[3];
    expect(src + i * 3, mono, e);
    for (int k = 0; k < 3; k++) {
      int d = iabs(e[k] - dec[i * 3 + k]);
      if (d > worst) {
        worst = d;
      }
    }
  }

  CHECK(worst <= 1, "sixel %dx%d mono=%d flat=%d pixels match palette (%d off)",
        w, h, mono, flat, worst);

  // Check if it close to picture itself
  int far = 0;
  for (int i = 0; i < w * h * 3; i++) {
    if (iabs(src[i] - dec[i]) > (mono ? 4 : 52)) {
      far++;
    }
  }

  CHECK(far == 0, "sixel %dx%d mono=%d stays near source (%d far)", w, h, mono,
        far);
}

static void test_sixel(void) {
  static const int sizes[][2] = {{1, 1}, {1, 6},   {1, 7},   {7, 5},
                                 {6, 6}, {13, 13}, {50, 31}, {200, 100}};
  for (size_t t = 0; t < sizeof(sizes) / sizeof(sizes[0]); t++) {
    for (int mono = 0; mono < 2; mono++) {
      for (int flat = 0; flat < 2; flat++) {
        check_sixel(sizes[t][0], sizes[t][1], mono, flat);
      }
    }
  }

  // Flat colour compresses with run-length codes
  for (int i = 0; i < 100 * 60; i++) {
    src[i * 3] = 200;
    src[i * 3 + 1] = 40;
    src[i * 3 + 2] = 40;
  }

  int n = gfx_sixel_encode(src, 100, 60, 0, buf, gfx_sixel_cap(100, 60));
  CHECK(n > 0 && n < 400, "flat picture is tiny (%d bytes)", n);

  // Too small a buffer
  CHECK(gfx_sixel_encode(src, 100, 60, 0, buf, 20) == -1,
        "tiny buffer refused");
  CHECK(gfx_sixel_encode(src, 0, 60, 0, buf, 4096) == -1, "zero width");
  CHECK(gfx_sixel_encode(NULL, 5, 5, 0, buf, 4096) == -1, "NULL pixels");
  CHECK(gfx_sixel_encode(src, GFX_MAX_W + 1, 1, 0, buf, 4096) == -1,
        "too wide");
}

static void test_resample(void) {
  // Identity
  for (int i = 0; i < 20 * 10 * 3; i++) {
    src[i] = (uint8_t)rnd();
  }

  CHECK(gfx_resample(NULL, src, 20, 10, 20, 10, rs) == 0, "identity runs");
  CHECK(memcmp(src, rs, 20 * 10 * 3) == 0, "same size is a copy");

  // Luma plane becomes grey rgb
  uint8_t g[8 * 8];
  for (int i = 0; i < 64; i++) {
    g[i] = (uint8_t)(i * 4);
  }

  CHECK(gfx_resample(g, NULL, 8, 8, 8, 8, rs) == 0, "luma runs");
  int ok = 1;
  for (int i = 0; i < 64; i++) {
    ok &= rs[i * 3] == g[i] && rs[i * 3 + 1] == g[i] && rs[i * 3 + 2] == g[i];
  }

  CHECK(ok, "luma to grey rgb");

  // 2x2 average
  uint8_t four[4 * 3] = {0, 0, 0, 100, 100, 100, 200, 200, 200, 100, 0, 0};
  CHECK(gfx_resample(NULL, four, 2, 2, 1, 1, rs) == 0, "2x2 -> 1x1");
  CHECK(rs[0] == 100 && rs[1] == 75 && rs[2] == 75, "average (%d %d %d)", rs[0],
        rs[1], rs[2]);

  // Flat stays flat at any scale, up and down
  for (int i = 0; i < 31 * 17; i++) {
    src[i * 3] = 17;
    src[i * 3 + 1] = 99;
    src[i * 3 + 2] = 250;
  }

  static const int d[][2] = {{1, 1}, {5, 3}, {31, 17}, {62, 40}, {100, 100}};
  for (size_t t = 0; t < sizeof(d) / sizeof(d[0]); t++) {
    CHECK(gfx_resample(NULL, src, 31, 17, d[t][0], d[t][1], rs) == 0,
          "scale ok");
    int flat = 1;
    for (int i = 0; i < d[t][0] * d[t][1]; i++) {
      flat &= rs[i * 3] == 17 && rs[i * 3 + 1] == 99 && rs[i * 3 + 2] == 250;
    }

    CHECK(flat, "flat colour stays flat at %dx%d", d[t][0], d[t][1]);
  }

  CHECK(gfx_resample(NULL, src, 31, 17, 0, 4, rs) == -1, "zero dst");
  CHECK(gfx_resample(NULL, NULL, 31, 17, 4, 4, rs) == -1, "no source");
  CHECK(gfx_resample(NULL, src, GFX_MAX_W + 1, 1, 4, 4, rs) == -1,
        "src too wide");
}

static void test_parse(void) {
  CHECK(gfx_parse("off", "xterm", NULL, 0) == GFX_OFF, "off");
  CHECK(gfx_parse(NULL, "xterm", NULL, 0) == GFX_OFF, "NULL is off");
  CHECK(gfx_parse("kitty", "xterm", NULL, 0) == GFX_KITTY, "kitty");
  CHECK(gfx_parse("sixel", "xterm", NULL, 0) == GFX_SIXEL, "sixel");
  CHECK(gfx_parse("png", "xterm", NULL, 0) == -1, "unknown name");
  CHECK(gfx_parse("auto", "xterm-256color", NULL, 1) == GFX_KITTY,
        "kitty window id");
  CHECK(gfx_parse("auto", "xterm-kitty", NULL, 0) == GFX_KITTY, "TERM kitty");
  CHECK(gfx_parse("auto", "xterm-ghostty", NULL, 0) == GFX_KITTY,
        "TERM ghostty");
  CHECK(gfx_parse("auto", "xterm-256color", "WezTerm", 0) == GFX_KITTY,
        "wezterm");
  CHECK(gfx_parse("auto", "foot", NULL, 0) == GFX_SIXEL, "foot");
  CHECK(gfx_parse("auto", "mlterm", NULL, 0) == GFX_SIXEL, "mlterm");
  CHECK(gfx_parse("auto", "xterm-256color", NULL, 0) == GFX_OFF, "plain xterm");
  CHECK(gfx_parse("auto", NULL, NULL, 0) == GFX_OFF, "no environment");
}

int main(void) {
  test_kitty();
  test_sixel();
  test_resample();
  test_parse();
  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}

