#include "nl_types.h"

/*
 * Freestanding implementations of four mem* functions C standard requires even
 * in a freestanding environment (C99 7.1.4 / C11 5.1.2.1)
 *
 * gcc at -O2 emits calls to these for:
 *   - large aggregate initializations (e.g. `char buf[65536] = {0}`)
 *   - struct copies, block moves
 *
 * The nolibc build has nl_memset / nl_memcpy etc., but gcc emits calls to
 * the standard names, so these wrappers must exist at link time.
 *
 * NOTE:: store/load pointers below are qualified `volatile` so that gcc's
 * -ftree-loop-distribute-patterns pass does not recognise these loops as
 * memset/memcpy patterns and replace them with calls to very functions being
 * defined (which would be infinitely recursive). Volatile stores are
 * side-effects compiler must preserve as written, so pattern matcher skips
 * them. This is portable: clang doesn't do this optimisation at all and
 * silently accepts volatile qualifier. No -fno-tree-loop-distribute-patterns
 * compiler flag is needed on either compiler, which matters because clang
 * rejects that flag as unknown
 */

void *memset(void *s, int c, size_t n) {
  volatile unsigned char *p = (volatile unsigned char *)s;
  unsigned char b = (unsigned char)c;
  while (n--) {
    *p++ = b;
  }

  return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
  volatile unsigned char *d = (volatile unsigned char *)dest;
  const volatile unsigned char *s = (const volatile unsigned char *)src;
  while (n--) {
    *d++ = *s++;
  }

  return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
  volatile unsigned char *d = (volatile unsigned char *)dest;
  const volatile unsigned char *s = (const volatile unsigned char *)src;
  if (d < s) {
    while (n--) {
      *d++ = *s++;
    }
  } else {
    d += n;
    s += n;
    while (n--) {
      *--d = *--s;
    }
  }

  return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const volatile unsigned char *a = (const volatile unsigned char *)s1;
  const volatile unsigned char *b = (const volatile unsigned char *)s2;
  while (n--) {
    if (*a != *b) {
      return (int)*a - (int)*b;
    }

    a++;
    b++;
  }

  return 0;
}
