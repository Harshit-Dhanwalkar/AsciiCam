#include "nl_alloc.h"
#include "nl_io.h"

#include <stdint.h>

#define ARENA_SIZE (2 * 1024 * 1024)
#define ALIGN 16
#define HDR_MAGIC 0xDEAD
#define MMAP_THRESHOLD (ARENA_SIZE / 2) // 1 MB

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

typedef struct block_hdr {
  size_t size;              // 8
  int free;                 // 4
  unsigned short magic;     // 2
  unsigned char mmap_alloc; // 1  (1 if mmap'ed)
  unsigned char _pad[1];    // 1
} block_hdr_t;

static unsigned char _arena[ARENA_SIZE] __attribute__((aligned(ALIGN)));
static int _arena_init = 0;

static void arena_boot(void) {
  block_hdr_t *h = (block_hdr_t *)_arena;
  h->size = ARENA_SIZE - sizeof(block_hdr_t);
  h->free = 1;
  h->magic = HDR_MAGIC;
  h->mmap_alloc = 0;
  _arena_init = 1;
}

static inline size_t align_up(size_t n) {
  return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

void *nl_malloc(size_t n) {
  if (n == 0)
    n = 1;
  n = align_up(n);

  // Large allocation: use mmap
  if (n > MMAP_THRESHOLD) {
    size_t total = sizeof(block_hdr_t) + n;
    void *p = nl_mmap(NULL, total, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
      return NULL;
    block_hdr_t *h = (block_hdr_t *)p;
    h->size = n;
    h->free = 0;
    h->magic = HDR_MAGIC;
    h->mmap_alloc = 1;
    return (char *)p + sizeof(block_hdr_t);
  }

  // Else use arena
  if (!_arena_init)
    arena_boot();

  unsigned char *p = _arena;
  unsigned char *end = _arena + ARENA_SIZE;

  while (p + sizeof(block_hdr_t) <= end) {
    block_hdr_t *h = (block_hdr_t *)p;
    if (h->magic != HDR_MAGIC)
      break; // heap corruption

    if (h->free && h->size >= n) {
      size_t leftover = h->size - n;
      if (leftover > sizeof(block_hdr_t) + ALIGN) {
        block_hdr_t *next = (block_hdr_t *)(p + sizeof(block_hdr_t) + n);
        next->size = leftover - sizeof(block_hdr_t);
        next->free = 1;
        next->magic = HDR_MAGIC;
        next->mmap_alloc = 0;
        h->size = n;
      }
      h->free = 0;
      return p + sizeof(block_hdr_t);
    }
    p += sizeof(block_hdr_t) + h->size;
  }
  return (void *)0; // OOM
}

void *nl_calloc(size_t nmemb, size_t size) {
  if (nmemb != 0 && size > SIZE_MAX / nmemb)
    return NULL;

  size_t total = nmemb * size;
  void *p = nl_malloc(total);
  if (p) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < total; i++)
      b[i] = 0;
  }
  return p;
}

void nl_free(void *ptr) {
  if (!ptr)
    return;
  block_hdr_t *h = (block_hdr_t *)((unsigned char *)ptr - sizeof(block_hdr_t));
  if (h->magic != HDR_MAGIC)
    return; // bad pointer guard

  if (h->mmap_alloc) {
    // Free mmap'ed block
    size_t total = sizeof(block_hdr_t) + h->size;
    nl_munmap(h, total);
    return;
  }

  // Arena block: mark free and coalesce
  h->free = 1;

  // Coalesce forward
  unsigned char *next_p = (unsigned char *)ptr + h->size;
  unsigned char *end = _arena + ARENA_SIZE;
  if (next_p + sizeof(block_hdr_t) <= end) {
    block_hdr_t *next = (block_hdr_t *)next_p;
    if (next->magic == HDR_MAGIC && next->free) {
      h->size += sizeof(block_hdr_t) + next->size;
      next->magic = 0; // invalidate merged header
    }
  }

  // Coalesce backward
  unsigned char *p = _arena;
  unsigned char *target = (unsigned char *)h;
  block_hdr_t *prev = NULL;
  while (p < target) {
    block_hdr_t *cur = (block_hdr_t *)p;
    if (cur->magic != HDR_MAGIC)
      break; // heap corruption guard
    prev = cur;
    p += sizeof(block_hdr_t) + cur->size;
  }
  if (prev && prev->free && p == target) {
    prev->size += sizeof(block_hdr_t) + h->size;
    h->magic = 0; // h is now absorbed into prev
  }
}
