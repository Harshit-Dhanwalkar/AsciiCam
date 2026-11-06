/*
 * tests/test_netenc.c - delta encoder (src/netenc.c)
 *
 * Encoder's output is replayed onto a small terminal model (written separately
 * from encoder's own parser) and compared with what same frame looks like when
 * painted directly: every lossless run must leave model identical to real
 * picture
 */

#include "../lib/nolibc.h"

#include "ascii.h"
#include "netenc.h"

static int _pass = 0;
static int _fail = 0;

static void check(int ok, const char *what, int line) {
  if (ok) {
    _pass++;

    return;
  }

  _fail++;
  char b[200];
  int n = nl_snprintf(b, sizeof(b), "FAIL [%d] %s\n", line, what);
  nl_write(1, b, (size_t)n);
}

#define CHECK(c, what) check((c) != 0, (what), __LINE__)

// Terminal model

#define TR 64
#define TC 200

typedef struct {
  uint32_t glyph, fg, bg;
} mcell_t;

typedef struct {
  mcell_t cell[TR][TC];
  int r, c;
  uint32_t fg, bg;
} term_t;

static void term_reset(term_t *t) {
  nl_memset(t, 0, sizeof(*t));
  t->r = 1;
  t->c = 1;
  for (int r = 0; r < TR; r++) {
    for (int c = 0; c < TC; c++) {
      t->cell[r][c].glyph = ' ';
    }
  }
}

static uint32_t m_rgb(int r, int g, int b) {
  return (1u << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t m_pal(int i) { return (2u << 24) | (uint32_t)i; }

static void term_sgr(term_t *t, const int *p, int np) {
  if (np == 0) {
    t->fg = 0;
    t->bg = 0;
  }
  for (int i = 0; i < np; i++) {
    if (p[i] == 0) {
      t->fg = 0;
      t->bg = 0;
    } else if (p[i] == 39) {
      t->fg = 0;
    } else if (p[i] == 49) {
      t->bg = 0;
    } else if ((p[i] == 38 || p[i] == 48) && p[i + 1] == 5) {
      (p[i] == 38 ? &t->fg : &t->bg)[0] = m_pal(p[i + 2]);
      i += 2;
    } else if ((p[i] == 38 || p[i] == 48) && p[i + 1] == 2) {
      (p[i] == 38 ? &t->fg : &t->bg)[0] = m_rgb(p[i + 2], p[i + 3], p[i + 4]);
      i += 4;
    } else {
      CHECK(0, "terminal model: unknown SGR parameter");
    }
  }
}

static void term_feed(term_t *t, const char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char b = (unsigned char)s[i];
    if (b == 0x1B) {
      i += 2; // ESC [
      int p[16] = {0};
      int np = 0;
      int cur = 0;
      int have = 0;
      while (i < n) {
        unsigned char ch = (unsigned char)s[i++];
        if (ch >= '0' && ch <= '9') {
          cur = cur * 10 + (ch - '0');
          have = 1;
        } else if (ch == ';') {
          p[np++] = cur;
          cur = 0;
          have = 0;
        } else {
          if (have || np > 0) {
            p[np++] = cur;
          }

          if (ch == 'H') {
            t->r = np > 0 && p[0] ? p[0] : 1;
            t->c = np > 1 && p[1] ? p[1] : 1;
          } else if (ch == 'C') {
            t->c += np > 0 && p[0] ? p[0] : 1;
          } else if (ch == 'm') {
            term_sgr(t, p, np);
          } else {
            CHECK(0, "terminal model: unknown CSI final byte");
          }

          break;
        }
      }

      continue;
    }

    if (b == '\n') {
      t->r++;
      t->c = 1;
      i++;

      continue;
    }

    int len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    uint32_t g = 0;
    for (int k = 0; k < len; k++) {
      g |= (uint32_t)(unsigned char)s[i + (size_t)k] << (8 * k);
    }

    i += (size_t)len;
    if (t->r >= 1 && t->r <= TR && t->c >= 1 && t->c <= TC) {
      mcell_t *c = &t->cell[t->r - 1][t->c - 1];
      c->glyph = g;
      c->fg = t->fg;
      c->bg = t->bg;
    }

    t->c++;
  }
}

static int term_equal(const term_t *a, const term_t *b) {
  for (int r = 0; r < TR; r++) {
    for (int c = 0; c < TC; c++) {
      if (a->cell[r][c].glyph != b->cell[r][c].glyph ||
          a->cell[r][c].fg != b->cell[r][c].fg ||
          a->cell[r][c].bg != b->cell[r][c].bg) {
        return 0;
      }
    }
  }

  return 1;
}

// Frame generator

typedef enum { M_MONO, M_C256, M_TRUE, M_HALF, M_BRAILLE } fmode_t;

static uint32_t rng_state = 88172645u;

static uint32_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;

  return rng_state;
}

typedef struct {
  uint8_t v[TR][TC][6]; // per cell: fg rgb, bg rgb
  uint8_t g[TR][TC];    // glyph choice
} scene_t;

static void scene_random(scene_t *s, int rows, int cols) {
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      for (int k = 0; k < 6; k++) {
        s->v[r][c][k] = (uint8_t)rnd();
      }

      s->g[r][c] = (uint8_t)(rnd() % 10);
    }
  }
}

// change a fraction (percent) of cells; "big" ones get a new colour, others
// only drift by a few levels
static void scene_mutate(scene_t *s, int rows, int cols, int percent, int big) {
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      if ((int)(rnd() % 100) >= percent) {
        continue;
      }

      for (int k = 0; k < 6; k++) {
        if (big) {
          s->v[r][c][k] = (uint8_t)rnd();
        } else {
          int d = (int)(rnd() % 5) - 2;
          int v = s->v[r][c][k] + d;
          s->v[r][c][k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
      }
      if (big) {
        s->g[r][c] = (uint8_t)(rnd() % 10);
      }
    }
  }
}

static const char *ramp_glyph[10] = {" ", ".", ":", "-", "=",
                                     "+", "*", "#", "%", "@"};
static const char *fancy_glyph[10] = {" ",
                                      "\xe2\x96\x80",
                                      "\xe2\x96\x84",
                                      "\xe2\x96\x88",
                                      "\xe2\xa0\x81",
                                      "\xe2\xa3\xbf",
                                      "\xe2\x96\x91",
                                      "\xe2\x96\x92",
                                      "\xe2\x96\x93",
                                      "\xf0\x9f\x80\x80"};

static size_t scene_to_stream(const scene_t *s, int rows, int cols, fmode_t m,
                              int ox, int oy, char *out, size_t cap) {
  size_t n = 0;
  if (ox > 1 || oy > 1) {
    n += (size_t)nl_snprintf(out + n, cap - n, "\033[%d;%dH", oy, ox);
  } else {
    n += (size_t)nl_snprintf(out + n, cap - n, "\033[H");
  }

  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      const uint8_t *v = s->v[r][c];
      const char *g = (m == M_HALF || m == M_BRAILLE) ? fancy_glyph[s->g[r][c]]
                                                      : ramp_glyph[s->g[r][c]];
      if (m == M_C256) {
        n += (size_t)nl_snprintf(out + n, cap - n, "\033[38;5;%dm%s", v[0], g);
      } else if (m == M_TRUE || m == M_BRAILLE) {
        n += (size_t)nl_snprintf(out + n, cap - n, "\033[38;2;%d;%d;%dm%s",
                                 v[0], v[1], v[2], g);
      } else if (m == M_HALF) {
        n += (size_t)nl_snprintf(out + n, cap - n,
                                 "\033[38;2;%d;%d;%dm\033[48;2;%d;%d;%dm%s",
                                 v[0], v[1], v[2], v[3], v[4], v[5], g);
      } else {
        n += (size_t)nl_snprintf(out + n, cap - n, "%s", g);
      }
    }

    if (m != M_MONO) {
      n += (size_t)nl_snprintf(out + n, cap - n, "\033[0m");
    }

    if (ox > 1) {
      n += (size_t)nl_snprintf(out + n, cap - n, "\033[%d;%dH", oy + r + 1, ox);
    } else {
      n += (size_t)nl_snprintf(out + n, cap - n, "\n");
    }
  }

  return n;
}

#define BUF 400000
static char frame_buf[BUF];
static char enc_buf[BUF];

// encode `frame`, feed model, and compare with directly painted frame
static int roundtrip(netenc_t *e, term_t *shown, const char *frame, size_t fn,
                     int *bytes) {
  int n = netenc_frame(e, frame, fn, enc_buf, sizeof(enc_buf));
  if (n < 0) {
    return -1;
  }

  term_feed(shown, enc_buf, (size_t)n);
  if (bytes) {
    *bytes = n;
  }

  return n;
}

static void direct(term_t *want, const char *frame, size_t fn) {
  term_reset(want);
  term_feed(want, frame, fn);
}

static scene_t scene;

static void test_lossless_modes(void) {
  static const fmode_t modes[] = {M_MONO, M_C256, M_TRUE, M_HALF, M_BRAILLE};
  static term_t shown, want;
  for (unsigned mi = 0; mi < sizeof(modes) / sizeof(modes[0]); mi++) {
    netenc_t e;
    netenc_init(&e, 0, 0);
    term_reset(&shown);
    rng_state = 12345u + mi;
    int rows = 20;
    int cols = 60;
    scene_random(&scene, rows, cols);
    int all_equal = 1;
    int first_bytes = 0;
    int delta_bytes = 0;
    int still_bytes = -1;
    for (int f = 0; f < 12; f++) {
      if (f > 0) {
        scene_mutate(&scene, rows, cols, f % 3 == 0 ? 40 : 5, f % 2);
      }

      size_t fn = scene_to_stream(&scene, rows, cols, modes[mi], 1, 1,
                                  frame_buf, sizeof(frame_buf));
      int bytes = 0;
      int n = roundtrip(&e, &shown, frame_buf, fn, &bytes);
      direct(&want, frame_buf, fn);
      if (n < 0 || !term_equal(&shown, &want)) {
        all_equal = 0;
      }
      if (f == 0) {
        first_bytes = bytes;
      }
      if (f == 1) {
        delta_bytes = bytes;
      }

      // same frame again costs nothing
      int again = roundtrip(&e, &shown, frame_buf, fn, &bytes);
      if (f == 3) {
        still_bytes = again;
      }
      if (again != 0 || !term_equal(&shown, &want)) {
        all_equal = 0;
      }
    }

    CHECK(all_equal, "lossless delta leaves terminal identical");
    CHECK(still_bytes == 0, "an unchanged frame sends nothing");
    CHECK(delta_bytes < first_bytes,
          "a 5%% change costs less than a full frame");
    CHECK(e.pending == 0, "nothing pending in lossless mode");
    CHECK(e.out_bytes < e.in_bytes, "total output below total input");
    netenc_free(&e);
  }
}

static void test_dense_mono_not_worse(void) {
  // when most cells change, delta must not cost more than frame
  static term_t shown, want;
  netenc_t e;
  netenc_init(&e, 0, 0);
  term_reset(&shown);
  rng_state = 8080u;
  scene_random(&scene, 24, 80);
  size_t fn = scene_to_stream(&scene, 24, 80, M_MONO, 1, 1, frame_buf,
                              sizeof(frame_buf));
  roundtrip(&e, &shown, frame_buf, fn, NULL);
  int worse = 0;
  int ok = 1;
  for (int f = 0; f < 30; f++) {
    scene_mutate(&scene, 24, 80, 30 + f, 1);
    fn = scene_to_stream(&scene, 24, 80, M_MONO, 1, 1, frame_buf,
                         sizeof(frame_buf));
    int bytes = 0;
    roundtrip(&e, &shown, frame_buf, fn, &bytes);
    direct(&want, frame_buf, fn);
    if (!term_equal(&shown, &want)) {
      ok = 0;
    }
    if ((size_t)bytes > fn) {
      worse++;
    }
  }

  CHECK(ok, "gap filling still reproduces the picture");
  CHECK(worse == 0, "a dense mono change never costs more than the frame");
  netenc_free(&e);
}

static void test_origin_and_overlay(void) {
  static term_t shown, want;
  netenc_t e;
  netenc_init(&e, 0, 0);
  term_reset(&shown);
  rng_state = 777u;
  scene_random(&scene, 12, 30);
  // frame placed at column 20 / row 5: every row repositions explicitly
  size_t fn = scene_to_stream(&scene, 12, 30, M_TRUE, 20, 5, frame_buf,
                              sizeof(frame_buf));
  CHECK(roundtrip(&e, &shown, frame_buf, fn, NULL) > 0, "offset frame encodes");
  direct(&want, frame_buf, fn);
  CHECK(term_equal(&shown, &want), "offset frame lands in the right cells");
  CHECK(shown.cell[0][0].glyph == ' ' && shown.cell[4][19].glyph != ' ',
        "nothing drawn left/above the frame");

  // an overlay (an fps box) is just more cells in same stream
  size_t more = fn;
  more += (size_t)nl_snprintf(
      frame_buf + more, sizeof(frame_buf) - more,
      "\033[5;40H\033[38;2;0;255;0m\033[48;2;30;30;30m[ FPS: 20.0 ]\033[0m");
  CHECK(roundtrip(&e, &shown, frame_buf, more, NULL) > 0, "overlay encodes");
  direct(&want, frame_buf, more);
  CHECK(term_equal(&shown, &want), "overlay cells are painted");
  netenc_free(&e);
}

static void test_hysteresis(void) {
  static term_t shown, want;
  netenc_t e;
  netenc_init(&e, 30, 0);
  term_reset(&shown);
  rng_state = 4242u;
  int rows = 10;
  int cols = 40;
  scene_random(&scene, rows, cols);
  size_t fn = scene_to_stream(&scene, rows, cols, M_TRUE, 1, 1, frame_buf,
                              sizeof(frame_buf));
  roundtrip(&e, &shown, frame_buf, fn, NULL);

  // +-2 per channel: below threshold, must not be sent
  scene_mutate(&scene, rows, cols, 100, 0);
  fn = scene_to_stream(&scene, rows, cols, M_TRUE, 1, 1, frame_buf,
                       sizeof(frame_buf));
  int bytes = 0;
  roundtrip(&e, &shown, frame_buf, fn, &bytes);
  CHECK(bytes < 100, "tiny colour drift is ignored");

  // a different glyph is always sent, however small colour change
  scene.g[3][3] = (uint8_t)((scene.g[3][3] + 1) % 10);
  fn = scene_to_stream(&scene, rows, cols, M_TRUE, 1, 1, frame_buf,
                       sizeof(frame_buf));
  roundtrip(&e, &shown, frame_buf, fn, &bytes);
  CHECK(bytes > 0, "a glyph change is sent");
  CHECK(shown.cell[3][3].glyph ==
                (uint32_t)(unsigned char)ramp_glyph[scene.g[3][3]][0] ||
            shown.cell[3][3].glyph != ' ' || scene.g[3][3] == 0,
        "the glyph reached the terminal");

  // drift accumulates against what was last SENT, so it cannot hide forever
  for (int k = 0; k < 40; k++) {
    for (int c = 0; c < cols; c++) {
      scene.v[7][c][0] =
          (uint8_t)(scene.v[7][c][0] > 250 ? 250 : scene.v[7][c][0] + 2);
    }

    fn = scene_to_stream(&scene, rows, cols, M_TRUE, 1, 1, frame_buf,
                         sizeof(frame_buf));
    roundtrip(&e, &shown, frame_buf, fn, &bytes);
  }

  direct(&want, frame_buf, fn);
  int worst = 0;
  for (int c = 0; c < cols; c++) {
    int d = (int)((want.cell[7][c].fg >> 16) & 255u) -
            (int)((shown.cell[7][c].fg >> 16) & 255u);
    worst = d < 0 ? -d : d > worst ? d : worst > -d ? worst : -d;
  }

  CHECK(worst < 30, "slow drift is eventually sent");
  netenc_free(&e);
}

static void test_budget(void) {
  static term_t shown, want;
  netenc_t e;
  int budget = 1500;
  netenc_init(&e, 0, budget);
  term_reset(&shown);
  rng_state = 99u;
  int rows = 20;
  int cols = 60;
  scene_random(&scene, rows, cols);
  size_t fn = scene_to_stream(&scene, rows, cols, M_TRUE, 1, 1, frame_buf,
                              sizeof(frame_buf));
  direct(&want, frame_buf, fn);

  int worst_bytes = 0;
  int frames_to_converge = 0;
  for (int f = 1; f <= 200; f++) {
    int bytes = 0;
    roundtrip(&e, &shown, frame_buf, fn, &bytes);
    if (bytes > worst_bytes) {
      worst_bytes = bytes;
    }

    if (term_equal(&shown, &want)) {
      frames_to_converge = f;

      break;
    }
  }

  CHECK(frames_to_converge > 1,
        "a first frame bigger than the budget takes several frames");
  CHECK(frames_to_converge > 0 && frames_to_converge < 200,
        "and still converges");
  CHECK(worst_bytes <= budget + 120, "every frame stays near the budget");
  CHECK(e.pending == 0, "nothing pending once converged");

  // most visible changes first: half cells change a lot, half a little
  term_reset(&shown);
  netenc_free(&e);
  netenc_init(&e, 0, 700);
  rows = 10;
  cols = 40;
  scene_random(&scene, rows, cols);
  fn = scene_to_stream(&scene, rows, cols, M_TRUE, 1, 1, frame_buf,
                       sizeof(frame_buf));
  for (int f = 0; f < 60; f++) {
    roundtrip(&e, &shown, frame_buf, fn, NULL);
  }

  direct(&want, frame_buf, fn);
  CHECK(term_equal(&shown, &want), "baseline established");
  // rows 0-4: big change, rows 5-9: change of ~30 per cell
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      for (int k = 0; k < 3; k++) {
        int v = scene.v[r][c][k];
        scene.v[r][c][k] =
            (uint8_t)(r < 5 ? 255 - v : (v < 200 ? v + 30 : v - 30));
      }
    }
  }

  fn = scene_to_stream(&scene, rows, cols, M_TRUE, 1, 1, frame_buf,
                       sizeof(frame_buf));
  term_t before = shown;
  roundtrip(&e, &shown, frame_buf, fn, NULL);
  int big_done = 0;
  int small_done = 0;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      if (shown.cell[r][c].fg != before.cell[r][c].fg) {
        if (r < 5) {
          big_done++;
        } else {
          small_done++;
        }
      }
    }
  }

  CHECK(big_done > 0, "the budget frame updates something");
  CHECK(small_done == 0 || big_done > 3 * small_done,
        "big changes are sent before small ones");
  netenc_free(&e);
}

static void test_invalidate_and_resize(void) {
  static term_t shown, want;
  netenc_t e;
  netenc_init(&e, 0, 0);
  term_reset(&shown);
  rng_state = 31337u;
  scene_random(&scene, 15, 50);
  size_t fn = scene_to_stream(&scene, 15, 50, M_C256, 1, 1, frame_buf,
                              sizeof(frame_buf));
  roundtrip(&e, &shown, frame_buf, fn, NULL);

  // something else paints over picture (a toast, a menu)
  term_feed(&shown, "\033[8;5H\033[0mXXXXXXXXXXXXXXXX", 28);
  direct(&want, frame_buf, fn);
  CHECK(!term_equal(&shown, &want), "model: the overlay damaged the picture");
  roundtrip(&e, &shown, frame_buf, fn, NULL);
  CHECK(!term_equal(&shown, &want),
        "without invalidate the damage stays (cells look unchanged)");
  netenc_invalidate(&e);
  int bytes = 0;
  roundtrip(&e, &shown, frame_buf, fn, &bytes);
  CHECK(term_equal(&shown, &want), "after invalidate the picture is repainted");
  CHECK(bytes > 500, "and repainted in full");

  // a different frame size starts over by itself
  scene_random(&scene, 9, 33);
  fn = scene_to_stream(&scene, 9, 33, M_C256, 1, 1, frame_buf,
                       sizeof(frame_buf));
  term_reset(&shown);
  roundtrip(&e, &shown, frame_buf, fn, &bytes);
  direct(&want, frame_buf, fn);
  CHECK(term_equal(&shown, &want), "new geometry is painted in full");
  netenc_free(&e);
}

static void test_unsupported_and_limits(void) {
  netenc_t e;
  netenc_init(&e, 0, 0);
  char out[4096];
  static const char *bad[] = {
      "\033[1mbold\033[0m", // bold is not in subset
      "\033[?25lhello",     // private mode
      "ab\tcd",             // tab
      "hello\033",          // truncated escape
      "hello\033[38;2;1;2", // truncated parameter list
      "\033[38;2;1;2;3",    // no final byte
      "\xe2\x96",           // truncated UTF-8
      "\xff\xfe",           // invalid lead byte
      "a\xe2"
      "b"
      "c",                 // bad continuation
      "\033]0;title\007",  // OSC
      "\033[2J",           // erase
      "\033[38;9;1m",      // unknown colour form
      "\033[99999999999m", // absurd number
      "\a",                // control character
  };
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    CHECK(netenc_frame(&e, bad[i], nl_strlen(bad[i]), out, sizeof(out)) == -1,
          "unsupported input is refused");
  }

  CHECK(netenc_frame(&e, "", 0, out, sizeof(out)) == 0,
        "empty frame: nothing to send");
  CHECK(netenc_frame(&e, "\033[H", 3, out, sizeof(out)) == 0,
        "cursor only: nothing to send");

  // a frame bigger than NETENC_MAX_CELLS is refused instead of allocating
  static char huge[300000];
  size_t n = 0;
  for (int r = 0; r < 300; r++) {
    for (int c = 0; c < 300; c++) {
      huge[n++] = 'x';
    }

    huge[n++] = '\n';
  }

  CHECK(netenc_frame(&e, huge, n, enc_buf, sizeof(enc_buf)) == -1,
        "oversized frame is refused");
  // 70000 columns would overflow rows*cols in a careless implementation
  CHECK(netenc_frame(&e, "\033[1;70000Hx", 11, out, sizeof(out)) == -1,
        "huge column is refused");
  CHECK(netenc_frame(&e, "\033[70000;70000Hx", 15, out, sizeof(out)) == -1,
        "huge row and column are refused");
  netenc_free(&e);
}

static void test_small_output_buffer(void) {
  static term_t shown, want;
  netenc_t e;
  netenc_init(&e, 0, 0);
  term_reset(&shown);
  rng_state = 555u;
  scene_random(&scene, 12, 40);
  size_t fn = scene_to_stream(&scene, 12, 40, M_TRUE, 1, 1, frame_buf,
                              sizeof(frame_buf));
  direct(&want, frame_buf, fn);

  char small[400];
  int worst = 0;
  int done = 0;
  for (int f = 0; f < 400; f++) {
    int n = netenc_frame(&e, frame_buf, fn, small, sizeof(small));
    if (n < 0) {
      break;
    }
    if (n > worst) {
      worst = n;
    }

    term_feed(&shown, small, (size_t)n);
    if (term_equal(&shown, &want)) {
      done = f + 1;

      break;
    }
  }

  CHECK(worst <= (int)sizeof(small), "output never exceeds the buffer");
  CHECK(done > 3, "a small buffer needs several frames");
  CHECK(done > 0, "and the picture still converges");
  netenc_free(&e);
}

static void test_fuzz(void) {
  netenc_t e;
  netenc_init(&e, 0, 0);
  static char junk[2048];
  static char out[8192 + 64];
  rng_state = 2024u;
  int refused = 0;
  int accepted = 0;
  const char *bits[] = {"\033[",        "38;2;", "48;5;", "m", "H",   ";", "\n",
                        "\xe2\x96\x88", "a",     "0",     "1", "255", "C"};
  for (int i = 0; i < 20000; i++) {
    size_t n = 0;
    int parts = (int)(rnd() % 40);
    for (int k = 0; k < parts && n + 16 < sizeof(junk); k++) {
      if (rnd() % 8 == 0) {
        junk[n++] = (char)rnd();
      } else {
        const char *b = bits[rnd() % (sizeof(bits) / sizeof(bits[0]))];
        size_t bl = nl_strlen(b);
        nl_memcpy(junk + n, b, bl);
        n += bl;
      }
    }

    nl_memset(out + 8192, 0xA5, 64);
    int r = netenc_frame(&e, junk, n, out, 8192);
    int guard = 1;
    for (int g = 0; g < 64; g++) {
      if ((unsigned char)out[8192 + g] != 0xA5) {
        guard = 0;
      }
    }

    CHECK(guard, "fuzz: output stayed inside the buffer");
    CHECK(r >= -1 && r <= 8192, "fuzz: sane return value");
    if (r < 0) {
      refused++;
      netenc_invalidate(&e);
    } else {
      accepted++;
    }
  }

  CHECK(refused > 100 && accepted > 100, "fuzz exercised both outcomes");
  netenc_free(&e);
}

static void test_real_renderer(void) {
  // actual escape output of grayscale_to_ascii, in every mode
  static term_t shown, want;
  static uint8_t gray[160 * 120];
  static uint8_t rgb[160 * 120 * 3];
  static char frame[400000];
  static char enc[400000];
  int checked = 0;
  int equal = 1;
  int saved = 0;
  for (int mode = 0; mode < 5; mode++) {
    for (int color = 0; color < 3; color++) {
      ascii_opts_t opts;
      nl_memset(&opts, 0, sizeof(opts));
      opts.contrast = 100;
      opts.gamma = 100;
      opts.render_mode = (render_mode_t)mode;
      opts.color = color != 0;
      opts.color_mode = color == 2 ? COLOR_256 : COLOR_TRUECOLOR;

      netenc_t e;
      netenc_init(&e, 0, 0);
      term_reset(&shown);
      for (int f = 0; f < 6; f++) {
        for (int y = 0; y < 120; y++) {
          for (int x = 0; x < 160; x++) {
            int v =
                (x * 255 / 159 + f * 9 + ((x / 24 + y / 20) % 2) * 60) & 255;
            gray[y * 160 + x] = (uint8_t)v;
            rgb[(y * 160 + x) * 3 + 0] = (uint8_t)v;
            rgb[(y * 160 + x) * 3 + 1] = (uint8_t)(255 - v);
            rgb[(y * 160 + x) * 3 + 2] = (uint8_t)(y * 2);
          }
        }

        int len = grayscale_to_ascii(gray, rgb, 160, 120, 50, 24, frame,
                                     sizeof(frame), &opts);
        if (len <= 0) {
          equal = 0;

          continue;
        }

        int n = netenc_frame(&e, frame, (size_t)len, enc, sizeof(enc));
        if (n < 0) {
          equal = 0;

          continue;
        }

        term_feed(&shown, enc, (size_t)n);
        direct(&want, frame, (size_t)len);
        checked++;
        if (!term_equal(&shown, &want)) {
          equal = 0;
        }
        if (f == 5) {
          saved += len - n;
        }
      }

      netenc_free(&e);
    }
  }

  CHECK(checked == 5 * 3 * 6, "every renderer frame was encoded");
  CHECK(equal, "real renderer output round-trips in all modes");
  CHECK(saved > 0, "and the delta is smaller than the full frame");
}

int main(void) {
  test_lossless_modes();
  test_dense_mono_not_worse();
  test_origin_and_overlay();
  test_hysteresis();
  test_budget();
  test_invalidate_and_resize();
  test_unsupported_and_limits();
  test_small_output_buffer();
  test_fuzz();
  test_real_renderer();

  char b[96];
  int n = nl_snprintf(b, sizeof(b), "%d passed  %d failed\n", _pass, _fail);
  nl_write(1, b, (size_t)n);

  return _fail ? 1 : 0;
}
