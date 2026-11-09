/*
 * tests/test_vcam.c - src/vcam.c: ANSI frame to pixels (glyphs from font,
 * braille dots, block elements, SGR colours, cursor moves, centring and
 * cropping), RGB to YUYV, raw output to a file, and garbage input
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_vcam.c src/vcam.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c -Iinclude -Ilib -o /tmp/tv
 */

#include "platform.h"

#if !defined(__LINUX_NOLIBC__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include "../lib/nolibc.h"
#include "vcam.h"

#include "../src/vcam_font.inc"

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

#define W 1280
#define H 720

static vcam_t vc;

static void draw(const char *s) { vcam_render(&vc, s, nl_strlen(s)); }

// Pixel (x,y) of cell (cx,cy)
static const uint8_t *px(int cx, int cy, int x, int y) {
  return vcam_pixels(&vc) +
         ((size_t)(cy * VCAM_CELL_H + y) * W + (size_t)(cx * VCAM_CELL_W + x)) *
             3;
}

static int is_rgb(const uint8_t *p, int r, int g, int b) {
  return p[0] == r && p[1] == g && p[2] == b;
}

// A single glyph is centred: cell (79,22) on 160x45
#define CX 79
#define CY 22

static int cell_lit(int cx, int cy) {
  int n = 0;
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 8; x++) {
      const uint8_t *p = px(cx, cy, x, y);
      if (p[0] || p[1] || p[2]) {
        n++;
      }
    }
  }

  return n;
}

int main(void) {
  CHECK(vcam_init(&vc, 7, 100) < 0, "canvas smaller than a cell refused");
  CHECK(vcam_init(&vc, 1281, 720) < 0, "odd width refused");
  CHECK(vcam_init(&vc, W, H) == 0, "1280x720 canvas");
  CHECK(vc.cols == 160 && vc.rows == 45, "grid %dx%d", vc.cols, vc.rows);

  // Font: every ASCII glyph shows exactly pixels of table
  for (int c = 33; c < 127; c++) {
    char s[8] = "\x1b[H";
    s[3] = (char)c;
    s[4] = 0;
    draw(s);
    int ok = 1, pop = 0;
    for (int y = 0; y < 16; y++) {
      for (int x = 0; x < 8; x++) {
        int want = (vcam_font[c - 32][y] >> (7 - x)) & 1;
        const uint8_t *p = px(CX, CY, x, y);
        pop += want;
        if ((p[0] != 0) != want) {
          ok = 0;
        }
      }
    }

    CHECK(ok && pop > 0, "glyph '%c' matches its font bitmap", c);
  }

  draw("\x1b[H ");
  CHECK(cell_lit(CX, CY) == 0, "space draws nothing");
  CHECK(vc.unknown == 0, "no unknown glyphs so far");

  // Colour: truecolor fg and bg on a full block, then 16 and 256 palettes
  draw("\x1b[38;2;10;200;30m\x1b[48;2;1;2;3m\xe2\x96\x88");
  CHECK(is_rgb(px(CX, CY, 0, 0), 10, 200, 30) &&
            is_rgb(px(CX, CY, 7, 15), 10, 200, 30),
        "full block is the foreground colour");
  draw("\x1b[38;2;10;200;30m\x1b[48;2;1;2;3m ");
  CHECK(is_rgb(px(CX, CY, 3, 3), 1, 2, 3), "space shows the background");
  draw("\x1b[41m ");
  CHECK(is_rgb(px(CX, CY, 3, 3), 205, 49, 49), "SGR 41 is palette red");
  draw("\x1b[101m ");
  CHECK(is_rgb(px(CX, CY, 3, 3), 241, 76, 76), "SGR 101 is bright red");
  draw("\x1b[48;5;196m ");
  CHECK(is_rgb(px(CX, CY, 3, 3), 255, 0, 0), "256-colour 196 is red");
  draw("\x1b[48;5;240m ");
  CHECK(is_rgb(px(CX, CY, 3, 3), 88, 88, 88), "256-colour grey ramp");
  draw("\x1b[31;42m\x1b[7m\xe2\x96\x88");
  CHECK(is_rgb(px(CX, CY, 3, 3), 13, 188, 121), "reverse swaps colours");
  draw("\x1b[31m\x1b[0m\xe2\x96\x88");
  CHECK(is_rgb(px(CX, CY, 3, 3), 204, 204, 204), "SGR 0 resets to default");
  draw("\x1b[38;2;9;9m\x1b[38;5m\xe2\x96\x88");
  CHECK(is_rgb(px(CX, CY, 3, 3), 204, 204, 204), "truncated colour ignored");

  // Block elements
  draw("\x1b[38;2;200;100;50m\xe2\x96\x80"); // upper half
  CHECK(is_rgb(px(CX, CY, 4, 7), 200, 100, 50) &&
            is_rgb(px(CX, CY, 4, 8), 0, 0, 0),
        "upper half block");
  draw("\x1b[38;2;200;100;50m\xe2\x96\x84"); // lower half
  CHECK(is_rgb(px(CX, CY, 4, 7), 0, 0, 0) &&
            is_rgb(px(CX, CY, 4, 8), 200, 100, 50),
        "lower half block");
  draw("\x1b[38;2;200;100;50m\xe2\x96\x8c"); // left half
  CHECK(is_rgb(px(CX, CY, 3, 5), 200, 100, 50) &&
            is_rgb(px(CX, CY, 4, 5), 0, 0, 0),
        "left half block");
  draw("\x1b[38;2;200;100;50m\xe2\x96\x90"); // right half
  CHECK(is_rgb(px(CX, CY, 3, 5), 0, 0, 0) &&
            is_rgb(px(CX, CY, 4, 5), 200, 100, 50),
        "right half block");
  draw("\x1b[38;2;200;200;200m\xe2\x96\x92"); // medium shade
  CHECK(px(CX, CY, 2, 2)[0] >= 99 && px(CX, CY, 2, 2)[0] <= 101,
        "medium shade is half-way (%d)", px(CX, CY, 2, 2)[0]);
  draw("\x1b[38;2;200;200;200m\xe2\x96\x9a"); // quadrants UL + LR
  CHECK(px(CX, CY, 1, 1)[0] == 200 && px(CX, CY, 6, 1)[0] == 0 &&
            px(CX, CY, 1, 14)[0] == 0 && px(CX, CY, 6, 14)[0] == 200,
        "quadrant UL+LR");
  draw("\x1b[38;2;200;200;200m\xe2\x96\x97"); // LR
  CHECK(px(CX, CY, 1, 1)[0] == 0 && px(CX, CY, 6, 14)[0] == 200, "quadrant LR");
  CHECK(vc.unknown == 0, "block elements are all known");

  // Braille: all eight dots, then dot 1 alone, dot 8 alone
  draw("\x1b[38;2;255;255;255m\xe2\xa3\xbf");
  CHECK(cell_lit(CX, CY) == 8 * 9 && px(CX, CY, 0, 0)[0] == 255 &&
            px(CX, CY, 3, 0)[0] == 0 && px(CX, CY, 4, 12)[0] == 255,
        "8-dot braille: 8 dots of 3x3 (%d lit)", cell_lit(CX, CY));
  draw("\x1b[38;2;255;255;255m\xe2\xa0\x81");
  CHECK(cell_lit(CX, CY) == 9 && px(CX, CY, 1, 1)[0] == 255,
        "dot 1 is top left");
  draw("\x1b[38;2;255;255;255m\xe2\xa2\x80");
  CHECK(cell_lit(CX, CY) == 9 && px(CX, CY, 5, 13)[0] == 255,
        "dot 8 is bottom right");
  draw("\x1b[38;2;255;255;255m\xe2\xa0\x84");
  CHECK(cell_lit(CX, CY) == 9 && px(CX, CY, 1, 9)[0] == 255,
        "dot 3 is left column, third row");

  // Dots charset: three sizes of round dot, all known
  draw("\x1b[38;2;255;255;255m\xc2\xb7");
  int n1 = cell_lit(CX, CY);
  draw("\x1b[38;2;255;255;255m\xe2\x80\xa2");
  int n2 = cell_lit(CX, CY);
  draw("\x1b[38;2;255;255;255m\xe2\x97\x8f");
  int n3 = cell_lit(CX, CY);
  CHECK(n1 > 0 && n1 < n2 && n2 < n3 && px(CX, CY, 4, 8)[0] == 255 &&
            px(CX, CY, 0, 0)[0] == 0,
        "dot, bullet, circle grow (%d %d %d)", n1, n2, n3);
  CHECK(vc.unknown == 0, "the dots charset glyphs are known");

  // Unknown glyph: hollow box, counted
  draw("\xc3\xa9");
  CHECK(vc.unknown == 1 && cell_lit(CX, CY) > 0, "unknown glyph is a box");
  CHECK(px(CX, CY, 4, 8)[0] == 0, "box is hollow");

  // Cursor moves: two blocks one cell right and one down are kept 1,1 apart
  // and pair is centred (3x2 cells: first at col (160-3)/2 = 78, row (45-2)/2 = 21)
  draw("\x1b[38;2;255;0;0m\x1b[5;9H\xe2\x96\x88\x1b[6;11H\xe2\x96\x88");
  CHECK(cell_lit(78, 21) == 128 && cell_lit(80, 22) == 128 &&
            cell_lit(79, 21) == 0 && cell_lit(79, 22) == 0,
        "explicit cursor positions keep their distance and are centred");

  // Rows with \r\n; origin move at start is not part of picture
  draw("\x1b[7;30H\xe2\x96\x88\xe2\x96\x88\x1b[8;30H\xe2\x96\x88\xe2\x96\x88");
  CHECK(cell_lit(79, 21) == 128 && cell_lit(80, 21) == 128 &&
            cell_lit(79, 22) == 128 && cell_lit(80, 22) == 128,
        "2x2 block from absolute moves, origin not part of the picture");
  draw("\x1b[H\xe2\x96\x88\r\n\xe2\x96\x88");
  CHECK(cell_lit(CX, 21) == 128 && cell_lit(CX, 22) == 128, "two rows");
  draw("\x1b[H\xe2\x96\x88\xe2\x96\x88\r\n\xe2\x96\x88\xe2\x96\x88");
  CHECK(cell_lit(79, 21) == 128 && cell_lit(80, 22) == 128,
        "CRLF returns to column 0");
  draw("\x1b[H\xe2\x96\x88\xe2\x96\x88\n\xe2\x96\x88\xe2\x96\x88");
  CHECK(cell_lit(79, 21) == 128 && cell_lit(80, 22) == 128 &&
            cell_lit(79, 22) == 128,
        "a bare LF also returns to column 0, as tty does");

  // Clear screen
  draw("\x1b[Hab\x1b[2J");
  int any = 0;
  for (int i = 0; i < W * H * 3; i++) {
    any |= vcam_pixels(&vc)[i];
  }

  CHECK(!any, "ESC[2J clears the picture");

  // Frame larger than canvas is cropped at top left
  draw("\x1b[H\x1b[38;2;255;255;255m"
       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
       "AAAAAAAAAAAAAAA"
       "\r\nB");
  CHECK(cell_lit(0, 21) > 0 && cell_lit(159, 21) > 0 && cell_lit(0, 22) > 0,
        "wide frame fills from the left edge");
  vcam_close(&vc);

  // 1x1-cell canvas: only top left cell of a bigger frame
  vcam_t sm;
  CHECK(vcam_init(&sm, 8, 16) == 0, "tiny canvas");
  const char big[] = "\x1b[HXY\r\nZ";
  vcam_render(&sm, big, sizeof(big) - 1);
  int lit = 0;
  for (int i = 0; i < 8 * 16 * 3; i++) {
    lit += vcam_pixels(&sm)[i] != 0;
  }

  CHECK(lit > 0, "tiny canvas shows the first glyph");
  vcam_close(&sm);

  // YUYV: white, black, red
  vcam_t y;
  CHECK(vcam_init(&y, 16, 16) == 0, "yuyv canvas");
  {
    const char *wh = "\x1b[H\x1b[38;2;255;255;255m\xe2\x96\x88";
    vcam_render(&y, wh, nl_strlen(wh));
  }
  vcam_to_yuyv(&y);
  // Cells are 8 px: cols 0..1 of canvas hold block at cell (0,0)
  CHECK(y.yuyv[0] == 235 && y.yuyv[1] >= 127 && y.yuyv[1] <= 129 &&
            y.yuyv[3] >= 127 && y.yuyv[3] <= 129,
        "white = Y235 U128 V128 (%d %d %d)", y.yuyv[0], y.yuyv[1], y.yuyv[3]);
  CHECK(y.yuyv[(size_t)16 * 2 - 4] == 16, "black = Y16 (%d)",
        y.yuyv[16 * 2 - 4]);
  {
    const char *rd = "\x1b[H\x1b[38;2;255;0;0m\xe2\x96\x88";
    vcam_render(&y, rd, nl_strlen(rd));
  }
  vcam_to_yuyv(&y);
  CHECK(y.yuyv[0] >= 81 && y.yuyv[0] <= 83 && y.yuyv[1] >= 89 &&
            y.yuyv[1] <= 91 && y.yuyv[3] >= 239 && y.yuyv[3] <= 241,
        "red = Y82 U90 V240 (%d %d %d)", y.yuyv[0], y.yuyv[1], y.yuyv[3]);
  vcam_close(&y);

  // raw output to a file: one full YUYV frame per call
  vcam_t f;
  CHECK(vcam_init(&f, 64, 32) == 0, "file canvas");
  CHECK(vcam_open(&f, "/nonexistent-dir/vcam.yuv") < 0, "bad path refused");
  const char *path = "build/tests/vcam_tmp.yuv";
  CHECK(vcam_open(&f, path) == 0, "open file");
  CHECK(!f.is_video, "a regular file is not a video device");
  CHECK(vcam_frame(&f, "\x1b[Hhi", 5) == 0, "frame 1");
  CHECK(vcam_frame(&f, "\x1b[Hhi", 5) == 0, "frame 2");
  CHECK(f.frames == 2, "frame counter");
  vcam_close(&f);
  {
    int fd = open(path, O_RDONLY);
    long total = 0;
    char buf[4096];
    ssize_t n;
    while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0) {
      total += n;
    }

    if (fd >= 0) {
      close(fd);
    }

    CHECK(total == 2L * 64 * 32 * 2, "two frames of 64x32 YUYV (%ld bytes)",
          total);
    unlink(path);
  }

  // garbage and truncated sequences never crash
  vcam_t g;
  CHECK(vcam_init(&g, 64, 32) == 0, "fuzz canvas");
  uint32_t s = 12345;
  char junk[600];
  for (int round = 0; round < 3000; round++) {
    size_t n = (size_t)(s % 580) + 1;
    for (size_t i = 0; i < n; i++) {
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      uint32_t r = s & 15;
      // bias towards bytes that matter
      junk[i] = r == 0   ? 0x1b
                : r == 1 ? '['
                : r == 2 ? ';'
                : r == 3 ? 'm'
                : r == 4 ? 'H'
                : r == 5 ? (char)0xe2
                : r == 6 ? '9'
                         : (char)(s >> 8);
    }

    vcam_render(&g, junk, n);
  }

  CHECK(1, "3000 random frames survived");
  vcam_close(&g);

  fprintf(stderr, "%d passed %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
