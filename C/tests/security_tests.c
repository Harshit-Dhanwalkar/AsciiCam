/*
 * Tests for plugins must never corrupt heap or crash due to integer
 * overflow in dimension-based allocations
 *
 * Dependency-free (nolibc) port of the old libcheck test, so it runs in CI
 * and under `make test` without installing anything.
 *
 * gcc -DTESTING -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdlib -static \
 *     tests/security_tests.c filters/edge_detect.c filters/invert.c \
 *     filters/threshold.c lib/nl_printf.c lib/nl_errno.c lib/nl_alloc.c \
 *     lib/nl_start.c -Iinclude -Ilib -o /tmp/security_tests
 */

#include "../lib/nolibc.h"

#include "../filters/edge_detect.h"
#include "../filters/invert.h"
#include "../filters/threshold.h"

static int _pass = 0;
static int _fail = 0;

static void _chk(int ok, const char *what, const char *name, int line) {
  char buf[256];
  int n = nl_snprintf(buf, sizeof(buf),
                      ok ? "ok   [%3d] %s: %s\n" : "FAIL [%3d] %s: %s\n", line,
                      name, what);
  if (ok) {
    _pass++;
  } else {
    _fail++;
  }

  if (n > 0) {
    nl_write(1, buf, (size_t)n);
  }
}

typedef uint8_t *(*filter_fn)(uint8_t *, int, int);

typedef struct {
  filter_fn fn;
  const char *name;
} filter_case_t;

typedef struct {
  int w;
  int h;
} dims_t;

int main(void) {
  filter_case_t filters[] = {
      {edge_detect, "edge_detect"},
      {invert, "invert"},
      {threshold, "threshold"},
  };

  dims_t payloads[] = {
      {65536, 65536},  // w*h == 0 mod 2^32: the original exploit
      {65535, 65535},  // product overflows 16 bits, fits no sane frame
      {2147483647, 2}, // int * int would overflow if computed as int
      {-1, 48},        // negative width
      {64, -1},        // negative height
      {0, 0},          // empty
      {64, 48},        // valid small frame
  };

  for (size_t p = 0; p < sizeof(filters) / sizeof(filters[0]); p++) {
    const char *name = filters[p].name;

    for (size_t i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++) {
      int w = payloads[i].w;
      int h = payloads[i].h;
      uint64_t area = (w > 0 && h > 0) ? (uint64_t)w * (uint64_t)h : 0;

      uint8_t *input = NULL;
      if (area > 0 && area <= (uint64_t)(16 * 1024 * 1024)) {
        input = nl_calloc((size_t)area, 1);
      }

      uint8_t *result = filters[p].fn(input, w, h);

      if (area > (uint64_t)UINT32_MAX || w <= 0 || h <= 0) {
        _chk(result == NULL, "rejects bad dimensions", name, __LINE__);
      } else if (input) {
        _chk(result != NULL, "accepts valid frame", name, __LINE__);
      }

      nl_free(result);
      nl_free(input);
    }

    _chk(filters[p].fn(NULL, 64, 48) == NULL, "NULL buffer rejected", name,
         __LINE__);
    _chk(filters[p].fn(NULL, 0, 0) == NULL, "NULL buffer, 0x0 rejected", name,
         __LINE__);
  }

  char buf[128];
  int n =
      nl_snprintf(buf, sizeof(buf), "\n%d passed  %d failed\n", _pass, _fail);
  nl_write(1, buf, (size_t)n);

  return _fail ? 1 : 0;
}
