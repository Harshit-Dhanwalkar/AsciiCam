/*
 * tests/test_demo.c - built-in demo picture
 */
#include "../lib/nolibc.h"

#include "demo.h"

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

#define W 160
#define H 96
#define PAD 32
static uint8_t a[(W * 2 + PAD) * H], b[(W * 2 + PAD) * H];

static long diff(const uint8_t *x, const uint8_t *y, int stride) {
  long d = 0;
  for (int r = 0; r < H; r++) {
    for (int c = 0; c < W * 2; c += 2) { // luma only
      int e = (int)x[r * stride + c] - (int)y[r * stride + c];
      d += e < 0 ? -e : e;
    }
  }

  return d;
}

int main(void) {
  CHECK(demo_is_device("demo") && !demo_is_device("/dev/video0") &&
            !demo_is_device(NULL) && !demo_is_device("demo2") &&
            !demo_is_device(""),
        "device name matching");

  nl_memset(a, 0xEE, sizeof(a));
  demo_fill_yuyv(a, W, H, W * 2, 0);

  // legal studio-range luma everywhere, chroma near neutral
  int lo = 255, hi = 0, bad = 0;
  for (int r = 0; r < H; r++) {
    for (int c = 0; c < W * 2; c += 2) {
      int y = a[r * W * 2 + c];
      lo = y < lo ? y : lo;
      hi = y > hi ? y : hi;
      if (y < 16 || y > 235) {
        bad++;
      }
    }
  }

  CHECK(bad == 0, "%d luma values outside 16..235", bad);
  CHECK(hi - lo > 100, "picture has contrast (%d..%d)", lo, hi);

  // deterministic, and it moves, and it loops
  demo_fill_yuyv(b, W, H, W * 2, 0);
  CHECK(diff(a, b, W * 2) == 0, "same frame number gives same picture");
  demo_fill_yuyv(b, W, H, W * 2, 1);
  CHECK(diff(a, b, W * 2) > 1000, "consecutive frames differ");
  demo_fill_yuyv(b, W, H, W * 2, DEMO_PERIOD);
  demo_fill_yuyv(a, W, H, W * 2, 0);
  CHECK(diff(a, b, W * 2) == 0, "loops every DEMO_PERIOD frames");

  // padded rows: pixels identical, padding untouched
  nl_memset(b, 0xEE, sizeof(b));
  demo_fill_yuyv(a, W, H, W * 2, 5);
  demo_fill_yuyv(b, W, H, W * 2 + PAD, 5);
  int same = 1, pad_ok = 1;
  for (int r = 0; r < H; r++) {
    for (int c = 0; c < W * 2; c++) {
      same = same && a[r * W * 2 + c] == b[r * (W * 2 + PAD) + c];
    }

    for (int c = W * 2; c < W * 2 + PAD; c++) {
      pad_ok = pad_ok && b[r * (W * 2 + PAD) + c] == 0xEE;
    }
  }

  CHECK(same, "stride does not change picture");
  CHECK(pad_ok, "row padding is never written");

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
