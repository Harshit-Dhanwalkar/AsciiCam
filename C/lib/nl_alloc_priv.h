#ifndef NL_ALLOC_PRIV_H
#define NL_ALLOC_PRIV_H

/*
 * Allocator internals shared by nl_alloc.c and collector (nl_gc.c)
 * Not part of public API: use nl_alloc.h / nl_gc.h
 */

#include "nl_types.h"

#define NL_ARENA_SIZE ((size_t)(2 * 1024 * 1024))

// Per-block flags
#define NL_BF_GC 1     // owned by collector: freed when unreachable
#define NL_BF_ATOMIC 2 // GC block that holds no pointers: never scanned
#define NL_BF_MARK 4   // reached during current collection

typedef struct block_hdr {
  size_t size;              // payload bytes
  int free;                 // 1 = free
  unsigned short magic;     // HDR_MAGIC while header is real
  unsigned char mmap_alloc; // 1 if mmap'ed
  unsigned char flags;      // NL_BF_*
} block_hdr_t;

// Each mmap-backed block starts with one of these, then its block_hdr_t
typedef struct mmap_node {
  struct mmap_node *next;
  struct mmap_node *prev;
} mmap_node_t;

typedef struct {
  size_t live_blocks;   // blocks currently allocated
  size_t live_bytes;    // their payload bytes
  size_t invalid_frees; // nl_free of a pointer that is not a live block start
  size_t double_frees;  // nl_free of a block that was already free
} nl_alloc_stats_t;

extern unsigned char nl_arena_buf[];
extern int nl_arena_init;

extern size_t (*nl_alloc_oom_hook)(void);

void nl_arena_ensure(void);
mmap_node_t *nl_mmap_list(void);
void *nl_alloc_flags(size_t n, unsigned flags);
void nl_alloc_release_mmap(mmap_node_t *node);
void nl_alloc_sweep_account(size_t size);
void nl_alloc_get_stats(nl_alloc_stats_t *out);

#endif
