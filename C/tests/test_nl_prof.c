/*
 * tests/test_nl_prof.c - lib/nl_prof.h
 *
 * Bucket mapping is checked for every value around each power of two and
 * for monotonic, gap-free coverage; percentiles are compared with an exact
 * sort of same random samples; accounting is checked against known sums
 */

#include "../lib/nl_prof.h"
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

static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static unsigned long long rnd64(void) {
  rng ^= rng >> 12;
  rng ^= rng << 25;
  rng ^= rng >> 27;

  return rng * 0x2545F4914F6CDD1DULL;
}

static nl_prof_t P;
static uint64_t samples[20000];

static int cmp_u64(const uint64_t *a, const uint64_t *b) {
  return (*a > *b) - (*a < *b);
}

static void sort_u64(uint64_t *a, int n) { // insertion-free shell sort
  for (int gap = n / 2; gap > 0; gap /= 2) {
    for (int i = gap; i < n; i++) {
      uint64_t t = a[i];
      int j = i;
      for (; j >= gap && cmp_u64(&a[j - gap], &t) > 0; j -= gap) {
        a[j] = a[j - gap];
      }
      a[j] = t;
    }
  }
}

static int within(uint64_t got, uint64_t want, int pct) {
  uint64_t tol = want * (uint64_t)pct / 100;
  if (tol < 1) {
    tol = 1;
  }

  return got + tol >= want && got <= want + tol;
}

int main(void) {
  // Every bucket's lower bound maps back to itself; one below maps to previous
  // bucket; buckets never go backwards
  int prev = -1;
  for (int b = 0; b < 252; b++) {
    uint64_t lo = nl_prof_bucket_low(b);
    CHECK(nl_prof_bucket(lo) == b, "low(%d)=%llu maps to %d", b,
          (unsigned long long)lo, nl_prof_bucket(lo));
    if (b > 0) {
      CHECK(nl_prof_bucket(lo - 1) == b - 1, "below low(%d) maps to %d", b,
            nl_prof_bucket(lo - 1));
    }
    CHECK(b > prev, "monotonic");
    prev = b;
  }
  CHECK(nl_prof_bucket(~(uint64_t)0) == 251, "max value lands in last bucket");

  // Accounting: mark charges time since previous mark
  nl_prof_init(&P);
  nl_prof_restart(&P, 1000);
  nl_prof_mark(&P, 0, 1500); // 500 to stage 0
  nl_prof_mark(&P, 1, 1600); // 100 to stage 1
  nl_prof_mark(&P, 0, 3600); // 2000 to stage 0
  CHECK(P.st[0].count == 2 && P.st[0].sum == 2500, "stage 0 sum");
  CHECK(P.st[0].min == 500 && P.st[0].max == 2000, "stage 0 min/max");
  CHECK(P.st[1].count == 1 && P.st[1].sum == 100, "stage 1 sum");
  CHECK(P.st[2].count == 0 && nl_prof_quantile(&P.st[2], 1, 2) == 0,
        "empty stage");

  // Clock that steps back never underflows; bad stage ids are ignored
  nl_prof_mark(&P, 2, 10);
  CHECK(P.st[2].count == 1 && P.st[2].sum == 0, "backwards clock gives 0");
  nl_prof_mark(&P, -1, 50);
  nl_prof_mark(&P, NL_PROF_STAGES, 60);
  uint64_t total = 0;
  for (int i = 0; i < NL_PROF_STAGES; i++) {
    total += P.st[i].count;
  }
  CHECK(total == 4, "out-of-range stage ignored (count %llu)",
        (unsigned long long)total);

  // Single sample: every quantile is that sample
  nl_prof_init(&P);
  nl_prof_restart(&P, 0);
  nl_prof_mark(&P, 3, 123456);
  CHECK(nl_prof_quantile(&P.st[3], 1, 2) == 123456, "p50 of one sample");
  CHECK(nl_prof_quantile(&P.st[3], 99, 100) == 123456, "p99 of one sample");

  // Percentiles against an exact sort, over several distributions
  for (int dist = 0; dist < 4; dist++) {
    int n = 20000;
    nl_prof_init(&P);
    nl_prof_restart(&P, 0);
    uint64_t t = 0;
    for (int i = 0; i < n; i++) {
      uint64_t r = rnd64();
      uint64_t v;
      switch (dist) {
      case 0:
        v = 1000 + r % 9000; // 1..10 us
        break;
      case 1:
        v = 1 + (r >> 40) % 40000000; // up to 40 ms, wide
        break;
      case 2:
        v = (r % 100 == 0) ? 30000000 : 2000000; // steady + spikes
        break;
      default:
        v = (uint64_t)1 << (r % 40); // log-uniform
        break;
      }
      samples[i] = v;
      t += v;
      nl_prof_mark(&P, 0, t);
    }
    sort_u64(samples, n);
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
      sum += samples[i];
    }
    CHECK(P.st[0].sum == sum && P.st[0].count == (uint64_t)n, "dist %d sum",
          dist);
    CHECK(P.st[0].min == samples[0] && P.st[0].max == samples[n - 1],
          "dist %d min/max", dist);
    static const uint64_t q[3][2] = {{1, 2}, {95, 100}, {99, 100}};
    for (int k = 0; k < 3; k++) {
      uint64_t rank = ((uint64_t)n * q[k][0] + q[k][1] - 1) / q[k][1];
      uint64_t want = samples[rank - 1];
      uint64_t got = nl_prof_quantile(&P.st[0], q[k][0], q[k][1]);
      CHECK(within(got, want, 15), "dist %d q%llu/%llu got %llu want %llu",
            dist, (unsigned long long)q[k][0], (unsigned long long)q[k][1],
            (unsigned long long)got, (unsigned long long)want);
    }
  }

  fprintf(stderr, "nl_prof: %d passed, %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
