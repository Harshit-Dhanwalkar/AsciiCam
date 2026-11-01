#include "nl_alloc.h"
#include "nl_alloc_priv.h"
#include "nl_checked.h"
#include "nl_io.h"
#include "nl_syscall.h"
#include "nl_types.h"

/*
 * ASan coverage note for this allocator
 *
 * nl_alloc manages 1 contiguous 2 MB static arena (arena_buf[]). ASan sees
 * whole array as a single allocation, it cannot automatically detect reads or
 * writes that cross an internal sub-block boundary (i.e. one nl_malloc'd region
 * reading into next)
 *
 * NOTE: What does work under `make sanitize`:
 *   - Stack OOB and UB regressions anywhere in codebase (UBSan)
 *   - Full redzone protection on mmap-backed large allocations (> 1 MB),
 *   - because those are individual mmap() calls that ASan tracks normally
 *   - UAF/OOB on any malloc()'d heap buffer used by grayscale_to_ascii
 *     (subpixel_g, subpixel_rgb, err[], dir_buf[], etc.) - they all go through
 *     nl_malloc, which for small sizes uses arena; ASan will catch writes past
 *     pointer end because those land in next block's header, which will have an
 *     unexpected magic value
 *
 * TODO: To get per-block redzone checking inside arena in future, poison
 * each free block's header region via ASAN_POISON_MEMORY_REGION on free and
 * unpoison on allocation:
 *
 *   #ifdef __SANITIZE_ADDRESS__
 *   #include <sanitizer/asan_interface.h>
 *   #define ARENA_POISON(p, n)   ASAN_POISON_MEMORY_REGION(p, n)
 *   #define ARENA_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION(p, n)
 *   #else
 *   #define ARENA_POISON(p, n)   ((void)0)
 *   #define ARENA_UNPOISON(p, n) ((void)0)
 *   #endif
 *
 * TODO: note: Implement above; current build is sufficient for catching
 * stack/UB/heap regressions in rendering path
 */

#define ARENA_SIZE NL_ARENA_SIZE
#define ALIGN 16
#define HDR_MAGIC 0xDEAD
#define MMAP_THRESHOLD (ARENA_SIZE / 2) // 1 MB

// Every block comes from either arena or an mmap, and nl_free validates a
// pointer against both before touching it: a pointer that is not start of a
// live block (never allocated, already freed, interior) is counted and ignored
// instead of corrupting heap or faulting
static nl_alloc_stats_t stats;

unsigned char nl_arena_buf[ARENA_SIZE] __attribute__((aligned(ALIGN)));
int nl_arena_init = 0;

// Every mmap-backed block is linked here, so it can be found by collector and
// so a free of an unknown pointer is never dereferenced
static mmap_node_t *mmap_head = NULL;

static void arena_boot(void) {
  block_hdr_t *h = (block_hdr_t *)nl_arena_buf;
  h->size = ARENA_SIZE - sizeof(block_hdr_t);
  h->free = 1;
  h->magic = HDR_MAGIC;
  h->mmap_alloc = 0;
  h->flags = 0;
  nl_arena_init = 1;
}

void nl_arena_ensure(void) {
  if (!nl_arena_init) {
    arena_boot();
  }
}

mmap_node_t *nl_mmap_list(void) { return mmap_head; }

static inline size_t align_up(size_t n) {
  return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

// Called when an allocation fails, before giving up: collector (nl_gc.c)
// installs itself here and returns number of bytes it reclaimed
size_t (*nl_alloc_oom_hook)(void) = NULL;

static void *alloc_once(size_t n, unsigned flags);

void *nl_alloc_flags(size_t n, unsigned flags) {
  void *p = alloc_once(n, flags);
  if (!p && nl_alloc_oom_hook) {
    static int in_hook = 0;
    if (!in_hook) {
      in_hook = 1;
      size_t freed = nl_alloc_oom_hook();
      in_hook = 0;
      if (freed > 0) {
        p = alloc_once(n, flags);
      }
    }
  }

  return p;
}

static void *alloc_once(size_t n, unsigned flags) {
  if (n > SIZE_MAX - ALIGN - sizeof(block_hdr_t) - sizeof(mmap_node_t)) {
    return NULL;
  }

  if (n == 0) {
    n = 1;
  }

  n = align_up(n);

  // Large allocation: use mmap
  if (n > MMAP_THRESHOLD) {
    size_t total = sizeof(mmap_node_t) + sizeof(block_hdr_t) + n;
    void *p = nl_mmap(NULL, total, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    if (p == MAP_FAILED) {
      return NULL;
    }

    mmap_node_t *node = (mmap_node_t *)p;
    node->prev = NULL;
    node->next = mmap_head;
    if (mmap_head) {
      mmap_head->prev = node;
    }

    mmap_head = node;

    block_hdr_t *h = (block_hdr_t *)((char *)p + sizeof(mmap_node_t));
    h->size = n;
    h->free = 0;
    h->magic = HDR_MAGIC;
    h->mmap_alloc = 1;
    h->flags = (unsigned char)flags;

    stats.live_blocks++;
    stats.live_bytes += n;

    return (char *)h + sizeof(block_hdr_t);
  }

  // Else use arena
  nl_arena_ensure();

  unsigned char *p = nl_arena_buf;
  const unsigned char *end = nl_arena_buf + ARENA_SIZE;

  while (p + sizeof(block_hdr_t) <= end) {
    block_hdr_t *h = (block_hdr_t *)p;
    if (h->magic != HDR_MAGIC) {
      break; // heap corruption
    }

    if (h->free && h->size >= n) {
      size_t leftover = h->size - n;
      if (leftover > sizeof(block_hdr_t) + ALIGN) {
        block_hdr_t *next = (block_hdr_t *)(p + sizeof(block_hdr_t) + n);
        next->size = leftover - sizeof(block_hdr_t);
        next->free = 1;
        next->magic = HDR_MAGIC;
        next->mmap_alloc = 0;
        next->flags = 0;
        h->size = n;
      }

      h->free = 0;
      h->flags = (unsigned char)flags;

      stats.live_blocks++;
      stats.live_bytes += h->size;

      return p + sizeof(block_hdr_t);
    }

    p += sizeof(block_hdr_t) + h->size;
  }

  return (void *)0; // OOM
}

void *nl_malloc(size_t n) { return nl_alloc_flags(n, 0); }

void *nl_calloc(size_t nmemb, size_t size) {
  size_t total;
  if (nl_size_mul(nmemb, size, &total) < 0) {
    return NULL;
  }

  void *p = nl_malloc(total);
  if (p) {
    uint8_t *b = (uint8_t *)p;

    for (size_t i = 0; i < total; i++) {
      b[i] = 0;
    }
  }

  return p;
}

void nl_alloc_get_stats(nl_alloc_stats_t *out) { *out = stats; }

// mmap node of block whose payload starts at ptr, or NULL
static mmap_node_t *mmap_find(const void *ptr) {
  for (mmap_node_t *n = mmap_head; n; n = n->next) {
    if ((const char *)n + sizeof(mmap_node_t) + sizeof(block_hdr_t) ==
        (const char *)ptr) {
      return n;
    }
  }

  return NULL;
}

void nl_alloc_release_mmap(mmap_node_t *node) {
  block_hdr_t *h = (block_hdr_t *)((char *)node + sizeof(mmap_node_t));
  size_t total = sizeof(mmap_node_t) + sizeof(block_hdr_t) + h->size;

  if (node->prev) {
    node->prev->next = node->next;
  } else {
    mmap_head = node->next;
  }

  if (node->next) {
    node->next->prev = node->prev;
  }

  stats.live_blocks--;
  stats.live_bytes -= h->size;
  h->magic = 0;
  nl_munmap(node, total);
}

// Checks alignment, header magic, a plausible size and that block
// that follows also starts with a valid header (or is arena end)
static block_hdr_t *arena_block_of(const void *ptr) {
  const unsigned char *c = (const unsigned char *)ptr;
  if (!nl_arena_init || c < nl_arena_buf + sizeof(block_hdr_t) ||
      c >= nl_arena_buf + ARENA_SIZE ||
      ((size_t)(c - nl_arena_buf) & (ALIGN - 1)) != 0) {
    return NULL;
  }

  block_hdr_t *h = (block_hdr_t *)(c - sizeof(block_hdr_t));
  if (h->magic != HDR_MAGIC || h->mmap_alloc || (h->size & (ALIGN - 1)) != 0 ||
      h->size > (size_t)(nl_arena_buf + ARENA_SIZE - c)) {
    return NULL;
  }

  const unsigned char *next = c + h->size;
  if (next < nl_arena_buf + ARENA_SIZE &&
      (next + sizeof(block_hdr_t) > nl_arena_buf + ARENA_SIZE ||
       ((const block_hdr_t *)next)->magic != HDR_MAGIC)) {
    return NULL;
  }

  return h;
}

// Free a validated arena block, merging with free neighbours
static void arena_release(block_hdr_t *h) {
  unsigned char *payload = (unsigned char *)h + sizeof(block_hdr_t);

  stats.live_blocks--;
  stats.live_bytes -= h->size;
  h->free = 1;
  h->flags = 0;

  // Coalesce forward
  unsigned char *next_p = payload + h->size;
  const unsigned char *end = nl_arena_buf + ARENA_SIZE;
  if (next_p + sizeof(block_hdr_t) <= end) {
    block_hdr_t *next = (block_hdr_t *)next_p;
    if (next->magic == HDR_MAGIC && next->free) {
      h->size += sizeof(block_hdr_t) + next->size;
      next->magic = 0; // invalidate merged header
    }
  }

  // Coalesce backward
  unsigned char *p = nl_arena_buf;
  const unsigned char *target = (unsigned char *)h;
  block_hdr_t *prev = NULL;
  while (p < target) {
    block_hdr_t *cur = (block_hdr_t *)p;
    if (cur->magic != HDR_MAGIC) {
      break; // heap corruption guard
    }

    prev = cur;
    p += sizeof(block_hdr_t) + cur->size;
  }

  if (prev && prev->free && p == target) {
    prev->size += sizeof(block_hdr_t) + h->size;
    h->magic = 0; // h is now absorbed into prev
  }
}

void nl_free(void *ptr) {
  if (!ptr) {
    return;
  }

  mmap_node_t *node = mmap_find(ptr);
  if (node) {
    nl_alloc_release_mmap(node);

    return;
  }

  block_hdr_t *h = arena_block_of(ptr);
  if (!h) {
    stats.invalid_frees++; // not ours, or an interior pointer
    return;
  }

  if (h->free) {
    stats.double_frees++;

    return;
  }

  arena_release(h);
}

// Free an arena block found by walking arena (collector sweep): no validation
// needed and caller merges neighbours in one pass
void nl_alloc_sweep_account(size_t size) {
  stats.live_blocks--;
  stats.live_bytes -= size;
}
