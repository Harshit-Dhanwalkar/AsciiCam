/*
 * tests/test_nl_slice.c - lib/nl_checked.h and lib/nl_slice.h
 *
 * Overflow arithmetic is compared with a 128-bit reference over every pair of
 * boundary values; slice operations are checked at every edge and with a
 * randomized run that surrounds buffers with guard bytes
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_nl_slice.c lib/nl_printf.c lib/nl_errno.c \
 *     lib/nl_alloc.c lib/nl_start.c -Iinclude -Ilib -o /tmp/ts
 */
#include "../lib/nl_slice.h"
#include "../lib/nolibc.h"

static int pass_n, fail_n;

#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      if (fail_n < 30) {                                                       \
        fprintf(stderr, "FAIL line %d: ", __LINE__);                           \
        fprintf(stderr, __VA_ARGS__);                                          \
        fprintf(stderr, "\n");                                                 \
      }                                                                        \
    }                                                                          \
  } while (0)

static nl_mslice_t nl_mslice_make_empty(void) {
  nl_mslice_t m = {(uint8_t *)0, 0};

  return m;
}

static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static unsigned rnd_n(unsigned n) {
  rng ^= rng >> 12;
  rng ^= rng << 25;
  rng ^= rng >> 27;

  return (unsigned)((rng * 0x2545F4914F6CDD1DULL) >> 33) % n;
}

static const size_t edge[] = {0,
                              1,
                              2,
                              3,
                              7,
                              8,
                              15,
                              16,
                              17,
                              4095,
                              4096,
                              (size_t)1 << 31,
                              (size_t)1 << 32,
                              (size_t)1 << 62,
                              (size_t)1 << 63,
                              SIZE_MAX / 2,
                              SIZE_MAX / 2 + 1,
                              SIZE_MAX - 16,
                              SIZE_MAX - 1,
                              SIZE_MAX};
#define NEDGE (sizeof(edge) / sizeof(edge[0]))

typedef unsigned __int128 u128;

static void test_checked(void) {
  for (size_t i = 0; i < NEDGE; i++) {
    for (size_t j = 0; j < NEDGE; j++) {
      size_t a = edge[i], b = edge[j], out = 12345;

      u128 sum = (u128)a + b;
      int ok = sum <= SIZE_MAX;
      CHECK(nl_size_add(a, b, &out) == (ok ? 0 : -1) &&
                (ok ? out == (size_t)sum : out == 12345),
            "add %zu + %zu", a, b);

      out = 12345;
      ok = a >= b;
      CHECK(nl_size_sub(a, b, &out) == (ok ? 0 : -1) &&
                (ok ? out == a - b : out == 12345),
            "sub %zu - %zu", a, b);

      out = 12345;
      u128 prod = (u128)a * b;
      ok = prod <= SIZE_MAX;
      CHECK(nl_size_mul(a, b, &out) == (ok ? 0 : -1) &&
                (ok ? out == (size_t)prod : out == 12345),
            "mul %zu * %zu", a, b);
    }
  }

  // align_up: every edge value against every power of two
  for (size_t i = 0; i < NEDGE; i++) {
    for (int sh = 0; sh < 64; sh++) {
      size_t a = (size_t)1 << sh, out = 777;
      u128 want = ((u128)edge[i] + a - 1) &
                  ~((u128)a - 1); // no 128-bit divide: libgcc is not linked
      int ok = want <= SIZE_MAX;
      CHECK(nl_size_align_up(edge[i], a, &out) == (ok ? 0 : -1) &&
                (ok ? out == (size_t)want : out == 777),
            "align_up(%zu, 1<<%d)", edge[i], sh);
    }
  }

  size_t out = 5;
  CHECK(nl_size_align_up(10, 0, &out) == -1 && out == 5, "align 0 refused");
  CHECK(nl_size_align_up(10, 3, &out) == -1 && out == 5,
        "non power-of-two align refused");
}

static void test_slice_bounds(void) {
  uint8_t data[16];
  for (int i = 0; i < 16; i++) {
    data[i] = (uint8_t)(i * 3);
  }

  nl_slice_t s = nl_slice_make(data, sizeof(data));
  nl_slice_t sentinel = {(const uint8_t *)0x1, 99};

  // every (off, len), including values that would wrap off+len
  for (size_t i = 0; i < NEDGE; i++) {
    for (size_t j = 0; j < NEDGE; j++) {
      size_t off = edge[i], len = edge[j];
      nl_slice_t o = sentinel;
      int ok = off <= 16 && len <= 16 - off;
      int r = nl_slice_sub(s, off, len, &o);
      CHECK(r == (ok ? 0 : -1), "sub(%zu,%zu) result", off, len);
      if (ok) {
        CHECK(o.ptr == data + off && o.len == len, "sub(%zu,%zu) view", off,
              len);
      } else {
        CHECK(o.ptr == sentinel.ptr && o.len == sentinel.len,
              "sub(%zu,%zu) untouched on failure", off, len);
      }
    }
  }

  for (size_t off = 0; off <= 16; off++) {
    for (size_t len = 0; len <= 17; len++) {
      nl_slice_t o;
      CHECK((nl_slice_sub(s, off, len, &o) == 0) == (off + len <= 16),
            "exhaustive sub(%zu,%zu)", off, len);
    }
  }

  uint8_t b = 0xEE;
  CHECK(nl_slice_get(s, 15, &b) == 0 && b == 45, "get last");
  b = 0xEE;
  CHECK(nl_slice_get(s, 16, &b) == -1 && b == 0xEE, "get one past end");
  CHECK(nl_slice_get(s, SIZE_MAX, &b) == -1, "get SIZE_MAX");
  nl_slice_t empty = nl_slice_make(data, 0);
  CHECK(nl_slice_get(empty, 0, &b) == -1, "get from empty");

  uint8_t m[4] = {1, 2, 3, 4};
  nl_mslice_t ms = NL_MSLICE_OF(m);
  CHECK(nl_mslice_set(ms, 3, 9) == 0 && m[3] == 9, "set last");
  CHECK(nl_mslice_set(ms, 4, 7) == -1, "set past end refused");
  nl_mslice_t msub;
  CHECK(nl_mslice_sub(ms, 1, 2, &msub) == 0 && msub.ptr == m + 1, "msub");
  CHECK(nl_mslice_sub(ms, 3, 2, &msub) == -1, "msub past end");
}

static void test_eq_find_copy(void) {
  uint8_t a[] = {1, 2, 3, 4, 5};
  uint8_t b[] = {1, 2, 3, 4, 5};
  uint8_t c[] = {1, 2, 3, 4, 6};
  CHECK(nl_slice_eq(NL_SLICE_OF(a), NL_SLICE_OF(b)), "eq");
  CHECK(!nl_slice_eq(NL_SLICE_OF(a), NL_SLICE_OF(c)), "ne");
  CHECK(!nl_slice_eq(nl_slice_make(a, 4), NL_SLICE_OF(a)), "different length");
  CHECK(nl_slice_eq(nl_slice_make(a, 0), nl_slice_make(c, 0)),
        "empty == empty");
  CHECK(nl_slice_find_byte(NL_SLICE_OF(a), 4) == 3, "find");
  CHECK(nl_slice_find_byte(NL_SLICE_OF(a), 9) == NL_NPOS, "find missing");
  CHECK(nl_slice_find_byte(nl_slice_make(a, 3), 4) == NL_NPOS,
        "find limited to slice");

  // copy: too big copies nothing; fits copies exactly; overlap both ways
  uint8_t dst[8];
  nl_mslice_fill(NL_MSLICE_OF(dst), 0xAA);
  CHECK(nl_mslice_copy(NL_MSLICE_OF(dst), NL_SLICE_OF(a)) == 0,
        "copy 5 into 8");
  CHECK(dst[0] == 1 && dst[4] == 5 && dst[5] == 0xAA && dst[7] == 0xAA,
        "copied exactly 5 bytes, rest untouched");

  uint8_t small[3] = {9, 9, 9};
  CHECK(nl_mslice_copy(NL_MSLICE_OF(small), NL_SLICE_OF(a)) == -1,
        "copy too large refused");
  CHECK(small[0] == 9 && small[1] == 9 && small[2] == 9,
        "nothing copied on refusal (no partial copy)");
  CHECK(nl_mslice_copy(nl_mslice_make_empty(), nl_slice_make(a, 0)) == 0,
        "empty into empty");

  uint8_t ov[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  nl_mslice_t whole = NL_MSLICE_OF(ov);
  nl_mslice_t hi;
  nl_mslice_sub(whole, 2, 6, &hi);
  nl_slice_t lo;
  nl_slice_sub(nl_mslice_ro(whole), 0, 6, &lo);
  CHECK(nl_mslice_copy(hi, lo) == 0 && ov[2] == 1 && ov[7] == 6 && ov[0] == 1,
        "overlapping copy forwards (dst above src)");
  uint8_t ov2[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  nl_mslice_t whole2 = NL_MSLICE_OF(ov2);
  nl_mslice_t lo2;
  nl_mslice_sub(whole2, 0, 6, &lo2);
  nl_slice_t hi2;
  nl_slice_sub(nl_mslice_ro(whole2), 2, 6, &hi2);
  CHECK(nl_mslice_copy(lo2, hi2) == 0 && ov2[0] == 3 && ov2[5] == 8,
        "overlapping copy backwards (dst below src)");
}

static void test_str(void) {
  nl_str_t s = NL_STR_LIT("plugin.so");
  CHECK(s.len == 9, "literal length");
  CHECK(nl_str_from_cstr("abc").len == 3 && nl_str_from_cstr(NULL).len == 0 &&
            nl_str_from_cstr("").len == 0,
        "from_cstr");
  CHECK(nl_str_starts_with(s, NL_STR_LIT("plug")) &&
            !nl_str_starts_with(s, NL_STR_LIT("lug")) &&
            nl_str_starts_with(s, s) &&
            !nl_str_starts_with(s, NL_STR_LIT("plugin.soo")) &&
            nl_str_starts_with(s, NL_STR_LIT("")),
        "starts_with");
  CHECK(nl_str_ends_with(s, NL_STR_LIT(".so")) &&
            !nl_str_ends_with(s, NL_STR_LIT(".dll")) &&
            nl_str_ends_with(s, s) &&
            !nl_str_ends_with(s, NL_STR_LIT("xplugin.so")) &&
            nl_str_ends_with(s, NL_STR_LIT("")),
        "ends_with");
  CHECK(nl_str_find(s, '.') == 6 && nl_str_find(s, 'z') == NL_NPOS, "find");

  nl_str_t h, t;
  CHECK(nl_str_split_once(NL_STR_LIT("key=value=x"), '=', &h, &t) == 0 &&
            nl_str_eq(h, NL_STR_LIT("key")) &&
            nl_str_eq(t, NL_STR_LIT("value=x")),
        "split at first separator");
  CHECK(nl_str_split_once(NL_STR_LIT("novalue"), '=', &h, &t) == -1 &&
            nl_str_eq(h, NL_STR_LIT("novalue")) && t.len == 0,
        "split without separator");
  CHECK(nl_str_split_once(NL_STR_LIT("k="), '=', &h, &t) == 0 && t.len == 0 &&
            nl_str_eq(h, NL_STR_LIT("k")),
        "split with empty tail");
  CHECK(nl_str_eq(nl_str_trim(NL_STR_LIT("  \t hi there \r\n")),
                  NL_STR_LIT("hi there")),
        "trim");
  CHECK(nl_str_trim(NL_STR_LIT("   ")).len == 0 &&
            nl_str_trim(NL_STR_LIT("")).len == 0,
        "trim all-space / empty");

  // copy_cstr: exact fit, one short, empty string, zero-size destination
  char buf[6];
  nl_mslice_t d = NL_MSLICE_OF(buf);
  nl_mslice_fill(d, 'x');
  CHECK(nl_str_copy_cstr(d, NL_STR_LIT("hello")) == 0 && nl_strlen(buf) == 5 &&
            buf[5] == 0,
        "5 chars + NUL fit exactly in 6");
  nl_mslice_fill(d, 'x');
  CHECK(nl_str_copy_cstr(d, NL_STR_LIT("hello!")) == -1, "6 chars do not fit");
  CHECK(buf[0] == 'x' && buf[5] == 'x', "destination untouched on refusal");
  CHECK(nl_str_copy_cstr(d, NL_STR_LIT("")) == 0 && buf[0] == 0,
        "empty string");
  CHECK(nl_str_copy_cstr(nl_mslice_make_empty(), NL_STR_LIT("")) == -1,
        "no room even for NUL");
  CHECK(nl_str_copy_cstr(d, nl_str_make("abc", SIZE_MAX)) == -1,
        "length near SIZE_MAX does not wrap +1");

  // parse
  size_t v = 7;
  CHECK(nl_str_to_size(NL_STR_LIT("0"), &v) == 0 && v == 0, "parse 0");
  CHECK(nl_str_to_size(NL_STR_LIT("12345"), &v) == 0 && v == 12345, "parse");
  CHECK(nl_str_to_size(NL_STR_LIT("18446744073709551615"), &v) == 0 &&
            v == SIZE_MAX,
        "parse SIZE_MAX");
  v = 7;
  CHECK(nl_str_to_size(NL_STR_LIT("18446744073709551616"), &v) == -1 && v == 7,
        "SIZE_MAX + 1 refused");
  CHECK(nl_str_to_size(NL_STR_LIT("99999999999999999999999"), &v) == -1,
        "far too large refused");
  CHECK(nl_str_to_size(NL_STR_LIT(""), &v) == -1 &&
            nl_str_to_size(NL_STR_LIT("-1"), &v) == -1 &&
            nl_str_to_size(NL_STR_LIT("+1"), &v) == -1 &&
            nl_str_to_size(NL_STR_LIT("1 "), &v) == -1 &&
            nl_str_to_size(NL_STR_LIT("0x10"), &v) == -1 &&
            nl_str_to_size(NL_STR_LIT("1a"), &v) == -1,
        "non-digits refused");
  CHECK(v == 7, "failed parses leave output alone");
  CHECK(nl_str_to_size(nl_str_make("12345", 2), &v) == 0 && v == 12,
        "length, not a NUL, ends number");
}

// Random operations on a buffer surrounded by guard bytes: nothing may land
// outside slice it was given
#define GUARD 32
static void test_guards(void) {
  uint8_t mem[GUARD + 64 + GUARD];
  int bad = 0;
  for (int it = 0; it < 20000; it++) {
    nl_mslice_fill(NL_MSLICE_OF(mem), 0xC3);
    nl_mslice_t whole = {mem + GUARD, 64};
    nl_mslice_t part;
    size_t off = rnd_n(80), len = rnd_n(80);
    int fits = off <= 64 && len <= 64 - off;
    if ((nl_mslice_sub(whole, off, len, &part) == 0) != fits) {
      bad++;

      continue;
    }

    if (!fits) {
      continue;
    }

    uint8_t src[80];
    size_t n = rnd_n(80);
    for (size_t i = 0; i < n; i++) {
      src[i] = (uint8_t)i;
    }

    int r = nl_mslice_copy(part, nl_slice_make(src, n));
    if ((r == 0) != (n <= len)) {
      bad++;
    }

    for (size_t i = 0; i < GUARD; i++) {
      if (mem[i] != 0xC3 || mem[GUARD + 64 + i] != 0xC3) {
        bad++;

        break;
      }
    }

    // bytes inside `whole` but outside `part` untouched too
    for (size_t i = 0; i < 64; i++) {
      if ((i < off || i >= off + len) && mem[GUARD + i] != 0xC3) {
        bad++;

        break;
      }
    }

    if (r != 0) { // refused: part must be untouched
      for (size_t i = 0; i < len; i++) {
        if (mem[GUARD + off + i] != 0xC3) {
          bad++;

          break;
        }
      }
    }
  }

  CHECK(bad == 0, "guarded random sub/copy run: %d violations", bad);
}

int main(void) {
  test_checked();
  test_slice_bounds();
  test_eq_find_copy();
  test_str();
  test_guards();

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
