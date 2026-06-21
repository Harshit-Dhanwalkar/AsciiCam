#ifndef NL_STRING_H
#define NL_STRING_H

#include <stddef.h>
#include <stdint.h>

static inline size_t nl_strlen(const char *s) {
  const char *p = s;
  while (*p)
    p++;
  return (size_t)(p - s);
}

static inline void *nl_memcpy(void *dst, const void *src, size_t n) {
  // uint8_t *d = (uint8_t *)dst;
  // const uint8_t *s = (const uint8_t *)src;
  // while (n--)
  //   *d++ = *s++;
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  for (size_t i = 0; i < n; i++) {
    d[i] = s[i];
  }
  return dst;
}

static inline void *nl_memset(void *dst, int c, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  while (n--)
    *d++ = (uint8_t)c;
  return dst;
}

static inline int nl_strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

static inline char *nl_strncpy_safe(char *dst, const char *src, size_t n) {
  if (n == 0)
    return dst;
  size_t i;
  for (i = 0; i < n - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = '\0';
  return dst;
}

static inline char *nl_basename(char *path) {
  char *p = path, *last = path;
  while (*p) {
    if (*p == '/')
      last = p + 1;
    p++;
  }
  return last;
}

static inline char *nl_dirname(char *path) {
  char *last = (char *)0, *p = path;
  while (*p) {
    if (*p == '/')
      last = p;
    p++;
  }
  if (!last) {
    path[0] = '.';
    path[1] = '\0';
  } else if (last == path) {
    path[1] = '\0';
  } else {
    *last = '\0';
  }
  return path;
}

static inline int nl_atoi(const char *s) {
  int n = 0, neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  }
  while (*s >= '0' && *s <= '9')
    n = n * 10 + (*s++ - '0');
  return neg ? -n : n;
}

#define strlen(s) nl_strlen(s)
#define memcpy(d, s, n) nl_memcpy(d, s, n)
#define memset(d, c, n) nl_memset(d, c, n)
#define strcmp(a, b) nl_strcmp(a, b)
#define strncpy(d, s, n) nl_strncpy_safe(d, s, n)
#define basename(p) nl_basename(p)
#define dirname(p) nl_dirname(p)
// #define atoi(s) nl_atoi(s)

#endif
