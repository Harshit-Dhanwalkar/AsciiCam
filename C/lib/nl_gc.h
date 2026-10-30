#ifndef NL_GC_H
#define NL_GC_H

/*
 * nl_gc - opt-in conservative garbage collector for the nolibc heap (Linux)
 *
 * On Linux (lib/nl_gc.c) this is a real collector. Everywhere else there is
 * no collector: nl_gc_alloc / nl_gc_alloc_atomic are ordinary zeroed mallocs,
 * nl_gc_release is a real free, and the tuning entry points are no-ops
 * Code written against this API is leak-free on every platform
 *
 * nl_free() on a collector block is allowed and frees it at once; calling it
 * on anything that is not a live block start (already freed or collected,
 * interior pointer, garbage) is detected and ignored, and counted in
 * nl_gc_stats()
 * Freed blocks and collected blocks are cleared on allocation
 */

#include "nl_types.h"

typedef struct {
  size_t collections;    // runs so far
  size_t freed_blocks;   // blocks reclaimed by those runs
  size_t freed_bytes;    // ... and their payload bytes
  size_t last_marked;    // blocks found reachable in latest run
  size_t foreign_allocs; // nl_gc_alloc from a thread that does not own gc
  size_t live_blocks;    // all live blocks (collector and nl_malloc)
  size_t live_bytes;
  size_t invalid_frees; // nl_free of something that was not a live block
  size_t double_frees;  // ... of a block that was already free
} nl_gc_stats_t;

#ifdef __LINUX_NOLIBC__

/* Zeroed block that is scanned for pointers and freed when unreachable
 * NULL only if memory is exhausted even after collecting */
void *nl_gc_alloc(size_t n);

/* Same, for data that contains no pointers (never scanned) */
void *nl_gc_alloc_atomic(size_t n);

/* Give a block back early
 * Optional on Linux (an unreleased block is collected later; releasing twice or
 * after collection is ignored) */
void nl_free(void *p);

/* Run a collection now. Returns bytes reclaimed (0 when it did nothing) */
size_t nl_gc_collect(void);

/* Automatic run once this many bytes were allocated since last one 0 = only on
 * demand / out of memory (default) */
void nl_gc_set_threshold(size_t bytes);

/* Nestable; while disabled nl_gc_collect() does nothing */
void nl_gc_disable(void);
void nl_gc_enable(void);

/* Extra root: [p, p + len) is scanned like a variable
 * Returns 0, or -1 if the root table is full */
int nl_gc_root_add(const void *p, size_t len);
void nl_gc_root_remove(const void *p);

void nl_gc_stats(nl_gc_stats_t *out);

#define nl_gc_release(p) nl_free(p)

#else // MACOS / Windows: no collector

#include <stdlib.h>

static inline void *nl_gc_alloc(size_t n) { return calloc(1, n); }
static inline void *nl_gc_alloc_atomic(size_t n) { return calloc(1, n); }

static inline size_t nl_gc_collect(void) { return 0; }
static inline void nl_gc_set_threshold(size_t bytes) { (void)bytes; }
static inline void nl_gc_disable(void) {}
static inline void nl_gc_enable(void) {}

static inline int nl_gc_root_add(const void *p, size_t len) {
  (void)p;
  (void)len;

  return -1;
}
static inline void nl_gc_root_remove(const void *p) { (void)p; }

static inline void nl_gc_stats(nl_gc_stats_t *out) {
  unsigned char *b = (unsigned char *)out;

  for (size_t i = 0; i < sizeof *out; i++) {
    b[i] = 0;
  }
}

#define nl_gc_release(p) free(p)

#endif

#endif /* NL_GC_H */
