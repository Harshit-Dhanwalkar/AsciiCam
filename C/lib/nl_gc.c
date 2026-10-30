#include "nl_gc.h"
#include "nl_alloc_priv.h"
#include "nl_elf.h"
#include "nl_io.h"
#include "nl_syscall.h"

#ifndef SYS_gettid
#define SYS_gettid 186
#endif

#define HDR sizeof(block_hdr_t)
#define MAX_ROOTS 16
#define WORD sizeof(uintptr_t)

extern void *nl_stack_top;            // nl_start.c
extern const Elf64_Ehdr __ehdr_start; // linker: start of ELF image

typedef struct {
  uintptr_t lo, hi;
} range_t;

static struct {
  long owner; // thread id of owner, 0 = nobody yet
  int disabled;
  size_t threshold;
  size_t since; // bytes allocated since last collection
  range_t roots[MAX_ROOTS];
  nl_gc_stats_t st;
} gc;

// One collection
typedef struct {
  uint32_t *idx; // arena block header offsets, ascending
  size_t nidx;
  uintptr_t **stack; // blocks to scan
  size_t sp;
  uintptr_t alo, ahi; // arena bounds
} ctx_t;

static long self_tid(void) { return __sc1(SYS_gettid, 0); }

// Block that contains address w, or NULL
// Interior pointers and address one past end both count (loops keep "end"
// pointers around)
static block_hdr_t *classify(const ctx_t *c, uintptr_t w) {
  if (w >= c->alo && w < c->ahi) {
    size_t off = (size_t)(w - c->alo);
    size_t lo = 0, hi = c->nidx;
    // last block whose payload starts at or before w
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if ((size_t)c->idx[mid] + HDR <= off) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }

    if (lo == 0) {
      return NULL;
    }

    block_hdr_t *h = (block_hdr_t *)(c->alo + c->idx[lo - 1]);
    if (h->free || off > (size_t)c->idx[lo - 1] + HDR + h->size) {
      return NULL;
    }

    return h;
  }

  for (mmap_node_t *n = nl_mmap_list(); n; n = n->next) {
    block_hdr_t *h = (block_hdr_t *)((char *)n + sizeof(mmap_node_t));
    uintptr_t start = (uintptr_t)h + HDR;
    if (w >= start && w <= start + h->size) {
      return h;
    }
  }

  return NULL;
}

static void scan(ctx_t *c, uintptr_t lo, uintptr_t hi) {
  lo = (lo + WORD - 1) & ~(uintptr_t)(WORD - 1);
  hi &= ~(uintptr_t)(WORD - 1);
  for (const uintptr_t *p = (const uintptr_t *)lo; (uintptr_t)p < hi; p++) {
    uintptr_t w = *p;
    // cheap reject: most words are small integers or far from heap
    if (w < 4096) {
      continue;
    }

    block_hdr_t *h = classify(c, w);
    if (h && (h->flags & NL_BF_GC) && !(h->flags & NL_BF_MARK)) {
      h->flags |= NL_BF_MARK;
      gc.st.last_marked++;
      if (!(h->flags & NL_BF_ATOMIC)) {
        c->stack[c->sp++] = (uintptr_t *)h;
      }
    }
  }
}

static void drain(ctx_t *c) {
  while (c->sp > 0) {
    block_hdr_t *h = (block_hdr_t *)c->stack[--c->sp];
    uintptr_t payload = (uintptr_t)h + HDR;
    scan(c, payload, payload + h->size);
  }
}

static void scan_root(ctx_t *c, uintptr_t lo, uintptr_t hi) {
  scan(c, lo, hi);
  drain(c);
}

// Writable segments of program image except arena (which is heap and is walked
// block by block)
static void scan_data(ctx_t *c) {
  const Elf64_Phdr *ph =
      (const Elf64_Phdr *)((const char *)&__ehdr_start + __ehdr_start.e_phoff);
  uintptr_t bias = 0;
  for (int i = 0; i < __ehdr_start.e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD && ph[i].p_offset == 0) {
      bias = (uintptr_t)&__ehdr_start - (uintptr_t)ph[i].p_vaddr;

      break;
    }
  }

  for (int i = 0; i < __ehdr_start.e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_W)) {
      continue;
    }

    uintptr_t lo = (uintptr_t)ph[i].p_vaddr + bias;
    uintptr_t hi = lo + (uintptr_t)ph[i].p_memsz;
    if (c->alo < hi && c->ahi > lo) { // contains arena: scan around it
      if (lo < c->alo) {
        scan_root(c, lo, c->alo);
      }

      if (hi > c->ahi) {
        scan_root(c, c->ahi, hi);
      }
    } else {
      scan_root(c, lo, hi);
    }
  }
}

static size_t page_up(size_t n) { return (n + 4095) & ~(size_t)4095; }

static size_t collect_impl(uintptr_t sp) {
  nl_arena_ensure();

  ctx_t c;
  c.alo = (uintptr_t)nl_arena_buf;
  c.ahi = c.alo + NL_ARENA_SIZE;

  // Index arena
  size_t na = 0;
  for (size_t off = 0; off + HDR <= NL_ARENA_SIZE;) {
    const block_hdr_t *h = (const block_hdr_t *)(c.alo + off);
    if (h->magic != 0xDEAD || (h->size & 15) != 0) {
      return 0; // corrupt heap: do not guess
    }

    na++;
    off += HDR + h->size;
  }

  size_t nm = 0;
  for (mmap_node_t *n = nl_mmap_list(); n; n = n->next) {
    nm++;
  }

  size_t bytes = page_up(na * sizeof(uint32_t)) +
                 page_up((na + nm + 1) * sizeof(uintptr_t *));
  void *mem = nl_mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  if (mem == MAP_FAILED) {
    return 0;
  }

  c.idx = (uint32_t *)mem;
  c.stack = (uintptr_t **)((char *)mem + page_up(na * sizeof(uint32_t)));
  c.nidx = 0;
  c.sp = 0;
  for (size_t off = 0; off + HDR <= NL_ARENA_SIZE;) {
    c.idx[c.nidx++] = (uint32_t)off;
    off += HDR + ((const block_hdr_t *)(c.alo + off))->size;
  }

  gc.st.last_marked = 0;

  // Roots
  scan_root(&c, sp, (uintptr_t)nl_stack_top);
  scan_data(&c);
  for (int i = 0; i < MAX_ROOTS; i++) {
    if (gc.roots[i].lo) {
      scan_root(&c, gc.roots[i].lo, gc.roots[i].hi);
    }
  }

  // Live nl_malloc blocks are roots too
  for (size_t i = 0; i < c.nidx; i++) {
    const block_hdr_t *h = (const block_hdr_t *)(c.alo + c.idx[i]);
    if (!h->free && !(h->flags & NL_BF_GC)) {
      scan_root(&c, (uintptr_t)h + HDR, (uintptr_t)h + HDR + h->size);
    }
  }

  for (mmap_node_t *n = nl_mmap_list(); n; n = n->next) {
    const block_hdr_t *h =
        (const block_hdr_t *)((char *)n + sizeof(mmap_node_t));
    if (!(h->flags & NL_BF_GC)) {
      scan_root(&c, (uintptr_t)h + HDR, (uintptr_t)h + HDR + h->size);
    }
  }

  // Sweep arena in one pass, merging runs of free blocks as we go
  size_t freed = 0;
  size_t nfreed = 0;
  block_hdr_t *prev_free = NULL;
  for (size_t off = 0; off + HDR <= NL_ARENA_SIZE;) {
    block_hdr_t *h = (block_hdr_t *)(c.alo + off);
    size_t next = off + HDR + h->size;

    if (!h->free && (h->flags & NL_BF_GC)) {
      if (h->flags & NL_BF_MARK) {
        h->flags &= (unsigned char)~NL_BF_MARK;
      } else {
        freed += h->size;
        nfreed++;
        nl_alloc_sweep_account(h->size);
        h->free = 1;
        h->flags = 0;
      }
    }

    if (h->free) {
      if (prev_free) {
        prev_free->size += HDR + h->size;
        h->magic = 0;
      } else {
        prev_free = h;
      }
    } else {
      prev_free = NULL;
    }

    off = next;
  }

  // Sweep mmap-backed blocks
  for (mmap_node_t *n = nl_mmap_list(); n;) {
    mmap_node_t *nx = n->next;
    block_hdr_t *h = (block_hdr_t *)((char *)n + sizeof(mmap_node_t));
    if (h->flags & NL_BF_GC) {
      if (h->flags & NL_BF_MARK) {
        h->flags &= (unsigned char)~NL_BF_MARK;
      } else {
        freed += h->size;
        nfreed++;
        nl_alloc_release_mmap(n);
      }
    }

    n = nx;
  }

  nl_munmap(mem, bytes);

  gc.st.collections++;
  gc.st.freed_blocks += nfreed;
  gc.st.freed_bytes += freed;
  gc.since = 0;

  return freed;
}

static int owns(void) {
  long t = self_tid();
  if (gc.owner == 0) {
    gc.owner = t;
  }

  return gc.owner == t;
}

__attribute__((noinline)) size_t nl_gc_collect(void) {
  if (gc.disabled || !nl_stack_top || !owns()) {
    return 0;
  }

  // Callee-saved registers may hold only reference to a block: spill them where
  // stack scan will see them
  // Caller-saved ones compiler already saved on stack around this call
  uintptr_t regs[6];
#if defined(__x86_64__)
  __asm__ volatile("movq %%rbx, 0(%0)\n\t"
                   "movq %%rbp, 8(%0)\n\t"
                   "movq %%r12, 16(%0)\n\t"
                   "movq %%r13, 24(%0)\n\t"
                   "movq %%r14, 32(%0)\n\t"
                   "movq %%r15, 40(%0)\n\t"
                   :
                   : "r"(regs)
                   : "memory");
#else
  __builtin_unwind_init();
#endif
  uintptr_t sp;
  __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
  if ((uintptr_t)regs < sp) {
    sp = (uintptr_t)regs;
  }

  size_t r = collect_impl(sp);

  __asm__ volatile("" : : "r"(regs) : "memory"); // keep regs alive to here

  return r;
}

static size_t oom_hook(void) { return nl_gc_collect(); }

static void *gc_alloc(size_t n, unsigned flags) {
  if (!nl_alloc_oom_hook) {
    nl_alloc_oom_hook = oom_hook;
  }

  if (!owns()) {
    // Another thread: stack not visible, so block must not be collectable
    gc.st.foreign_allocs++;
    void *q = nl_alloc_flags(n, 0);
    if (q) {
      uint8_t *b = (uint8_t *)q;
      size_t sz = ((block_hdr_t *)((char *)q - HDR))->size;
      for (size_t i = 0; i < sz; i++) {
        b[i] = 0;
      }
    }

    return q;
  }

  if (gc.threshold && gc.since >= gc.threshold) {
    nl_gc_collect();
  }

  void *p = nl_alloc_flags(n, NL_BF_GC | flags); // OOM runs a collection
  if (p) {
    block_hdr_t *h = (block_hdr_t *)((char *)p - HDR);
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < h->size; i++) {
      b[i] = 0;
    }

    gc.since += h->size;
  }

  return p;
}

void *nl_gc_alloc(size_t n) { return gc_alloc(n, 0); }

void *nl_gc_alloc_atomic(size_t n) { return gc_alloc(n, NL_BF_ATOMIC); }

void nl_gc_set_threshold(size_t bytes) { gc.threshold = bytes; }

void nl_gc_disable(void) { gc.disabled++; }

void nl_gc_enable(void) {
  if (gc.disabled > 0) {
    gc.disabled--;
  }
}

int nl_gc_root_add(const void *p, size_t len) {
  for (int i = 0; i < MAX_ROOTS; i++) {
    if (!gc.roots[i].lo) {
      gc.roots[i].lo = (uintptr_t)p;
      gc.roots[i].hi = (uintptr_t)p + len;

      return 0;
    }
  }

  return -1;
}

void nl_gc_root_remove(const void *p) {
  for (int i = 0; i < MAX_ROOTS; i++) {
    if (gc.roots[i].lo == (uintptr_t)p) {
      gc.roots[i].lo = gc.roots[i].hi = 0;
    }
  }
}

void nl_gc_stats(nl_gc_stats_t *out) {
  nl_alloc_stats_t a;
  nl_alloc_get_stats(&a);
  *out = gc.st;
  out->live_blocks = a.live_blocks;
  out->live_bytes = a.live_bytes;
  out->invalid_frees = a.invalid_frees;
  out->double_frees = a.double_frees;
}
