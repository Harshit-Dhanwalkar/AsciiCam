#ifndef NL_CHECKED_H
#define NL_CHECKED_H

/*
 * Overflow-checked arithmetic on sizes: nl_size_add/sub/mul/align_up return 0
 * and store result, or return -1 and leave *out untouched when true result does
 * not fit in size_t
 *
 * Use these wherever a size comes from a caller, a file or network: an
 * unchecked `n * size` or `n + header` that wraps turns into a tiny allocation
 * followed by a large write (nl_malloc(SIZE_MAX) once did exactly that)
 *
 * Header-only, GCC/Clang builtins, no libc
 */

#include "nl_types.h" // size_t/uint8_t: own definitions on Linux, system ones elsewhere

static inline int nl_size_add(size_t a, size_t b, size_t *out) {
  size_t r;
  if (__builtin_add_overflow(a, b, &r)) {
    return -1;
  }

  *out = r;

  return 0;
}

static inline int nl_size_sub(size_t a, size_t b, size_t *out) {
  size_t r;
  if (__builtin_sub_overflow(a, b, &r)) {
    return -1;
  }

  *out = r;

  return 0;
}

static inline int nl_size_mul(size_t a, size_t b, size_t *out) {
  size_t r;
  if (__builtin_mul_overflow(a, b, &r)) {
    return -1;
  }

  *out = r;

  return 0;
}

// Round n up to a multiple of `align` (a power of two)
// -1 on overflow or if align is not a power of two
static inline int nl_size_align_up(size_t n, size_t align, size_t *out) {
  if (align == 0 || (align & (align - 1)) != 0) {
    return -1;
  }

  size_t t;
  if (nl_size_add(n, align - 1, &t) < 0) {
    return -1;
  }

  *out = t & ~(align - 1);

  return 0;
}

#endif
