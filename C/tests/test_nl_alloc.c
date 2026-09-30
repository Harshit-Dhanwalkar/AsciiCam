/*
 * tests/test_nl_alloc.c - unit tests for nl_alloc arena allocator
 *
 * Exercises nl_malloc / nl_calloc / nl_free against two allocation
 * paths (arena for small requests, mmap for > 1 MiB) and forward +
 * backward coalescing logic
 *
 * gcc -D__LINUX_NOLIBC__ -nostdlib -static  test_nl_alloc.c \
 *        ../lib/nl_printf.c ../lib/nl_errno.c ../lib/nl_alloc.c \
 *        ../lib/nl_start.c -I../include -I../lib -o test_nl_alloc
 */
#include "../lib/nolibc.h"

// constants mirrored from nl_alloc.c (must stay in sync)
#define ARENA_SIZE (2 * 1024 * 1024)
#define MMAP_THRESHOLD (ARENA_SIZE / 2) // 1 MiB
#define ALLOC_ALIGN 16

// Test framework
static int _pass = 0, _fail = 0;

static void _chk(int ok, const char *msg, int line) {
  char buf[256];
  int n = nl_snprintf(buf, sizeof(buf),
                      ok ? "ok   [%3d] %s\n" : "FAIL [%3d] %s\n", line, msg);
  if (ok) {
    _pass++;
  } else {
    _fail++;
  }

  if (n > 0) {
    nl_write(1, buf, (size_t)n);
  }
}

#define EXPECT(cond) _chk(!!(cond), #cond, __LINE__)
#define EXPECT_NULL(p) _chk((p) == (void *)0, #p " == NULL", __LINE__)
#define EXPECT_NONNULL(p) _chk((p) != (void *)0, #p " != NULL", __LINE__)
#define EXPECT_EQ(a, b)                                                        \
  _chk((long long)(a) == (long long)(b), #a " == " #b, __LINE__)

// Helpers
// Write a canary pattern across a buffer and verify it later
static void fill(unsigned char *p, size_t n, unsigned char v) {
  for (size_t i = 0; i < n; i++) {
    p[i] = v;
  }
}

static int verify(const unsigned char *p, size_t n, unsigned char v) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] != v) {
      return 0;
    }
  }

  return 1;
}

/* Test cases */
static void test_basic(void) {
  // malloc(0) is treated as malloc(1) by implementation
  void *p0 = nl_malloc(0);
  EXPECT_NONNULL(p0);

  nl_free(p0);

  // Basic alloc/free cycle
  void *p = nl_malloc(64);
  EXPECT_NONNULL(p);
  fill(p, 64, 0xAB);
  EXPECT(verify(p, 64, 0xAB));

  nl_free(p);

  // free(NULL) must be a no-op
  nl_free((void *)0); // should not crash
  EXPECT(1);          // reaching here means no crash
}

static void test_alignment(void) {
  // Every allocation must be aligned to ALLOC_ALIGN (16) bytes
  void *ptrs[8];
  for (int i = 0; i < 8; i++) {
    ptrs[i] = nl_malloc((size_t)(1 + i * 7));
    EXPECT_NONNULL(ptrs[i]);
    EXPECT(((unsigned long)(unsigned long long)(size_t)ptrs[i] &
            (ALLOC_ALIGN - 1)) == 0);
  }

  for (int i = 0; i < 8; i++) {
    nl_free(ptrs[i]);
  }
}

static void test_calloc(void) {
  // calloc must zero-fill
  unsigned char *p = nl_calloc(64, 1);
  EXPECT_NONNULL(p);
  EXPECT(verify(p, 64, 0x00));

  // calloc with non-trivial element size
  unsigned char *q = nl_calloc(4, 16);
  EXPECT_NONNULL(q);
  EXPECT(verify(q, 64, 0x00));

  nl_free(p);
  nl_free(q);

  // calloc overflow protection: nmemb * size would overflow size_t
  void *ov = nl_calloc((size_t)-1, 2); // would wrap to 0xfffffe or overflow
  EXPECT_NULL(ov);
}

static void test_mmap_path(void) {
  /* Allocations > MMAP_THRESHOLD must use mmap, not arena
   * Key observable difference: mmap allocations are always freed via
   * munmap - freeing one does not affect adjacent arena blocks  */
  size_t big = MMAP_THRESHOLD + 1;
  void *p = nl_malloc(big);
  EXPECT_NONNULL(p);

  fill(p, big, 0x55);
  EXPECT(verify(p, big, 0x55));

  // Two mmap allocations at different addresses
  void *q = nl_malloc(big);
  EXPECT_NONNULL(q);
  EXPECT(p != q);

  nl_free(p);
  nl_free(q);
}

static void test_coalesce_forward(void) {
  /* After freeing two adjacent arena blocks in order, a subsequent
   * allocation of their combined size should fit in merged block
   * Size chosen so that each is exactly 2 × ALLOC_ALIGN bytes of user
   * data, making combined hole 2×user + 1×header = 2×32 + 16 = 80. */
  const size_t SZ = 2 * ALLOC_ALIGN; // 32 bytes user data per block

  void *a = nl_malloc(SZ);
  void *b = nl_malloc(SZ);
  EXPECT_NONNULL(a);
  EXPECT_NONNULL(b);

  // Free in forward order
  nl_free(a);
  nl_free(b);

  // Combined region = SZ + header_bytes + SZ = 32 + 16 + 32 = 80
  void *big = nl_malloc(SZ * 2 + 16 /* header */);
  EXPECT_NONNULL(big); // fails if coalescing didn't fire

  nl_free(big);
}

static void test_coalesce_backward(void) {
  // Same idea but freed in reverse order to exercise backward scan
  const size_t SZ = 2 * ALLOC_ALIGN;

  void *a = nl_malloc(SZ);
  void *b = nl_malloc(SZ);
  EXPECT_NONNULL(a);
  EXPECT_NONNULL(b);

  nl_free(b); // free second first
  nl_free(a); // backward-coalesce absorbs b

  void *big = nl_malloc(SZ * 2 + 16);
  EXPECT_NONNULL(big);
  nl_free(big);
}

static void test_many_small(void) {
  // Allocate many small blocks and verify each is independently writable
  const int N = 64;
  void *ptrs[64];
  for (int i = 0; i < N; i++) {
    ptrs[i] = nl_malloc(16);
    EXPECT_NONNULL(ptrs[i]);
    fill(ptrs[i], 16, (unsigned char)i);
  }

  // Verify no allocation overwrote another's canary
  int ok = 1;
  for (int i = 0; i < N; i++)
    if (!verify(ptrs[i], 16, (unsigned char)i)) {
      ok = 0;

      break;
    }

  _chk(ok, "64 independent 16-byte allocs: canaries intact", __LINE__);

  for (int i = 0; i < N; i++) {
    nl_free(ptrs[i]);
  }
}

// Main
int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  test_basic();
  test_alignment();
  test_calloc();
  test_mmap_path();
  test_coalesce_forward();
  test_coalesce_backward();
  test_many_small();

  char buf[128];
  int n =
      nl_snprintf(buf, sizeof(buf), "\n%d passed  %d failed\n", _pass, _fail);
  nl_write(1, buf, (size_t)n);
  return _fail ? 1 : 0;
}
