/*
 * tests/fuzz_nolibc.c - dependency-free randomized stress test for nolibc
 * layer: nl_vsnprintf, nl_malloc/nl_calloc/nl_free and nl_getopt
 *
 * No libFuzzer/AFL needed: a fixed-seed xorshift64* PRNG drives every case, so
 * a failure is reproducible from "seed=... iter=...". Checks invariants, not
 * exact output
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -ffreestanding -fno-builtin \
 *     -fno-stack-protector -nostdlib -static fuzz_nolibc.c \
 *     ../lib/nl_printf.c ../lib/nl_errno.c ../lib/nl_alloc.c \
 *     ../lib/nl_getopt.c ../lib/nl_start.c -I../include -I../lib -o fuzz_nolibc
 * ./fuzz_nolibc [iterations=20000] [seed=1]
 */
#include "../lib/nolibc.h"

static unsigned long long rng_state;
static unsigned long long rnd(void) {
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;

  return rng_state * 0x2545F4914F6CDD1DULL;
}
static unsigned rnd_n(unsigned n) { return (unsigned)(rnd() % n); }

static unsigned long long g_seed;
static long g_iter;
static int g_fails;
static const char *g_target = "";

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      if (g_fails < 25) {                                                      \
        fprintf(stderr, "FAIL %s seed=%u iter=%d line=%d: ", g_target,         \
                (unsigned)g_seed, (int)g_iter, __LINE__);                      \
        fprintf(stderr, __VA_ARGS__);                                          \
        fprintf(stderr, "\n");                                                 \
      }                                                                        \
                                                                               \
      g_fails++;                                                               \
    }                                                                          \
  } while (0)

static int mem_cmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) {
      return x[i] < y[i] ? -1 : 1;
    }
  }

  return 0;
}

// printf

#define GUARD 16
#define CANARY 0xA5

enum kind { K_NONE, K_INT, K_UINT, K_LL, K_STR };
static const struct {
  const char *conv;
  enum kind k;
} convs[] = {{"%d", K_INT},    {"%5d", K_INT}, {"%-5d", K_INT}, {"%05d", K_INT},
             {"%c", K_INT},    {"%3c", K_INT}, {"%u", K_UINT},  {"%x", K_UINT},
             {"%12u", K_UINT}, {"%ld", K_LL},  {"%lu", K_LL},   {"%s", K_STR},
             {"%8s", K_STR},   {"%%", K_NONE}, {"%z", K_NONE},  {"%lx", K_NONE},
             {"%99d", K_INT}};
static const char *const lits[] = {"",  "abc",          " ",         "=",
                                   "[", "\xe2\x96\x88", "0123456789"};

static void fuzz_printf_one(void) {
  char fmt[64];
  size_t fl = 0;
  const char *l1 = lits[rnd_n(7)], *l2 = lits[rnd_n(7)];
  unsigned ci = rnd_n(sizeof(convs) / sizeof(convs[0]));
  const char *parts[3] = {l1, convs[ci].conv, l2};
  for (int i = 0; i < 3; i++) {
    size_t pl = nl_strlen(parts[i]);
    nl_memcpy(fmt + fl, parts[i], pl);

    fl += pl;
  }

  fmt[fl] = '\0';

  static const int ints[] = {
      0, 1, -1, 42, -42, 2147483647, (int)(-2147483647 - 1)};
  int iv = (rnd_n(2) == 0) ? ints[rnd_n(7)] : (int)rnd();
  if (convs[ci].conv[1] == 'c' || convs[ci].conv[2] == 'c') {
    iv = 'A' + (int)rnd_n(26);
  }

  unsigned uv = (unsigned)rnd();
  long long llv = (long long)rnd();
  const char *sv = (rnd_n(6) == 0) ? (const char *)0 : "hello";

#define CALL(b, n)                                                             \
  (convs[ci].k == K_INT    ? nl_snprintf(b, n, fmt, iv)                        \
   : convs[ci].k == K_UINT ? nl_snprintf(b, n, fmt, uv)                        \
   : convs[ci].k == K_LL   ? nl_snprintf(b, n, fmt, llv)                       \
   : convs[ci].k == K_STR  ? nl_snprintf(b, n, fmt, sv)                        \
                           : nl_snprintf(b, n, fmt))

  char ref[512];
  CALL(ref, sizeof(ref));
  size_t reflen = nl_strlen(ref);

  size_t sz = 1 + rnd_n(40);
  unsigned char raw[64 + 2 * GUARD];
  for (size_t i = 0; i < sizeof(raw); i++) {
    raw[i] = CANARY;
  }

  char *buf = (char *)raw + GUARD;
  int r = CALL(buf, sz);

  for (int i = 0; i < GUARD; i++) {
    CHECK(raw[i] == CANARY, "underflow write fmt=\"%s\" sz=%d", fmt, (int)sz);
  }

  for (size_t i = sz; i < sizeof(raw) - GUARD; i++) {
    CHECK(raw[GUARD + i] == CANARY, "overflow write at +%d fmt=\"%s\" sz=%d",
          (int)i, fmt, (int)sz);
  }

  CHECK(r >= 0 && (size_t)r < sz, "ret %d out of range sz=%d fmt=\"%s\"", r,
        (int)sz, fmt);
  if (r >= 0 && (size_t)r < sz) {
    CHECK(buf[r] == '\0', "no NUL at ret fmt=\"%s\"", fmt);
    CHECK((size_t)r == nl_strlen(buf), "ret != strlen fmt=\"%s\"", fmt);

    size_t want = reflen < sz - 1 ? reflen : sz - 1;
    CHECK((size_t)r == want, "len %d != expected %d fmt=\"%s\"", r, (int)want,
          fmt);
    CHECK(mem_cmp(buf, ref, (size_t)r) == 0,
          "truncated output not a prefix of full output fmt=\"%s\"", fmt);
  }
}

/* NOTE: A format that ends right after '%' must not read past its NUL. The
 * bytes after terminator are 'Z'; they must never show up in output. */
static void fuzz_printf_trailing_percent(void) {
  g_target = "printf-trailing-%";
  static const char *const tails[] = {"%", "%5", "%-", "%0", "%l", "%12"};
  for (unsigned t = 0; t < sizeof(tails) / sizeof(tails[0]); t++) {
    char fmtbuf[16];
    size_t tl = nl_strlen(tails[t]);

    fmtbuf[0] = 'a';
    nl_memcpy(fmtbuf + 1, tails[t], tl);
    fmtbuf[1 + tl] = '\0';
    for (int i = 1 + (int)tl + 1; i < 16; i++) {
      fmtbuf[i] = 'Z';
    }

    char out[32];
    nl_snprintf(out, sizeof(out), fmtbuf);
    int leaked = 0;
    for (size_t i = 0; out[i]; i++) {
      if (out[i] == 'Z') {
        leaked = 1;
      }
    }

    CHECK(!leaked, "fmt \"a%s\" read past NUL, got \"%s\"", tails[t], out);
  }
}

// allocator

#define MAXLIVE 192
static struct {
  unsigned char *p;
  size_t n;
  unsigned char pat;
} live[MAXLIVE];

static void fill(unsigned char *p, size_t n, unsigned char pat) {
  for (size_t i = 0; i < n; i++) {
    p[i] = (unsigned char)(pat + i);
  }
}

static int verify(const unsigned char *p, size_t n, unsigned char pat) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] != (unsigned char)(pat + i))
      return 0;
  }

  return 1;
}

static void fuzz_alloc_all(long iters) {
  g_target = "alloc";
  for (g_iter = 0; g_iter < iters; g_iter++) {
    unsigned slot = rnd_n(MAXLIVE);
    if (live[slot].p) {
      CHECK(verify(live[slot].p, live[slot].n, live[slot].pat),
            "contents corrupted before free n=%d", (int)live[slot].n);

      nl_free(live[slot].p);

      live[slot].p = (unsigned char *)0;
      continue;
    }

    unsigned cls = rnd_n(100);
    size_t n = cls < 60   ? 1 + rnd_n(64)
               : cls < 90 ? 1 + rnd_n(4096)
               : cls < 99
                   ? 1 + rnd_n(60000)
                   : (size_t)(1024 * 1024) + rnd_n(300000); /* mmap path */
    int zero = rnd_n(4) == 0;
    unsigned char *p = zero ? nl_calloc(n, 1) : nl_malloc(n);
    if (!p) {
      continue; // OOM is legal; not corruption
    }

    CHECK(((unsigned long)p & 15) == 0, "misaligned %p", (void *)p);
    if (zero) {
      int allz = 1;
      for (size_t i = 0; i < n; i++) {
        if (p[i]) {
          allz = 0;
        }
      }

      CHECK(allz, "calloc(%d) not zeroed", (int)n);
    }
    for (int k = 0; k < MAXLIVE; k++) {
      if (!live[k].p) {
        continue;
      }

      CHECK(p + n <= live[k].p || live[k].p + live[k].n <= p,
            "overlap new=[%p,+%d) old=[%p,+%d)", (void *)p, (int)n,
            (void *)live[k].p, (int)live[k].n);
    }

    live[slot].p = p;
    live[slot].n = n;
    live[slot].pat = (unsigned char)rnd();
    fill(p, n, live[slot].pat);
  }

  for (int k = 0; k < MAXLIVE; k++)
    if (live[k].p) {
      CHECK(verify(live[k].p, live[k].n, live[k].pat), "final corruption");

      nl_free(live[k].p);

      live[k].p = (unsigned char *)0;
    }

  // everything freed => arena must have fully coalesced back to one block
  void *a = nl_malloc(900 * 1024);
  void *b = nl_malloc(900 * 1024);
  CHECK(a && b, "arena fragmented after freeing everything (a=%p b=%p)", a, b);

  nl_free(a);
  nl_free(b);
}

// Absurd sizes must fail (NULL), never succeed with a tiny block
static void fuzz_alloc_overflow(void) {
  g_target = "alloc-overflow";
  static const size_t huge[] = {
      SIZE_MAX,      SIZE_MAX - 1,     SIZE_MAX - 15,   SIZE_MAX - 16,
      SIZE_MAX - 31, SIZE_MAX / 2 + 1, (size_t)1 << 62, (size_t)1 << 47};

  for (unsigned i = 0; i < sizeof(huge) / sizeof(huge[0]); i++) {
    void *p = nl_malloc(huge[i]);

    CHECK(p == (void *)0, "nl_malloc(%lu) returned %p (should be NULL)",
          (unsigned long long)huge[i], p);

    if (p) {
      nl_free(p);
    }
  }
}

// getopt

static void fuzz_getopt_one(void) {
  static const char *const toks[] = {"-a",  "-b", "-c", "-ab", "-bval",
                                     "-cx", "-",  "--", "--x", "val",
                                     "",    "-z", "-:", "-W",  "x"};
  static const char *const optstrs[] = {"abc:", "ab:c", "a:b:c:", "abc",
                                        ":a",   "",     "W:"};
  const char *av[12];
  int argc = 1 + (int)rnd_n(10);
  av[0] = "prog";
  for (int i = 1; i < argc; i++) {
    av[i] = toks[rnd_n(sizeof(toks) / sizeof(toks[0]))];
  }

  av[argc] = (const char *)0;
  const char *opts = optstrs[rnd_n(sizeof(optstrs) / sizeof(optstrs[0]))];

  nl_optind = 1;
  nl_opterr = 0; // keep stderr quiet
  int steps = 0, prev = nl_optind;
  for (;;) {
    int c = nl_getopt(argc, (char *const *)av, opts);
    if (c == -1) {
      break;
    }

    steps++;
    CHECK(steps <= 64, "no termination argc=%d opts=\"%s\"", argc, opts);
    if (steps > 64) {
      break;
    }

    CHECK(nl_optind >= prev, "optind went backwards %d -> %d", prev, nl_optind);
    CHECK(nl_optind <= argc + 1, "optind %d beyond argc %d", nl_optind, argc);
    prev = nl_optind;
  }

  // getopt keeps static position state; park it so next case starts clean
  nl_optind = argc;
  (void)nl_getopt(argc, (char *const *)av, opts);
}

// main

static unsigned long long parse_u(const char *s, unsigned long long dflt) {
  if (!s || !*s) {
    return dflt;
  }

  unsigned long long v = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (unsigned long long)(*s++ - '0');
  }

  return v;
}

int main(int argc, char **argv) {
  long iters = (long)parse_u(argc > 1 ? argv[1] : (char *)0, 20000);
  g_seed = parse_u(argc > 2 ? argv[2] : (char *)0, 1);
  rng_state = g_seed * 0x9E3779B97F4A7C15ULL + 1;

  g_target = "printf";
  for (g_iter = 0; g_iter < iters; g_iter++) {
    fuzz_printf_one();
  }

  fuzz_printf_trailing_percent();

  fuzz_alloc_overflow();
  fuzz_alloc_all(iters);

  g_target = "getopt";
  for (g_iter = 0; g_iter < iters; g_iter++) {
    fuzz_getopt_one();
  }

  fprintf(stderr, "fuzz_nolibc: seed=%u iters=%d failures=%d\n",
          (unsigned)g_seed, (int)iters, g_fails);

  return g_fails ? 1 : 0;
}
