#ifndef NL_RAMFS_H
#define NL_RAMFS_H

/*
 * nl_ramfs - read-only archive of named blobs, meant to be linked into binary
 * (plugins, charsets) and looked up by name at run time
 *
 * reader (nl_ramfs_init / _find / _entry), packer (nl_ramfs_pack)
 *
 * Layout, all integers little endian, offsets counted from archive start:
 *
 *    0  "NLRF"
 *    4  u16 version (1)
 *    6  u16 reserved (0)
 *    8  u32 entry count
 *   12  u32 total size (<= bytes handed to nl_ramfs_init)
 *   16  entry[count], 16 bytes each:
 *         u32 name offset, u32 name length (without NUL),
 *         u32 data offset (multiple of 16), u32 data length
 *       names, each NUL terminated
 *       data blobs, each starting on a 16 byte boundary, zero padded
 *
 * Archive is not trusted more than a plugin file is: nl_ramfs_init checks every
 * offset and length once, and lookups after it need no further checks
 * Reads are bytewise, so archive needs no particular alignment
 *
 * Linux (ELF) only for built-in archive: src/ramfs_blob.S defines
 * nl_ramfs_blob_start / nl_ramfs_blob_end, nl_ramfs_builtin() finds them
 * They are weak references, so a build without blob links fine and
 * nl_ramfs_builtin() just fails
 */

#ifdef NL_RAMFS_HOSTED /* tools/mkramfs.c: ordinary libc program */
#include <stddef.h>
#include <stdint.h>
#else
#include "nl_types.h"
#endif

#define NL_RAMFS_VERSION 1
#define NL_RAMFS_HDR_SIZE 16
#define NL_RAMFS_ENT_SIZE 16
#define NL_RAMFS_ALIGN 16
#define NL_RAMFS_MAX_ENTRIES 4096
#define NL_RAMFS_MAX_NAME 255
#define NL_RAMFS_MAX_SIZE (1ULL << 30)

typedef struct {
  const char *name; // e.g. "plugins/invert.so"
  const void *data;
  size_t len;
} nl_ramfs_src_t;

typedef struct {
  const uint8_t *base;
  size_t size;
  uint32_t count;
} nl_ramfs_t;

static inline unsigned nl_ramfs_rd16_(const uint8_t *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static inline unsigned nl_ramfs_rd32_(const uint8_t *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
         ((unsigned)p[3] << 24);
}

static inline void nl_ramfs_wr32_(uint8_t *p, unsigned long long v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static inline size_t nl_ramfs_strlen_(const char *s) {
  size_t n = 0;
  while (s[n]) {
    n++;
  }

  return n;
}

static inline int nl_ramfs_streq_(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }

  return *a == *b;
}

static inline unsigned long long nl_ramfs_align_(unsigned long long v) {
  return (v + (NL_RAMFS_ALIGN - 1)) & ~(unsigned long long)(NL_RAMFS_ALIGN - 1);
}

// Validate an archive in memory
// Return 0 and fs filled in, or -1 and fs empty
static inline int nl_ramfs_init(nl_ramfs_t *fs, const void *img, size_t len) {
  const uint8_t *b = (const uint8_t *)img;
  fs->base = (const uint8_t *)0;
  fs->size = 0;
  fs->count = 0;

  if (!b || len < NL_RAMFS_HDR_SIZE) {
    return -1;
  }

  if (b[0] != 'N' || b[1] != 'L' || b[2] != 'R' || b[3] != 'F') {
    return -1;
  }

  if (nl_ramfs_rd16_(b + 4) != NL_RAMFS_VERSION || nl_ramfs_rd16_(b + 6) != 0) {
    return -1;
  }

  // 64 bit arithmetic below: every operand is a u32 or a small constant, so
  // sums and products of a few of them cannot wrap
  unsigned long long count = nl_ramfs_rd32_(b + 8);
  unsigned long long total = nl_ramfs_rd32_(b + 12);
  if (count > NL_RAMFS_MAX_ENTRIES || total > (unsigned long long)len ||
      total > NL_RAMFS_MAX_SIZE) {
    return -1;
  }

  unsigned long long tab_end = NL_RAMFS_HDR_SIZE + count * NL_RAMFS_ENT_SIZE;
  if (tab_end > total) {
    return -1;
  }

  for (unsigned long long i = 0; i < count; i++) {
    const uint8_t *e = b + NL_RAMFS_HDR_SIZE + i * NL_RAMFS_ENT_SIZE;
    unsigned long long noff = nl_ramfs_rd32_(e);
    unsigned long long nlen = nl_ramfs_rd32_(e + 4);
    unsigned long long doff = nl_ramfs_rd32_(e + 8);
    unsigned long long dlen = nl_ramfs_rd32_(e + 12);

    if (nlen == 0 || nlen > NL_RAMFS_MAX_NAME || noff < tab_end ||
        noff + nlen + 1 > total || b[noff + nlen] != 0) {
      return -1;
    }

    for (unsigned long long j = 0; j < nlen; j++) {
      if (b[noff + j] == 0) {
        return -1;
      }
    }

    if (doff % NL_RAMFS_ALIGN || doff > total || dlen > total - doff) {
      return -1;
    }
  }

  fs->base = b;
  fs->size = (size_t)total;
  fs->count = (uint32_t)count;

  return 0;
}

// Entry i (0 <= i < count)
// Any output pointer may be NULL name is NUL terminated, data points into the
// archive
static inline int nl_ramfs_entry(const nl_ramfs_t *fs, uint32_t i,
                                 const char **name, const void **data,
                                 size_t *len) {
  if (!fs->base || i >= fs->count) {
    return -1;
  }

  const uint8_t *e =
      fs->base + NL_RAMFS_HDR_SIZE + (size_t)i * NL_RAMFS_ENT_SIZE;
  if (name) {
    *name = (const char *)(fs->base + nl_ramfs_rd32_(e));
  }

  if (data) {
    *data = fs->base + nl_ramfs_rd32_(e + 8);
  }

  if (len) {
    *len = nl_ramfs_rd32_(e + 12);
  }

  return 0;
}

// Look up an entry by exact name
// Return 0 and data/len filled in, or -1
static inline int nl_ramfs_find(const nl_ramfs_t *fs, const char *name,
                                const void **data, size_t *len) {
  for (uint32_t i = 0; i < fs->count; i++) {
    const char *n = (const char *)0;
    nl_ramfs_entry(fs, i, &n, data, len);
    if (n && nl_ramfs_streq_(n, name)) {
      return 0;
    }
  }

  return -1;
}

// Build an archive in out[0..cap)
// Returns size the archive needs
// It was written only if that is <= cap (call with out == NULL to ask for the
// size), 0 if input is unusable: too many entries, empty / too long / duplicate
// name, NULL data with len > 0, or archive over NL_RAMFS_MAX_SIZE
// Entries keep order given, so a caller wanting reproducible output sorts them
// first */
static inline size_t nl_ramfs_pack(uint8_t *out, size_t cap,
                                   const nl_ramfs_src_t *src, uint32_t n) {
  if (n > NL_RAMFS_MAX_ENTRIES) {
    return 0;
  }

  unsigned long long names =
      NL_RAMFS_HDR_SIZE + (unsigned long long)n * NL_RAMFS_ENT_SIZE;
  unsigned long long cur = names;
  for (uint32_t i = 0; i < n; i++) {
    if (!src[i].name) {
      return 0;
    }

    size_t nl = nl_ramfs_strlen_(src[i].name);
    if (nl == 0 || nl > NL_RAMFS_MAX_NAME) {
      return 0;
    }

    for (uint32_t j = 0; j < i; j++) {
      if (nl_ramfs_streq_(src[j].name, src[i].name)) {
        return 0;
      }
    }

    cur += nl + 1;
  }

  unsigned long long data_start = nl_ramfs_align_(cur);
  unsigned long long total = data_start;
  for (uint32_t i = 0; i < n; i++) {
    if ((!src[i].data && src[i].len) || src[i].len > NL_RAMFS_MAX_SIZE) {
      return 0;
    }

    total = nl_ramfs_align_(total + src[i].len);
    if (total > NL_RAMFS_MAX_SIZE) {
      return 0;
    }
  }

  if (!out || (unsigned long long)cap < total) {
    return (size_t)total;
  }

  for (unsigned long long i = 0; i < total; i++) {
    out[i] = 0;
  }

  out[0] = 'N';
  out[1] = 'L';
  out[2] = 'R';
  out[3] = 'F';
  out[4] = NL_RAMFS_VERSION;
  nl_ramfs_wr32_(out + 8, n);
  nl_ramfs_wr32_(out + 12, total);

  unsigned long long noff = names;
  unsigned long long doff = data_start;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t *e = out + NL_RAMFS_HDR_SIZE + (size_t)i * NL_RAMFS_ENT_SIZE;
    size_t nl = nl_ramfs_strlen_(src[i].name);
    nl_ramfs_wr32_(e, noff);
    nl_ramfs_wr32_(e + 4, nl);
    nl_ramfs_wr32_(e + 8, doff);
    nl_ramfs_wr32_(e + 12, src[i].len);

    for (size_t k = 0; k < nl; k++) {
      out[noff + k] = (uint8_t)src[i].name[k];
    }

    const uint8_t *d = (const uint8_t *)src[i].data;
    for (size_t k = 0; k < src[i].len; k++) {
      out[doff + k] = d[k];
    }

    noff += nl + 1;
    doff = nl_ramfs_align_(doff + src[i].len);
  }

  return (size_t)total;
}

#if defined(__ELF__) && defined(__GNUC__)
extern const uint8_t nl_ramfs_blob_start[] __attribute__((weak));
extern const uint8_t nl_ramfs_blob_end[] __attribute__((weak));

// Archive linked in by src/ramfs_blob.S
// Return -1 if none was linked or it is damaged
static inline int nl_ramfs_builtin(nl_ramfs_t *fs) {
  const uintptr_t start_addr = (uintptr_t)nl_ramfs_blob_start;
  const uintptr_t end_addr = (uintptr_t)nl_ramfs_blob_end;

  if (start_addr == 0 || end_addr == 0 || end_addr < start_addr) {
    fs->base = (const uint8_t *)0;
    fs->size = 0;
    fs->count = 0;

    return -1;
  }

  return nl_ramfs_init(fs, (const void *)start_addr,
                       (size_t)(end_addr - start_addr));
}
#else
static inline int nl_ramfs_builtin(nl_ramfs_t *fs) {
  fs->base = (const uint8_t *)0;
  fs->size = 0;
  fs->count = 0;

  return -1;
}
#endif

#endif /* NL_RAMFS_H */
