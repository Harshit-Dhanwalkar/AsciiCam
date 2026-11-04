#include "nl_types.h"

/*
 * Freestanding implementations of four mem* functions C standard requires even
 * in a freestanding environment (C99 7.1.4 / C11 5.1.2.1)
 *
 * gcc at -O2 emits calls to these for:
 *   - large aggregate initializations (e.g. `char buf[65536] = {0}`)
 *   - struct copies, block moves
 *   - loop-distribution optimization (recognising a manual zero-fill loop
 *     and replacing it with memset)
 *
 * NOTE:: this file must be compiled with -fno-tree-loop-distribute-patterns
 * (see Makefile rule for lib_nl_mem.o). Without it, gcc recognises loops below
 * as memset/memcpy patterns and replaces them with calls to very functions
 * being defined -> infinite recursion at runtime
 */

void *memset(void *s, int c, size_t n) {
  unsigned char *p = (unsigned char *)s;
  unsigned char b = (unsigned char)c;
  while (n--) {
    *p++ = b;
  }

  return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dest;
  const unsigned char *s = (const unsigned char *)src;
  while (n--) {
    *d++ = *s++;
  }

  return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dest;
  const unsigned char *s = (const unsigned char *)src;
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
  const unsigned char *a = (const unsigned char *)s1;
  const unsigned char *b = (const unsigned char *)s2;
  while (n--) {
    if (*a != *b) {
      return (int)*a - (int)*b;
    }

    a++;
    b++;
  }

  return 0;
}
