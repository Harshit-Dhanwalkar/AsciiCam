/*
 * tests/test_nl_sumspan.c - lib/nl_sumspan.h: assembly kernel against its C
 * twin and against sums worked out by hand
 *
 *   - every span length 0..140 at every start offset 0..31 (16-, 8- and 1-byte
 *     steps, alignment)
 *   - empty and backwards spans add nothing, acc is added to and not replaced
 *   - nothing outside [xs, xe) is read: row ends at last byte of a mapped page
 *     and page after it is unmapped, so one byte too many is a SIGSEGV
 *     A 0xFF fence around spans catches over-reads that stay inside page
 *   - whole 4096 x 4096 picture in one cell does not overflow
 *   - random rows and spans, many cells per call, including n = 0
 *   - nl_addrow_u8: same sweeps, fenced output, 32-bit exactness
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_nl_sumspan.c lib/nl_mem.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c -Iinclude -Ilib -o /tmp/ts
 */
#define NL_SUMSPAN_IMPL
#include "../lib/nl_sumspan.h"

#include "../lib/nolibc.h"

#ifndef PROT_READ
#define PROT_READ 1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 2
#endif

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

static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;

  return rng_state;
}

// What an honest person would write: one span, byte by byte
static uint64_t naive(const uint8_t *row, int xs, int xe) {
  uint64_t s = 0;
  for (int x = xs; x < xe; x++) {
    s += row[x];
  }

  return s;
}

int main(void) {
  fprintf(stderr, "nl_sumspan: %s\n",
          NL_SUMSPAN_ASM ? "x86-64 assembly" : "C fallback");

  // A row that ends exactly at end of mapped memory (guard page after)
  const size_t page = 4096;
  uint8_t *map = nl_mmap(0, 2 * page, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(map != MAP_FAILED, "mmap");
  if (map == MAP_FAILED) {
    return 1;
  }

  CHECK(nl_munmap(map + page, page) == 0, "unmap guard page");
  uint8_t *row = map; // 4096 bytes, last one is last mapped byte
  for (size_t i = 0; i < page; i++) {
    row[i] = (uint8_t)(rnd() >> 7);
  }

  // Every length at every offset, spans that end on last byte of page
  {
    int bad = 0;
    for (int len = 0; len <= 140; len++) {
      for (int off = 0; off < 32; off++) {
        int xs = (int)page - off - len;
        int xe = xs + len;
        if (xs < 0) {
          continue;
        }

        uint64_t acc = 7; // is added to, not replaced
        nl_sumspan_u8(row, &xs, &xe, 1, &acc);
        if (acc != 7 + naive(row, xs, xe)) {
          bad++;
        }

        uint64_t acc_c = 7;
        nl_sumspan_u8_c(row, &xs, &xe, 1, &acc_c);
        if (acc_c != acc) {
          bad++;
        }
      }
    }

    CHECK(bad == 0, "length x offset sweep: %d mismatches", bad);
  }

  // Fence: bytes just outside span are 0xFF, inside they are 0x01
  {
    static uint8_t buf[512];
    int bad = 0;
    for (int len = 0; len <= 70; len++) {
      for (int off = 0; off < 17; off++) {
        for (size_t i = 0; i < sizeof(buf); i++) {
          buf[i] = 0xFF;
        }

        int xs = 64 + off, xe = xs + len;
        for (int x = xs; x < xe; x++) {
          buf[x] = 1;
        }

        uint64_t acc = 0;
        nl_sumspan_u8(buf, &xs, &xe, 1, &acc);
        if (acc != (uint64_t)len) {
          bad++;
        }
      }
    }

    CHECK(bad == 0, "fence: %d spans read outside themselves", bad);
  }

  // Empty, backwards, n = 0, several cells with a shared acc
  {
    int xs[5] = {10, 20, 30, 40, 0};
    int xe[5] = {10, 15, 31, 100, 3};
    uint64_t acc[5] = {1, 2, 3, 4, 5};
    nl_sumspan_u8(row, xs, xe, 0, acc);
    CHECK(acc[0] == 1 && acc[4] == 5, "n = 0 touches nothing");

    nl_sumspan_u8(row, xs, xe, 5, acc);
    CHECK(acc[0] == 1, "empty span adds 0");
    CHECK(acc[1] == 2, "backwards span adds 0");
    CHECK(acc[2] == 3u + row[30], "one byte");
    CHECK(acc[3] == 4 + naive(row, 40, 100), "60 bytes");
    CHECK(acc[4] == 5u + row[0] + row[1] + row[2], "three bytes");

    // Called again: sums stack up, which is what row-by-row use relies on
    nl_sumspan_u8(row, xs, xe, 5, acc);
    CHECK(acc[3] == 4 + 2 * naive(row, 40, 100), "second call adds again");
  }

  // 4096 x 4096 of 255 in one cell: 4.27e9, over 32 bits only if acc is not
  {
    static uint8_t full[4096];
    for (int i = 0; i < 4096; i++) {
      full[i] = 255;
    }

    int xs = 0;
    int xe = 4096;
    uint64_t acc = 0;
    for (int y = 0; y < 4096; y++) {
      nl_sumspan_u8(full, &xs, &xe, 1, &acc);
    }

    CHECK(acc == 4096ull * 4096ull * 255ull, "no overflow: %llu",
          (unsigned long long)acc);
  }

  // Random rows, random span tables, assembly = C = naive
  {
    enum { N = 97 };
    int32_t xs[N];
    int32_t xe[N];
    uint64_t a1[N];
    uint64_t a2[N];
    int bad = 0;
    for (int round = 0; round < 2000; round++) {
      int n = (int)(rnd() % N) + 1;
      for (int i = 0; i < n; i++) {
        int a = (int)(rnd() % page), b = (int)(rnd() % page);
        if (rnd() % 4) { // Mostly short spans, like real cells
          b = a + (int)(rnd() % 40);
          if (b > (int)page) {
            b = (int)page;
          }
        }

        xs[i] = a;
        xe[i] = b; // May be < a: empty
        a1[i] = a2[i] = rnd();
      }

      nl_sumspan_u8(row, xs, xe, n, a1);
      nl_sumspan_u8_c(row, xs, xe, n, a2);
      for (int i = 0; i < n; i++) {
        if (a1[i] != a2[i]) {
          bad++;
        }
      }
    }

    CHECK(bad == 0, "random tables: %d mismatches", bad);
  }

  // nl_addrow_u8: every length at every offset, source ends on last mapped byte
  // acc is fenced: nothing before or after its n entries may change
  {
    static uint32_t buf[4 + 160 + 4];
    int bad = 0, bad_fence = 0;
    for (int n = 0; n <= 140; n++) {
      for (int off = 0; off < 17; off++) {
        int start = (int)page - off - n;
        if (start < 0) {
          continue;
        }

        for (int i = 0; i < 4 + 160 + 4; i++) {
          buf[i] = 0xDEADBEEFu;
        }

        uint32_t *acc = buf + 4;
        for (int i = 0; i < n; i++) {
          acc[i] = (uint32_t)i * 1000u; // added to, not replaced
        }

        nl_addrow_u8(acc, row + start, (size_t)n);
        for (int i = 0; i < n; i++) {
          if (acc[i] != (uint32_t)i * 1000u + row[start + i]) {
            bad++;
          }
        }

        for (int i = 0; i < 4; i++) {
          if (buf[i] != 0xDEADBEEFu || acc[n + i] != 0xDEADBEEFu) {
            bad_fence++;
          }
        }
      }
    }

    CHECK(bad == 0, "addrow sweep: %d wrong sums", bad);
    CHECK(bad_fence == 0, "addrow wrote outside acc: %d", bad_fence);
  }

  // Rows stack up and match  C twin; 255 x many rows stays exact
  {
    static uint32_t a1[3 * 331], a2[3 * 331];
    static uint8_t full[3 * 331];
    for (size_t i = 0; i < sizeof(full); i++) {
      full[i] = 255;
    }

    for (int r = 0; r < 20000; r++) {
      nl_addrow_u8(a1, full, sizeof(full));
    }

    int ok = 1;
    for (size_t i = 0; i < sizeof(full); i++) {
      ok &= a1[i] == 255u * 20000u;
    }

    CHECK(ok, "20000 rows of 255 are exact in 32 bits");

    for (size_t i = 0; i < sizeof(a1) / sizeof(a1[0]); i++) {
      a1[i] = a2[i] = 0;
    }

    int bad = 0;
    for (int round = 0; round < 300; round++) {
      size_t n = rnd() % sizeof(full);
      size_t off = rnd() % (page - n);
      nl_addrow_u8(a1, row + off, n);
      nl_addrow_u8_c(a2, row + off, n);
    }

    for (size_t i = 0; i < sizeof(a1) / sizeof(a1[0]); i++) {
      bad += a1[i] != a2[i];
    }

    CHECK(bad == 0, "random rows: %d mismatches", bad);
  }

  // Not a check: how much faster (640 x 480 picture into 80 x 40 cells)
  {
    enum { W = 640, H = 480, CW = 80 };
    static uint8_t pic[W * H];
    static int32_t xs[CW], xe[CW];
    static uint64_t acc[CW];
    for (int i = 0; i < W * H; i++) {
      pic[i] = (uint8_t)rnd();
    }

    for (int x = 0; x < CW; x++) {
      xs[x] = x * (W / CW);
      xe[x] = xs[x] + W / CW;
    }

    struct timespec t0;
    struct timespec t1;
    long ns_asm;
    long ns_c;
    const int frames = 300;
    uint64_t guard = 0;

    nl_clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < frames; f++) {
      for (int x = 0; x < CW; x++) {
        acc[x] = 0;
      }

      for (int y = 0; y < H; y++) {
        nl_sumspan_u8(pic + y * W, xs, xe, CW, acc);
      }

      guard += acc[f % CW];
    }

    nl_clock_gettime(CLOCK_MONOTONIC, &t1);
    ns_asm = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);

    nl_clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < frames; f++) {
      for (int x = 0; x < CW; x++) {
        acc[x] = 0;
      }

      for (int y = 0; y < H; y++) {
        nl_sumspan_u8_c(pic + y * W, xs, xe, CW, acc);
      }

      guard += acc[f % CW];
    }

    nl_clock_gettime(CLOCK_MONOTONIC, &t1);
    ns_c = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
    CHECK(guard != 1, "keep the loops");
    fprintf(stderr,
            "640x480 -> 80 cells: %ld us/frame (%s), %ld us/frame (C)\n",
            ns_asm / frames / 1000, NL_SUMSPAN_ASM ? "asm" : "C",
            ns_c / frames / 1000);
  }

  fprintf(stderr, "%d passed %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
