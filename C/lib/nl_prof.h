/*
 * lib/nl_prof.h - per-stage frame timing with percentiles, no allocation
 *
 *   nl_prof_t p;  nl_prof_init(&p);
 *   nl_prof_restart(&p, now_ns);          // start of a frame (or after a skip)
 *   ... stage A ...  nl_prof_mark(&p, STAGE_A, now_ns);
 *   ... stage B ...  nl_prof_mark(&p, STAGE_B, now_ns);
 *
 * mark() charges time since previous mark (or restart) to a stage
 * Per stage it keeps count, sum, min, max and a histogram with 4 buckets per
 * power of two (256 buckets, 8 bytes each => 2 KiB per stage), so p50/p99 are
 * within about 13% of true value over whole 1 ns .. 584 years range without
 * storing samples
 *
 * Time comes from caller, so module has no clock of its own and tests drive it
 * with made-up numbers
 */

#ifndef NL_PROF_H
#define NL_PROF_H

#include "nl_types.h"

#define NL_PROF_STAGES 8
#define NL_PROF_BUCKETS 256

typedef struct {
  uint64_t count;
  uint64_t sum;
  uint64_t min;
  uint64_t max;
  uint32_t hist[NL_PROF_BUCKETS];
} nl_prof_stage_t;

typedef struct {
  nl_prof_stage_t st[NL_PROF_STAGES];
  uint64_t last;
} nl_prof_t;

static inline void nl_prof_init(nl_prof_t *p) {
  char *b = (char *)p;
  for (size_t i = 0; i < sizeof(*p); i++) {
    b[i] = 0;
  }
  for (int i = 0; i < NL_PROF_STAGES; i++) {
    p->st[i].min = ~(uint64_t)0;
  }
}

static inline void nl_prof_restart(nl_prof_t *p, uint64_t now) {
  p->last = now;
}

// Bucket of a duration: 4 per power of two. 0 and 1 share bucket 0
static inline int nl_prof_bucket(uint64_t v) {
  if (v < 4) {
    return (int)v; // 0..3 are exact
  }

  int msb = 63 - __builtin_clzll(v);
  int sub = (int)((v >> (msb - 2)) & 3);

  return (msb - 1) * 4 + sub; // msb 2 -> 4..7, msb 63 -> 248..251
}

// Smallest value that lands in bucket b
static inline uint64_t nl_prof_bucket_low(int b) {
  if (b < 4) {
    return (uint64_t)b;
  }

  int msb = b / 4 + 1;
  uint64_t sub = (uint64_t)(b % 4);

  return ((uint64_t)1 << msb) | (sub << (msb - 2));
}

static inline void nl_prof_mark(nl_prof_t *p, int stage, uint64_t now) {
  if (stage < 0 || stage >= NL_PROF_STAGES) {
    return;
  }

  uint64_t d = now > p->last ? now - p->last : 0;
  p->last = now;

  nl_prof_stage_t *s = &p->st[stage];
  s->count++;
  s->sum += d;
  if (d < s->min) {
    s->min = d;
  }
  if (d > s->max) {
    s->max = d;
  }
  s->hist[nl_prof_bucket(d)]++;
}

// Value at fraction num/den of samples (e.g. 99/100); bucket midpoint, clamped
// into [min, max]
// Return 0 when stage has no samples
static inline uint64_t nl_prof_quantile(const nl_prof_stage_t *s, uint64_t num,
                                        uint64_t den) {
  if (s->count == 0) {
    return 0;
  }

  uint64_t want = (s->count * num + den - 1) / den; // Rank, 1-based
  if (want < 1) {
    want = 1;
  }

  uint64_t seen = 0;
  for (int b = 0; b < NL_PROF_BUCKETS; b++) {
    seen += s->hist[b];
    if (seen >= want) {
      uint64_t lo = nl_prof_bucket_low(b);
      uint64_t hi = b < 250 ? nl_prof_bucket_low(b + 1) : lo;
      uint64_t v = lo + (hi - lo) / 2;
      if (b < 4) {
        v = lo; // Exact buckets
      }
      if (v < s->min) {
        v = s->min;
      }
      if (v > s->max) {
        v = s->max;
      }

      return v;
    }
  }

  return s->max;
}

#endif
