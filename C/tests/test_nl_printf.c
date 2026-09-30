/*
 * tests/test_nl_printf.c - unit tests for nl_vsnprintf / nl_snprintf
 *
 * Tests implementation behaviour, which diverges from C99 snprintf in two
 * intentional ways:
 *   1. Return value is number of chars written, not number that would have been
 *      written if buffer were large enough
 *   2. Width padding is always space-filled; '0' flag is parsed but silently
 *      ignored ("%05d" pads with spaces, not zeros)
 *
 * gcc -D__LINUX_NOLIBC__ -nostdlib -static  test_nl_printf.c \
 *           ../lib/nl_printf.c ../lib/nl_errno.c ../lib/nl_alloc.c \
 *           ../lib/nl_start.c -I../include -I../lib -o test_nl_printf
 *
 */
#include "../lib/nolibc.h"

// Test framework
static int _pass = 0, _fail = 0;

static void _check_str(const char *got, const char *want, const char *label,
                       int line) {
  char buf[512];
  int ok = (nl_strcmp(got, want) == 0);
  int n;
  if (ok) {
    n = nl_snprintf(buf, sizeof(buf), "ok   [%3d] %s\n", line, label);
    _pass++;
  } else {
    n = nl_snprintf(buf, sizeof(buf),
                    "FAIL [%3d] %s\n"
                    "          got  = '%s'\n"
                    "          want = '%s'\n",
                    line, label, got, want);
    _fail++;
  }

  if (n > 0) {
    nl_write(1, buf, (size_t)n);
  }
}

static void _check_int(int got, int want, const char *label, int line) {
  char buf[256];
  int ok = (got == want);
  int n;
  if (ok) {
    n = nl_snprintf(buf, sizeof(buf), "ok   [%3d] %s\n", line, label);
    _pass++;
  } else {
    n = nl_snprintf(buf, sizeof(buf), "FAIL [%3d] %s: got=%d want=%d\n", line,
                    label, got, want);
    _fail++;
  }

  if (n > 0) {
    nl_write(1, buf, (size_t)n);
  }
}

// Formats with nl_snprintf, compares result to expected string
#define FMT_EQ(want, ...)                                                      \
  do {                                                                         \
    char _b[256];                                                              \
    int _n = nl_snprintf(_b, sizeof(_b), __VA_ARGS__);                         \
    _check_str(_b, (want), #__VA_ARGS__ " == \"" want "\"", __LINE__);         \
    (void)_n;                                                                  \
  } while (0)

// Return-value check: nl_snprintf returns chars written (not C99 would-have)
#define FMT_RET(size, want_ret, ...)                                           \
  do {                                                                         \
    char _b[(size)];                                                           \
    int _r = nl_snprintf(_b, (size), __VA_ARGS__);                             \
    _check_int(_r, (want_ret), "ret(" #__VA_ARGS__ ", sz=" #size ")",          \
               __LINE__);                                                      \
  } while (0)

// Test cases
static void test_percent_d(void) {
  FMT_EQ("0", "%d", 0);
  FMT_EQ("1", "%d", 1);
  FMT_EQ("-1", "%d", -1);
  FMT_EQ("42", "%d", 42);
  FMT_EQ("-42", "%d", -42);
  FMT_EQ("2147483647", "%d", 2147483647);   // INT_MAX
  FMT_EQ("-2147483648", "%d", -2147483648); // INT_MIN
  // Width: right-aligned with spaces
  FMT_EQ("   42", "%5d", 42);
  FMT_EQ("-   42", "%5d", -42); // minus emitted first, then (width-n) spaces
  FMT_EQ("-   42", "%5d", -42);
  // Width smaller than value: no truncation
  FMT_EQ("12345", "%3d", 12345);
  // Zero-flag parsed but IGNORED
  FMT_EQ("   42", "%05d", 42); // spaces, NOT zeros
}

static void test_percent_u(void) {
  FMT_EQ("0", "%u", 0u);
  FMT_EQ("42", "%u", 42u);
  FMT_EQ("4294967295", "%u", 4294967295u); // UINT_MAX
  FMT_EQ("   42", "%5u", 42u);             // right-aligned with spaces
}

static void test_percent_x(void) {
  FMT_EQ("0", "%x", 0u);
  FMT_EQ("ff", "%x", 0xffu);
  FMT_EQ("deadbeef", "%x", 0xdeadbeefu);
  FMT_EQ("a", "%x", 10u);
  // Width is not implemented for %x - value prints unpadded
  FMT_EQ("2a", "%5x", 42u); // no padding
}

static void test_percent_s(void) {
  FMT_EQ("hello", "%s", "hello");
  FMT_EQ("", "%s", "");
  FMT_EQ("(null)", "%s", (char *)0); // NULL -> "(null)"
  FMT_EQ("     hi", "%7s", "hi");    // right-aligned
  FMT_EQ("hi", "%2s", "hi");         // no truncation when value >= width
  FMT_EQ("hi", "%1s", "hi");
}

static void test_percent_c(void) {
  FMT_EQ("A", "%c", 'A');
  FMT_EQ("\n", "%c", '\n');
  FMT_EQ("%", "%%");
}

static void test_long(void) {
  FMT_EQ("0", "%ld", 0LL);
  FMT_EQ("-1", "%ld", -1LL);
  FMT_EQ("9223372036854775807", "%ld", 9223372036854775807LL); // LLONG_MAX
  FMT_EQ("0", "%lu", 0ULL);
  FMT_EQ("18446744073709551615", "%lu",
         18446744073709551615ULL); // ULLONG_MAX
}

static void test_truncation(void) {
  // Return value = chars written (not C99 would have written)
  FMT_RET(1, 0, "%d", 42);         // size=1: only null terminator fits
  FMT_RET(3, 2, "%d", 42);         // size=3: "42\0" -> wrote 2
  FMT_RET(4, 3, "%d", 123);        // size=4: "123\0" -> wrote 3
  FMT_RET(3, 2, "%d", 123);        // size=3: "12\0" -> wrote 2
  FMT_RET(9, 8, "%s", "abcdefgh"); // fits exactly (8 chars + null)
  FMT_RET(5, 4, "%s", "abcdefgh"); // truncated: "abcd\0"

  // Null terminator always written within buffer
  {
    char buf[4] = {'X', 'X', 'X', 'X'};
    nl_snprintf(buf, 4, "%d", 99999);
    _check_int(buf[3], '\0', "truncated buf always null-terminated", __LINE__);
  }
}

static void test_combined(void) {
  // Multiple specifiers in one call
  FMT_EQ("x=1 y=2", "x=%d y=%d", 1, 2);
  FMT_EQ("hi there", "%s %s", "hi", "there");
  FMT_EQ("val=0xff", "val=0x%x", 0xff);
  // Literal %%
  FMT_EQ("100%", "%d%%", 100);
}

// Main
int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  test_percent_d();
  test_percent_u();
  test_percent_x();
  test_percent_s();
  test_percent_c();
  test_long();
  test_truncation();
  test_combined();

  char summary[128];
  int n = nl_snprintf(summary, sizeof(summary), "\n%d passed  %d failed\n",
                      _pass, _fail);
  nl_write(1, summary, (size_t)n);

  return _fail ? 1 : 0;
}
