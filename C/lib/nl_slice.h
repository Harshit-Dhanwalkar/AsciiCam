#ifndef NL_SLICE_H
#define NL_SLICE_H

/*
 * Slices: a pointer and a length that travel together, with bounds-checked
 * access
 * NOTE: Rust idea (&[u8], &str) without language: no function here reads or
 * writes outside [ptr, ptr+len) and none truncates silently - a request that
 * does not fit returns -1 and changes nothing
 *
 *   nl_slice_t   read-only bytes            nl_mslice_t  writable bytes
 *   nl_str_t     read-only text, NOT NUL-terminated (length is explicit)
 *
 * Sub-slices share memory with their parent
 * A slice does not own anything and does not outlive what it points at
 */

#include "nl_checked.h"
#include "nl_types.h" // size_t/uint8_t: own definitions on Linux, system ones elsewhere

#define NL_NPOS ((size_t)-1) // "not found"

typedef struct {
  const uint8_t *ptr;
  size_t len;
} nl_slice_t;

typedef struct {
  uint8_t *ptr;
  size_t len;
} nl_mslice_t;

typedef struct {
  const char *ptr;
  size_t len;
} nl_str_t;

// Bytes
#define NL_SLICE_OF(arr) ((nl_slice_t){(const uint8_t *)(arr), sizeof(arr)})
#define NL_MSLICE_OF(arr) ((nl_mslice_t){(uint8_t *)(arr), sizeof(arr)})

static inline nl_slice_t nl_slice_make(const void *p, size_t n) {
  nl_slice_t s = {(const uint8_t *)p, n};

  return s;
}

static inline nl_slice_t nl_mslice_ro(nl_mslice_t m) {
  nl_slice_t s = {m.ptr, m.len};

  return s;
}

// [off, off+len) of s. -1 (and *out untouched) unless it lies entirely inside
static inline int nl_slice_sub(nl_slice_t s, size_t off, size_t len,
                               nl_slice_t *out) {
  if (off > s.len || len > s.len - off) {
    return -1;
  }

  out->ptr = s.ptr + off;
  out->len = len;

  return 0;
}

static inline int nl_mslice_sub(nl_mslice_t s, size_t off, size_t len,
                                nl_mslice_t *out) {
  if (off > s.len || len > s.len - off) {
    return -1;
  }

  out->ptr = s.ptr + off;
  out->len = len;

  return 0;
}

static inline int nl_slice_get(nl_slice_t s, size_t i, uint8_t *out) {
  if (i >= s.len) {
    return -1;
  }

  *out = s.ptr[i];

  return 0;
}

static inline int nl_mslice_set(nl_mslice_t s, size_t i, uint8_t v) {
  if (i >= s.len) {
    return -1;
  }

  s.ptr[i] = v;

  return 0;
}

static inline int nl_slice_eq(nl_slice_t a, nl_slice_t b) {
  if (a.len != b.len) {
    return 0;
  }

  for (size_t i = 0; i < a.len; i++) {
    if (a.ptr[i] != b.ptr[i]) {
      return 0;
    }
  }

  return 1;
}

static inline size_t nl_slice_find_byte(nl_slice_t s, uint8_t b) {
  for (size_t i = 0; i < s.len; i++) {
    if (s.ptr[i] == b) {
      return i;
    }
  }

  return NL_NPOS;
}

// Copy src into start of dst. -1 if it does not fit: never a partial copy
// Overlap-safe (memmove semantics)
static inline int nl_mslice_copy(nl_mslice_t dst, nl_slice_t src) {
  if (src.len > dst.len) {
    return -1;
  }

  if (dst.ptr <= src.ptr) {
    for (size_t i = 0; i < src.len; i++) {
      dst.ptr[i] = src.ptr[i];
    }
  } else {
    for (size_t i = src.len; i > 0; i--) {
      dst.ptr[i - 1] = src.ptr[i - 1];
    }
  }

  return 0;
}

static inline void nl_mslice_fill(nl_mslice_t dst, uint8_t v) {
  for (size_t i = 0; i < dst.len; i++) {
    dst.ptr[i] = v;
  }
}

// Text
#define NL_STR_LIT(lit) ((nl_str_t){(lit), sizeof(lit) - 1})

static inline nl_str_t nl_str_make(const char *p, size_t n) {
  nl_str_t s = {p, n};

  return s;
}

// View of a NUL-terminated string (NULL -> empty); length excludes NUL
static inline nl_str_t nl_str_from_cstr(const char *c) {
  nl_str_t s = {c, 0};
  if (c) {
    while (c[s.len]) {
      s.len++;
    }
  }

  return s;
}

static inline int nl_str_eq(nl_str_t a, nl_str_t b) {
  return nl_slice_eq(nl_slice_make(a.ptr, a.len), nl_slice_make(b.ptr, b.len));
}

static inline int nl_str_starts_with(nl_str_t s, nl_str_t prefix) {
  return prefix.len <= s.len &&
         nl_str_eq(nl_str_make(s.ptr, prefix.len), prefix);
}

static inline int nl_str_ends_with(nl_str_t s, nl_str_t suffix) {
  return suffix.len <= s.len &&
         nl_str_eq(nl_str_make(s.ptr + (s.len - suffix.len), suffix.len),
                   suffix);
}

static inline size_t nl_str_find(nl_str_t s, char c) {
  for (size_t i = 0; i < s.len; i++) {
    if (s.ptr[i] == c) {
      return i;
    }
  }

  return NL_NPOS;
}

// Split at first `sep`: head = before, tail = after (sep itself dropped)
// Returns 0 if found; -1 if not (head = whole string, tail empty)
static inline int nl_str_split_once(nl_str_t s, char sep, nl_str_t *head,
                                    nl_str_t *tail) {
  size_t i = nl_str_find(s, sep);
  if (i == NL_NPOS) {
    *head = s;
    *tail = nl_str_make(s.ptr + s.len, 0);

    return -1;
  }

  *head = nl_str_make(s.ptr, i);
  *tail = nl_str_make(s.ptr + i + 1, s.len - i - 1);

  return 0;
}

static inline nl_str_t nl_str_trim(nl_str_t s) {
  while (s.len && (*s.ptr == ' ' || *s.ptr == '\t' || *s.ptr == '\n' ||
                   *s.ptr == '\r')) {
    s.ptr++;
    s.len--;
  }

  while (s.len && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t' ||
                   s.ptr[s.len - 1] == '\n' || s.ptr[s.len - 1] == '\r')) {
    s.len--;
  }

  return s;
}

// Copy `s` plus a NUL into dst. -1 if it (with NUL) does not fit: dst is then
// left as it was, instead of holding a silently truncated path or name
static inline int nl_str_copy_cstr(nl_mslice_t dst, nl_str_t s) {
  size_t need;
  if (nl_size_add(s.len, 1, &need) < 0 || need > dst.len) {
    return -1;
  }

  for (size_t i = 0; i < s.len; i++) {
    dst.ptr[i] = (uint8_t)s.ptr[i];
  }

  dst.ptr[s.len] = 0;

  return 0;
}

// Decimal unsigned integer: whole string must be digits (no sign, no spaces,
// not empty) and value must fit
// 0 and *out set, or -1
static inline int nl_str_to_size(nl_str_t s, size_t *out) {
  if (s.len == 0) {
    return -1;
  }

  size_t v = 0;
  for (size_t i = 0; i < s.len; i++) {
    if (s.ptr[i] < '0' || s.ptr[i] > '9') {
      return -1;
    }

    if (nl_size_mul(v, 10, &v) < 0 ||
        nl_size_add(v, (size_t)(s.ptr[i] - '0'), &v) < 0) {
      return -1;
    }
  }

  *out = v;

  return 0;
}

#endif
