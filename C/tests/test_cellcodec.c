/*
 * tests/test_cellcodec.c - src/cellcodec.c (links src/netenc.c): ANSI to
 * cells, key and delta frames both ways, damaged and hostile input, drawing
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_cellcodec.c src/cellcodec.c src/netenc.c \
 *     lib/nl_*.c -Iinclude -Ilib -o /tmp/tc
 */

#include "../lib/nolibc.h"
#include "cellcodec.h"
#include "platform.h"

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

static uint32_t rng = 2463534242u;

static unsigned rnd(unsigned n) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;

  return rng % n;
}

static const uint32_t glyphs[] = {' ',     '.',       '#',        '@',
                                  0xA9C3u, 0x96B8E2u, 0x80989FF0u};

static void fill_random(cc_grid_t *g, int colour, int unicode) {
  for (int i = 0; i < g->cols * g->rows; i++) {
    g->cell[i].glyph = glyphs[rnd(unicode ? 7 : 4)];
    g->cell[i].fg = colour && rnd(2) ? CC_RGB(rnd(256), rnd(256), rnd(256)) : 0;
    g->cell[i].bg =
        colour && rnd(3) == 0 ? CC_RGB(rnd(256), rnd(256), rnd(256)) : 0;
  }
}

static int same(const cc_grid_t *a, const cc_grid_t *b) {
  if (a->cols != b->cols || a->rows != b->rows) {
    return 0;
  }

  for (int i = 0; i < a->cols * a->rows; i++) {
    if (a->cell[i].glyph != b->cell[i].glyph ||
        a->cell[i].fg != b->cell[i].fg || a->cell[i].bg != b->cell[i].bg) {
      return 0;
    }
  }

  return 1;
}

static void copy_grid(cc_grid_t *dst, const cc_grid_t *src) {
  cc_grid_size(dst, src->cols, src->rows);
  nl_memcpy(dst->cell, src->cell,
            (size_t)src->cols * (size_t)src->rows * sizeof(netcell_t));
}

int main(void) {
  static uint8_t buf[1 << 18];
  cc_grid_t a = {0}, b = {0}, st = {0};

  // Sizes
  CHECK(cc_grid_size(&a, 0, 5) != 0 && cc_grid_size(&a, 5, 0) != 0 &&
            cc_grid_size(&a, CC_MAX_COLS + 1, 5) != 0 &&
            cc_grid_size(&a, 5, CC_MAX_ROWS + 1) != 0,
        "bad sizes refused");
  CHECK(cc_grid_size(&a, 12, 5) == 0 && a.cols == 12 && a.rows == 5 &&
            a.cell[0].glyph == ' ' && a.cell[59].glyph == ' ',
        "grid starts blank");

  // From ANSI
  const char *ansi = "\033[1;1H\033[38;2;10;20;30mAB\033[0m\n\033[3;4H\033[48;"
                     "2;1;2;3m\xC3\xA9z";
  CHECK(cc_grid_from_ansi(&a, ansi, nl_strlen(ansi)) == 0, "parses");
  CHECK(a.cols == 5 && a.rows == 3, "extent %dx%d", a.cols, a.rows);
  CHECK(a.cell[0].glyph == 'A' && a.cell[0].fg == CC_RGB(10, 20, 30) &&
            a.cell[1].glyph == 'B' && a.cell[1].fg == CC_RGB(10, 20, 30),
        "colours and glyphs");
  CHECK(a.cell[2 * 5 + 3].glyph == 0xA9C3u &&
            a.cell[2 * 5 + 3].bg == CC_RGB(1, 2, 3) &&
            a.cell[2 * 5 + 4].glyph == 'z',
        "unicode glyph, background");
  CHECK(a.cell[7].glyph == ' ' && a.cell[7].fg == 0, "unwritten cell is blank");
  const char *pal = "\033[38;5;100mX";
  CHECK(cc_grid_from_ansi(&a, pal, nl_strlen(pal)) == -1,
        "palette colour refused");
  const char *junk = "\033[2JX";
  CHECK(cc_grid_from_ansi(&a, junk, nl_strlen(junk)) == -1,
        "unsupported escape");
  CHECK(cc_grid_from_ansi(&a, "", 0) == -1, "nothing drawn");
  char wide[300];
  nl_memset(wide, 'x', sizeof(wide));
  CHECK(cc_grid_from_ansi(&a, wide, sizeof(wide)) == -1, "too wide");

  // Key round trip: mono, colour, unicode
  for (int mode = 0; mode < 3; mode++) {
    cc_grid_size(&a, 60, 30);
    fill_random(&a, mode >= 1, mode == 2);
    size_t n = cc_encode(&a, NULL, 1, buf, sizeof(buf));
    CHECK(n > 0 && n <= cc_max_encoded(60, 30), "mode %d key encodes", mode);
    CHECK(cc_is_key(buf, n) == 1, "key flag");
    CHECK(cc_decode(&st, buf, n) == 0 && same(&a, &st),
          "mode %d key round trip", mode);
    if (mode == 0) {
      CHECK(n < 60 * 30 * 2 + 16 && (buf[0] / 2u) % 2u == 1u,
            "mono frame: about one byte a cell (%zu)", n);
    }
  }

  size_t n;
  // A frame whose only colour is a background is not "no colour"
  cc_grid_size(&a, 8, 3);
  a.cell[10].bg = CC_RGB(9, 8, 7);
  n = cc_encode(&a, NULL, 1, buf, sizeof(buf));
  CHECK(cc_decode(&st, buf, n) == 0 && same(&a, &st) &&
            (buf[0] / 2u) % 2u == 0u,
        "background-only colour survives");
  cc_grid_size(&a, 8, 3);
  a.cell[4].glyph = 0x80989FF0u;
  n = cc_encode(&a, NULL, 1, buf, sizeof(buf));
  CHECK(cc_decode(&st, buf, n) == 0 && same(&a, &st),
        "4-byte glyph round trip");

  // Delta chain
  cc_grid_size(&a, 80, 40);
  fill_random(&a, 1, 1);
  cc_grid_t prev = {0};
  n = cc_encode(&a, NULL, 1, buf, sizeof(buf));
  cc_decode(&st, buf, n);
  int chain_ok = 1;
  int deltas_smaller = 1;
  for (int f = 0; f < 200; f++) {
    copy_grid(&prev, &a);
    int changes = (int)rnd(f % 10 == 0 ? 3200 : 60);
    for (int k = 0; k < changes; k++) {
      int i = (int)rnd(3200);
      a.cell[i].glyph = glyphs[rnd(7)];
      a.cell[i].fg = rnd(2) ? CC_RGB(rnd(256), rnd(256), rnd(256)) : 0;
      a.cell[i].bg = rnd(4) == 0 ? CC_RGB(rnd(256), rnd(256), rnd(256)) : 0;
    }

    n = cc_encode(&a, &prev, 0, buf, sizeof(buf));
    chain_ok = chain_ok && n > 0 && cc_is_key(buf, n) == 0 &&
               cc_decode(&st, buf, n) == 0 && same(&a, &st);
    if (changes < 60) {
      deltas_smaller = deltas_smaller && n < 3200 * 2;
    }
  }

  CHECK(chain_ok, "200 random deltas keep the receiver exact");
  CHECK(deltas_smaller, "small changes make small frames");
  copy_grid(&prev, &a);
  n = cc_encode(&a, &prev, 0, buf, sizeof(buf));
  CHECK(n > 0 && n < 16 && cc_decode(&st, buf, n) == 0 && same(&a, &st),
        "nothing changed: a few bytes (%zu)", n);

  // Delta against a different size becomes a key
  cc_grid_size(&b, 20, 10);
  fill_random(&b, 0, 0);
  n = cc_encode(&b, &a, 0, buf, sizeof(buf));
  CHECK(n > 0 && cc_is_key(buf, n) == 1, "size change: key");
  CHECK(cc_decode(&st, buf, n) == 0 && same(&b, &st) && st.cols == 20,
        "receiver takes the new size");

  cc_grid_size(&b, 20, 11);
  cc_grid_size(&a, 20, 10);
  n = cc_encode(&b, &a, 0, buf, sizeof(buf));
  CHECK(n > 0 && cc_is_key(buf, n) == 1, "only the rows differ: still a key");
  cc_grid_size(&b, 21, 10);
  n = cc_encode(&b, &a, 0, buf, sizeof(buf));
  CHECK(n > 0 && cc_is_key(buf, n) == 1,
        "only the columns differ: still a key");

  // Deltas receiver cannot apply
  cc_grid_size(&a, 20, 10);
  copy_grid(&prev, &a);
  a.cell[5].glyph = '#';
  n = cc_encode(&a, &prev, 0, buf, sizeof(buf));
  cc_grid_t other = {0};
  CHECK(cc_decode(&other, buf, n) == -2,
        "delta with no baseline: wait for a key");
  cc_grid_size(&other, 21, 10);
  CHECK(cc_decode(&other, buf, n) == -2,
        "delta for another size: wait for a key");

  // Too small an output buffer
  CHECK(cc_encode(&a, NULL, 1, buf, 30) == 0, "does not fit: 0");

  // Damaged input: never a crash, a refused frame leaves state alone
  cc_grid_size(&a, 30, 12);
  fill_random(&a, 1, 1);
  n = cc_encode(&a, NULL, 1, buf, sizeof(buf));
  cc_grid_t good = {0};
  CHECK(cc_decode(&good, buf, n) == 0, "good copy");
  int trunc_refused = 1;
  for (size_t cut = 0; cut < n; cut += 1 + n / 200) {
    cc_grid_t t = {0};
    copy_grid(&t, &good);
    int rc = cc_decode(&t, buf, cut);
    trunc_refused = trunc_refused && rc != 0 && same(&t, &good);
    cc_grid_free(&t);
  }

  CHECK(trunc_refused, "every truncation refused, state kept");
  uint8_t more[4096];
  nl_memcpy(more, buf, 100);
  CHECK(cc_decode(&good, buf, n) == 0, "again fine");
  nl_memcpy(buf + n, "xyz", 3);
  CHECK(cc_decode(&good, buf, n + 3) == -1, "trailing bytes refused");
  uint8_t hdr[8] = {0x80, 0, 5, 0, 5, ' ', ' ', ' '};
  CHECK(cc_decode(&good, hdr, 8) == -1, "unknown flag bits refused");
  uint8_t zero[8] = {1, 0, 0, 0, 5, ' ', ' ', ' '};
  CHECK(cc_decode(&good, zero, 8) == -1 && cc_is_key(zero, 8) == -1,
        "zero columns refused");
  uint8_t huge[8] = {1, 9, 9, 9, 9, ' ', ' ', ' '};
  CHECK(cc_decode(&good, huge, 8) == -1, "huge size refused");
  uint8_t ctl[9] = {3, 0, 1, 0, 1, 0x1B, 0, 0, 0};
  CHECK(cc_decode(&good, ctl, 6) == -1, "control character as glyph refused");
  uint8_t badutf[9] = {3, 0, 1, 0, 1, 0xC3, 0x41, 0, 0};
  CHECK(cc_decode(&good, badutf, 7) == -1, "broken UTF-8 refused");
  uint8_t over[9] = {3, 0, 1, 0, 1, 0xF5, 0x80, 0x80, 0x80};
  CHECK(cc_decode(&good, over, 9) == -1, "UTF-8 beyond U+10FFFF refused");
  uint8_t noprog[10] = {2, 0, 2, 0, 2, 0, 0, 0, 0, 0};
  copy_grid(&other, &good);
  cc_grid_size(&other, 2, 2);
  CHECK(cc_decode(&other, noprog, 7) == -1,
        "a pair that makes no progress is refused");
  uint8_t past[10] = {2, 0, 2, 0, 2, 9, 1, 'x', 0, 0};
  CHECK(cc_decode(&other, past, 8) == -1, "skip past the end refused");
  uint8_t runpast[10] = {2, 0, 2, 0, 2, 3, 2, 'x', 'y', 0};
  CHECK(cc_decode(&other, runpast, 9) == -1, "run past the end refused");
  uint8_t cvar[12] = {2, 0, 2, 0, 2, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0, 0};
  CHECK(cc_decode(&other, cvar, 12) == -1, "oversized varint refused");

  // Fuzz: random bytes and random damage never crash, state stays sane
  int fuzz_ok = 1;
  for (int it = 0; it < 4000; it++) {
    uint8_t f[64];
    size_t fl = 5 + rnd(59);
    for (size_t i = 0; i < fl; i++) {
      f[i] = (uint8_t)rnd(256);
    }

    f[0] = (uint8_t)rnd(4);
    f[1] = 0;
    f[2] = (uint8_t)(1 + rnd(6));
    f[3] = 0;
    f[4] = (uint8_t)(1 + rnd(4));
    cc_grid_t t = {0};
    cc_grid_size(&t, f[2], f[4]);
    int rc = cc_decode(&t, f, fl);
    fuzz_ok = fuzz_ok && (rc == 0 || rc == -1 || rc == -2) && t.cols >= 1;
    cc_grid_free(&t);
  }

  for (int it = 0; it < 300; it++) {
    n = cc_encode(&a, NULL, 1, buf, sizeof(buf));
    for (int k = 0; k < 1 + (int)rnd(4); k++) {
      buf[rnd((unsigned)n)] ^= (uint8_t)(1u << rnd(8));
    }

    cc_grid_t t = {0};
    int rc = cc_decode(&t, buf, n);
    fuzz_ok = fuzz_ok && (rc == 0 || rc == -1 || rc == -2);
    cc_grid_free(&t);
  }

  CHECK(fuzz_ok, "4300 random and damaged frames: no crash");
  (void)more;

  // Drawing: what cc_draw writes is what grid holds
  cc_grid_size(&a, 30, 12);
  fill_random(&a, 1, 1);
  static char scr[1 << 17];
  size_t dn = cc_draw(&a, 1, 1, 100, 100, scr, sizeof(scr));
  CHECK(dn > 0 && dn <= cc_max_draw(30, 12), "draw fits its bound (%zu)", dn);
  CHECK(cc_grid_from_ansi(&b, scr, dn) == 0 && same(&a, &b),
        "draw then parse gives the same grid");
  dn = cc_draw(&a, 11, 4, 100, 100, scr, sizeof(scr));
  CHECK(dn > 0 && scr[2] == '4' && scr[3] == ';' && scr[4] == '1' &&
            scr[5] == '1',
        "placed at row 4, column 11");
  dn = cc_draw(&a, 1, 1, 10, 5, scr, sizeof(scr));
  CHECK(cc_grid_from_ansi(&b, scr, dn) == 0 && b.cols == 10 && b.rows == 5 &&
            b.cell[2 * 10 + 3].glyph == a.cell[2 * 30 + 3].glyph,
        "clipped to 10 x 5");
  CHECK(cc_draw(&a, 1, 1, 100, 100, scr, 10) == 0, "out of room: 0");
  CHECK(cc_draw(&a, 0, 1, 10, 5, scr, sizeof(scr)) == 0, "column 0 refused");

  // Diff drawing: on a screen that shows `shown`, drawing changes leaves screen
  // showing new grid, and costs less than a whole draw
  int diff_ok = 1;
  int cheaper = 1;
  int saw_move_inside_row = 0;
  for (int round = 0; round < 60; round++) {
    cc_grid_t shown = {0};
    cc_grid_t now = {0};
    cc_grid_t screen = {0};
    int dc = 5 + (int)rnd(30);
    int dr = 2 + (int)rnd(10);
    cc_grid_size(&shown, dc, dr);
    cc_grid_size(&now, dc, dr);
    fill_random(&shown, round % 2, round % 3 == 0);
    for (int i = 0; i < dc * dr; i++) {
      now.cell[i] = shown.cell[i];
    }

    int changes = round % 5 == 0 ? dc * dr : 1 + (int)rnd(6);
    for (int k = 0; k < changes; k++) {
      int at = (int)rnd((unsigned)(dc * dr));
      now.cell[at].glyph = glyphs[rnd(4)];
      now.cell[at].fg = round % 2 && rnd(2) ? CC_RGB(rnd(256), 7, 9) : 0;
      now.cell[at].bg = 0;
    }

    static uint8_t seen[CC_MAX_COLS * CC_MAX_ROWS];
    cc_grid_size(&screen, dc, dr);
    for (int i = 0; i < dc * dr; i++) {
      screen.cell[i] = shown.cell[i];
    }

    size_t full = cc_draw(&now, 1, 1, 100, 100, scr, sizeof(scr));
    size_t dd = cc_draw_diff(&now, &shown, 1, 1, 100, 100, scr + full,
                             sizeof(scr) - full);
    diff_ok = diff_ok && dd > 0 && dd <= cc_max_draw(dc, dr) &&
              netenc_parse(scr + full, dd, screen.cell, seen, dr, dc) == 0 &&
              same(&screen, &now);
    cheaper = cheaper && (changes == dc * dr || dd < full / 2 + 8);
    saw_move_inside_row = saw_move_inside_row || changes < 4;
    cc_grid_free(&shown);
    cc_grid_free(&now);
    cc_grid_free(&screen);
  }

  CHECK(diff_ok, "diff draw leaves the screen showing the new grid");
  CHECK(cheaper && saw_move_inside_row, "a few changes cost a few bytes");
  {
    cc_grid_t x = {0};
    cc_grid_t y = {0};
    cc_grid_size(&x, 10, 4);
    cc_grid_size(&y, 12, 4);
    size_t same_n = cc_draw_diff(&x, &x, 1, 1, 100, 100, scr, sizeof(scr));
    size_t other_n = cc_draw_diff(&x, &y, 1, 1, 100, 100, scr, sizeof(scr));
    size_t full_n = cc_draw(&x, 1, 1, 100, 100, scr, sizeof(scr));
    CHECK(same_n == 4 && other_n == full_n &&
              cc_draw_diff(&x, NULL, 1, 1, 100, 100, scr, sizeof(scr)) ==
                  full_n,
          "nothing changed: no cells; other size or none: a whole draw");
    cc_grid_size(&y, 10, 5);
    CHECK(cc_draw_diff(&x, &y, 1, 1, 100, 100, scr, sizeof(scr)) == full_n,
          "a screen with other rows: a whole draw");
    cc_grid_free(&x);
    cc_grid_free(&y);
  }

  cc_grid_free(&a);
  cc_grid_free(&b);
  cc_grid_free(&st);
  cc_grid_free(&prev);
  cc_grid_free(&good);
  cc_grid_free(&other);

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
