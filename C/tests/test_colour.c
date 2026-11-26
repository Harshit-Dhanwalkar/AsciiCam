/*
 * tests/test_colour.c - mirrored colour conversion (yuyv_to_rgb_mirror)
 *
 * With -f only luma plane is flipped
 * Converting it with camera's unflipped chroma (yuyv_to_rgb_ex) gives colours
 * that do not follow  shapes; yuyv_to_rgb_mirror reads U/V from opposite end of
 * row, so result equals a true mirror of normal conversion
 */

#include "../lib/nolibc.h"

#include "ascii.h"

static int _pass = 0;
static int _fail = 0;

static void check(int ok, const char *what, int line) {
  if (ok) {
    _pass++;

    return;
  }

  _fail++;
  char buf[200];
  int n = nl_snprintf(buf, sizeof(buf), "FAIL [%d] %s\n", line, what);
  nl_write(1, buf, (size_t)n);
}

#define CHECK(c, what) check((c) != 0, (what), __LINE__)

#define W 16
#define H 3
#define STRIDE 48 // padded rows

static uint8_t yuyv[H * STRIDE];
static uint8_t luma[W * H];
static uint8_t flipped[W * H];
static uint8_t rgb_plain[W * H * 3];
static uint8_t rgb_mirror[W * H * 3];
static uint8_t rgb_flip_after[W * H * 3];
static uint8_t rgb_naive[W * H * 3];

// True when every ESC [ in buf[0..n) is closed by a final byte (0x40..0x7E)
// before the buffer ends: a cut-off escape would swallow what the terminal
// shows next
static int escapes_complete(const char *buf, int n) {
  for (int i = 0; i < n; i++) {
    if (buf[i] != 0x1b) {
      continue;
    }

    int j = i + 2;
    while (j < n && (unsigned char)buf[j] < 0x40) {
      j++;
    }
    if (j >= n) {
      return 0;
    }
    i = j;
  }

  return 1;
}

int main(void) {
  nl_memset(yuyv, 0, sizeof(yuyv));
  // every pair gets its own colour and brightness
  for (int y = 0; y < H; y++) {
    for (int i = 0; i < W / 2; i++) {
      uint8_t *q = yuyv + y * STRIDE + i * 4;
      q[0] = (uint8_t)(40 + i * 15 + y * 5);
      q[1] = (uint8_t)(60 + i * 20); // U
      q[2] = (uint8_t)(50 + i * 15 + y * 5 + 7);
      q[3] = (uint8_t)(220 - i * 22); // V
    }
  }

  // luma of picture, then same flipped
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      luma[y * W + x] = yuyv[y * STRIDE + (x / 2) * 4 + (x % 2) * 2];
      flipped[y * W + (W - 1 - x)] = luma[y * W + x];
    }
  }

  yuyv_to_rgb_ex(yuyv, STRIDE, luma, rgb_plain, W, H);
  yuyv_to_rgb_mirror(yuyv, STRIDE, flipped, rgb_mirror, W, H);

  // Reference: mirror normal picture pixel by pixel
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      for (int c = 0; c < 3; c++) {
        rgb_flip_after[(y * W + x) * 3 + c] =
            rgb_plain[(y * W + (W - 1 - x)) * 3 + c];
      }
    }
  }

  int same = 1;
  for (size_t i = 0; i < sizeof(rgb_mirror); i++) {
    if (rgb_mirror[i] != rgb_flip_after[i]) {
      same = 0;
    }
  }

  CHECK(same, "mirror conversion equals a true mirror of the normal picture");

  // Old path: flipped luma with unflipped chroma disagrees
  yuyv_to_rgb_ex(yuyv, STRIDE, flipped, rgb_naive, W, H);
  int differ = 0;
  for (size_t i = 0; i < sizeof(rgb_naive); i++) {
    if (rgb_naive[i] != rgb_flip_after[i]) {
      differ++;
    }
  }

  CHECK(differ > W * H, "unflipped chroma really is wrong (negative control)");

  // Leftmost pixel after mirroring has colour of rightmost before
  CHECK(rgb_mirror[0] == rgb_plain[(W - 1) * 3] &&
            rgb_mirror[2] == rgb_plain[(W - 1) * 3 + 2],
        "left edge takes the colour of the old right edge");

  // odd width: no overrun, last column handled
  static uint8_t odd_y[5 * 2 * 2 + 8];
  static uint8_t odd_l[5 * 2];
  static uint8_t odd_rgb[5 * 2 * 3 + 16];
  nl_memset(odd_y, 128, sizeof(odd_y));
  nl_memset(odd_l, 100, sizeof(odd_l));
  nl_memset(odd_rgb, 0xA5, sizeof(odd_rgb));
  yuyv_to_rgb_mirror(odd_y, 5 * 2, odd_l, odd_rgb, 5, 2);
  int guard = 1;
  for (size_t i = 5 * 2 * 3; i < sizeof(odd_rgb); i++) {
    if (odd_rgb[i] != 0xA5) {
      guard = 0;
    }
  }

  CHECK(guard, "odd width writes nothing past the picture");

  // Undersized output buffer: cells that do not fit are dropped whole, never
  // written as a cut-off colour escape (every mode, both colour depths)
  {
    enum { GW = 48, GH = 32, CW = 20, CH = 12 };
    static uint8_t g[GW * GH], c[GW * GH * 3];
    static char o[1 << 16];
    for (int i = 0; i < GW * GH; i++) {
      g[i] = (uint8_t)(i * 37);
    }
    for (int i = 0; i < GW * GH * 3; i++) {
      c[i] = (uint8_t)(i * 91 + 13);
    }

    int bad = 0, cut_seen = 0;
    for (int mode = 0; mode < RENDER_MODE_COUNT; mode++) {
      for (int cm = 0; cm < 2; cm++) {
        for (size_t sz = 24; sz < 700; sz++) {
          ascii_opts_t opts;
          nl_memset(&opts, 0, sizeof(opts));
          opts.contrast = 100;
          opts.gamma = 100;
          opts.color = 1;
          opts.color_mode = (color_mode_t)cm;
          opts.threshold_val = 128;
          opts.render_mode = (render_mode_t)mode;
          int n = grayscale_to_ascii(g, c, GW, GH, CW, CH, o, sz, &opts);
          if (n < 0 || (size_t)n > sz || !escapes_complete(o, n)) {
            bad++;
          }
          cut_seen += (n > 0 && (size_t)n < sz);
        }
      }
    }

    CHECK(bad == 0, "tiny buffers never end inside an escape sequence");
    CHECK(cut_seen > 0, "tiny buffers really are cut short (control)");
    // Negative control: checker itself does flag a cut-off escape
    const char *cut = "ab\033[38;2;12";
    const char *whole = "ab\033[38;2;12m";
    CHECK(!escapes_complete(cut, (int)nl_strlen(cut)),
          "checker flags a cut escape");
    CHECK(escapes_complete(whole, (int)nl_strlen(whole)),
          "checker accepts a closed escape");
  }

  char buf[96];
  int n = nl_snprintf(buf, sizeof(buf), "colour: %d passed, %d failed\n", _pass,
                      _fail);
  nl_write(1, buf, (size_t)n);

  return _fail ? 1 : 0;
}
