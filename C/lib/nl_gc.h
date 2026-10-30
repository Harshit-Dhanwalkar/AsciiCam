#ifndef NL_GC_H
#define NL_GC_H

/*
 * nl_gc - opt-in conservative garbage collector for nolibc heap (Linux)
 *
 * nl_free() on a collector block is allowed and frees it at once; calling it on
 * anything that is not a live block start (already freed or collected, interior
 * pointer, garbage) is detected and ignored, and counted in nl_gc_stats()
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

// Zeroed block that is scanned for pointers and freed when unreachable
// NULL only if memory is exhausted even after collecting
void *nl_gc_alloc(size_t n);

// Same, for data that contains no pointers (never scanned)
void *nl_gc_alloc_atomic(size_t n);

// Give a block back early
// Optional on Linux (an unreleased block is collected later; releasing a block
// twice, or one that was already collected, is ignored), required elsewhere,
// where there is no collector
// Write code to release what it allocates and it is leak-free on every platform
// and error-proof on Linux
void nl_free(void *p);
#define nl_gc_release(p) nl_free(p)

// Run a collection now
// Returns bytes reclaimed (0 when it did nothing: disabled, called from a
// thread that does not own collector or stack base is unknown)
size_t nl_gc_collect(void);

// Automatic run once this many bytes were allocated since last one
// 0 = only on demand / out of memory (default)
void nl_gc_set_threshold(size_t bytes);

// Nestable; while disabled nl_gc_collect() does nothing and allocations that
// fail simply fail
void nl_gc_disable(void);
void nl_gc_enable(void);

// Extra root: [p, p + len) is scanned like a variable
// Returns 0, or -1 if table (16 entries) is full
int nl_gc_root_add(const void *p, size_t len);
void nl_gc_root_remove(const void *p);

void nl_gc_stats(nl_gc_stats_t *out);

#endif
