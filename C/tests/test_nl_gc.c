/*
 * tests/test_nl_gc.c - conservative collector (lib/nl_gc.c) and checked nl_free
 * it builds on
 *
 * Built at -O0 and -O2 by `make test`: a conservative collector has to work on
 * whatever optimiser does with pointers (registers, interior pointers)
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_nl_gc.c lib/nl_gc.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c -Iinclude -Ilib -o /tmp/gc
 */
#include "../lib/nolibc.h"
#include "nl_alloc_priv.h"
#include "nl_gc.h"
#include "nl_thread.h"

static int pass_n, fail_n;

#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

// Overwrite stack below caller so stale copies of pointers left by
// earlier calls cannot keep garbage alive
__attribute__((noinline)) static void clobber_stack(void) {
  volatile char junk[24 * 1024];
  for (size_t i = 0; i < sizeof(junk); i++) {
    junk[i] = 0;
  }
}

#if defined(__x86_64__)
#define SCRUB_REGS()                                                           \
  __asm__ volatile(                                                            \
      "xor %%eax,%%eax\n\txor %%ecx,%%ecx\n\txor %%edx,%%edx\n\t"              \
      "xor %%esi,%%esi\n\txor %%edi,%%edi\n\txor %%r8d,%%r8d\n\t"              \
      "xor %%r9d,%%r9d\n\txor %%r10d,%%r10d\n\txor %%r11d,%%r11d" ::           \
          : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",       \
            "memory")
#else
#define SCRUB_REGS() ((void)0)
#endif

__attribute__((noinline)) static void alloc_into(void **dst) {
  *dst = nl_gc_alloc(100);
  SCRUB_REGS();
}

__attribute__((noinline)) static size_t collect_clean(void) {
  clobber_stack();
  SCRUB_REGS();
  return nl_gc_collect();
}

static nl_gc_stats_t st(void) {
  nl_gc_stats_t s;
  nl_gc_stats(&s);

  return s;
}

typedef struct node {
  struct node *next;
  struct node *other;
  long value;
} node_t;

static node_t *g_head; // a root: lives in .bss
static node_t *g_mid;
static void *volatile g_p; // another one

// Allocation helpers kept out of line so no pointer survives in caller

// True if p is (still) a live block: freeing it is accepted without being
// flagged as a double or invalid free. Frees it.
static int was_live(void *p) {
  nl_gc_stats_t a = st();
  nl_free(p);
  nl_gc_stats_t b = st();

  return b.double_frees == a.double_frees && b.invalid_frees == a.invalid_frees;
}

__attribute__((noinline)) static void *alloc_unlinked(size_t n) {
  return nl_gc_alloc(n);
}

// Pointer to first byte AFTER block, and block's real size; base pointer is not
// kept anywhere
__attribute__((noinline)) static char *alloc_with_end(size_t n, size_t *size) {
  char *p = nl_gc_alloc(n);
  *size = ((block_hdr_t *)p - 1)->size;

  return p + *size;
}

__attribute__((noinline)) static void make_garbage(int count, size_t size) {
  for (int i = 0; i < count; i++) {
    volatile char *p = nl_gc_alloc(size);
    p[0] = 1;
  }
}

__attribute__((noinline)) static void make_list(int count) {
  g_head = NULL;
  for (int i = 0; i < count; i++) {
    node_t *n = nl_gc_alloc(sizeof(node_t));
    n->value = i;
    n->next = g_head;
    g_head = n;
  }
}

__attribute__((noinline)) static void make_cycle(void) {
  node_t *x = nl_gc_alloc(sizeof(node_t));
  node_t *y = nl_gc_alloc(sizeof(node_t));
  x->other = y;
  y->other = x;
  g_p = x;
}

__attribute__((noinline)) static long sum_list(void) {
  long sum = 0;
  for (node_t *n = g_head; n; n = n->next) {
    sum += n->value;
  }

  return sum;
}

__attribute__((noinline)) static void test_basic(void) {
  collect_clean();
  nl_gc_stats_t a = st();

  make_garbage(100, 200);
  nl_gc_stats_t b = st();
  CHECK(b.live_blocks >= a.live_blocks + 100, "garbage is live before collect");

  size_t freed = collect_clean();
  nl_gc_stats_t c = st();
  CHECK(freed >= 100 * 200, "unreachable blocks reclaimed (%zu bytes)", freed);
  CHECK(c.live_blocks <= a.live_blocks + 2,
        "live blocks back to baseline (%zu vs %zu)", c.live_blocks,
        a.live_blocks);

  // zero-filled, also when memory is recycled
  volatile char *dirty = nl_gc_alloc(4000);
  for (int i = 0; i < 4000; i++) {
    dirty[i] = (char)0x5A;
  }

  nl_free((void *)dirty);
  unsigned char *z = nl_gc_alloc(4000);
  int zero = 1;
  for (int i = 0; i < 4000; i++) {
    zero = zero && z[i] == 0;
  }

  CHECK(zero, "nl_gc_alloc returns zeroed memory");
  nl_free(z);
  CHECK(nl_gc_alloc(0) != NULL, "alloc(0) works");
}

__attribute__((noinline)) static void test_reachability(void) {
  // root in .bss, interior pointer, stack local
  g_p = nl_gc_alloc(1000);
  ((char *)g_p)[999] = 7;
  void *interior = (char *)g_p + 500;
  g_p = NULL;
  collect_clean();
  CHECK(((char *)interior)[499] == 7, "interior pointer keeps block alive");

  make_list(2000);
  collect_clean();
  CHECK(sum_list() == 1999L * 2000 / 2, "linked list survives collection");

  g_mid = g_head;
  for (int i = 0; i < 1000; i++) {
    g_mid = g_mid->next;
  }

  g_head = NULL; // cut off first 1000 nodes, keep rest via `mid`
  nl_gc_stats_t a = st();
  collect_clean();
  nl_gc_stats_t b = st();
  CHECK(b.live_blocks + 900 < a.live_blocks + 1,
        "dropped list head frees unreachable part (%zu -> %zu)", a.live_blocks,
        b.live_blocks);
  long s = 0;
  for (node_t *n = g_mid; n; n = n->next) {
    s++;
  }

  CHECK(s == 1000, "part still referenced is intact (%ld)", s);
  g_mid = NULL;
  collect_clean();

  // cycles are collected
  nl_gc_stats_t c0 = st();
  make_cycle();

  collect_clean();
  CHECK(st().live_blocks >= c0.live_blocks + 2, "cycle alive while referenced");
  g_p = NULL;
  collect_clean();
  CHECK(st().live_blocks <= c0.live_blocks,
        "unreachable cycle is collected (%zu vs %zu)", st().live_blocks,
        c0.live_blocks);
}

__attribute__((noinline)) static void *hide_in_manual(void) {
  void **holder = nl_malloc(sizeof(void *));
  *holder = nl_gc_alloc(300);
  ((char *)*holder)[0] = 42;

  return holder;
}

__attribute__((noinline)) static void *hide_in_atomic(void) {
  void **holder = nl_gc_alloc_atomic(sizeof(void *));
  *holder = nl_gc_alloc(300);

  return holder;
}

// __attribute__((noinline)) static void test_roots_and_kinds(void) {
//   // an nl_malloc block is a root for what it points at
//   void **holder = hide_in_manual();
//   collect_clean();
//   CHECK(((char *)*holder)[0] == 42, "pointer held by an nl_malloc block");
//   CHECK(was_live(*holder), "... and that block really was not freed");
//   nl_free(holder);
//   collect_clean();
//
//   // atomic blocks are not scanned: pointer inside does not keep alive
//   nl_gc_stats_t a = st();
//   g_p = hide_in_atomic();
//   collect_clean();
//   CHECK(st().live_blocks == a.live_blocks + 1,
//         "block only referenced from an atomic block is collected (%zu vs
//         %zu)", st().live_blocks, a.live_blocks);
//   g_p = NULL;
//   collect_clean();
//
//   // roots outside program image need registering
//   nl_gc_stats_t b = st();
//   void **page = nl_mmap(NULL, 4096, PROT_READ | PROT_WRITE,
//                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
//   alloc_into(&page[0]); // pointer lands only in unregistered mmap
//   collect_clean();
//   CHECK(st().live_blocks == b.live_blocks,
//         "pointer in unregistered memory does not keep a block alive");
//
//   page[0] = nl_gc_alloc(100);
//   ((char *)page[0])[3] = 9;
//   CHECK(nl_gc_root_add(page, 4096) == 0, "root_add");
//   collect_clean();
//   CHECK(((char *)page[0])[3] == 9, "registered root keeps its pointers
//   alive"); nl_gc_root_remove(page); nl_munmap(page, 4096); collect_clean();
//
//   // disable / enable
//   nl_gc_disable();
//   CHECK(nl_gc_collect() == 0, "collect does nothing while disabled");
//   nl_gc_enable();
// }
__attribute__((noinline)) static void test_roots_and_kinds(void) {
  // an nl_malloc block is a root for what it points at
  void **holder = hide_in_manual();
  collect_clean();
  CHECK(((char *)*holder)[0] == 42, "pointer held by an nl_malloc block");
  CHECK(was_live(*holder), "... and that block really was not freed");
  nl_free(holder);
  collect_clean();

  // registered root: pointer in an mmap page keeps its target alive
  nl_gc_stats_t b = st();
  void **page = nl_mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  page[0] = nl_gc_alloc(100);
  ((char *)page[0])[3] = 9;
  CHECK(nl_gc_root_add(page, 4096) == 0, "root_add");
  collect_clean();
  CHECK(((char *)page[0])[3] == 9, "registered root keeps its pointers alive");
  nl_gc_root_remove(page);
  nl_munmap(page, 4096);
  collect_clean();
  // (void)b;

  // disable / enable
  nl_gc_disable();
  CHECK(nl_gc_collect() == 0, "collect does nothing while disabled");
  nl_gc_enable();
}

__attribute__((noinline)) static void test_atomic_holder(void) {
  collect_clean();
  nl_gc_stats_t a = st();

  g_p = hide_in_atomic();
  collect_clean();
  CHECK(st().live_blocks == a.live_blocks + 1,
        "block only referenced from an atomic block is collected (%zu vs %zu)",
        st().live_blocks, a.live_blocks);

  g_p = NULL;
  collect_clean();
  CHECK(st().live_blocks == a.live_blocks,
        "atomic holder collected once g_p is cleared (%zu vs %zu)",
        st().live_blocks, a.live_blocks);
}

__attribute__((noinline)) static void test_unregistered_mmap(void) {
  collect_clean();
  nl_gc_stats_t b = st();

  void **page = nl_mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  alloc_into(&page[0]); // only copy is in unregistered mmap
  collect_clean();
  CHECK(st().live_blocks == b.live_blocks,
        "pointer in unregistered memory does not keep a block alive "
        "(%zu vs %zu)",
        st().live_blocks, b.live_blocks);

  nl_munmap(page, 4096);
  collect_clean();
}

// Pointers that live only on stack, only in a register, only as an end  pointer
// or only inside object
__attribute__((noinline)) __attribute__((noinline)) static void
test_stack_and_registers(void) {
  collect_clean();

  char *volatile on_stack = nl_gc_alloc(200);
  on_stack[7] = 3;
  collect_clean();
  CHECK(on_stack[7] == 3 && was_live((void *)on_stack),
        "block referenced only from stack survives");
  on_stack = NULL; // block was freed by was_live(): do not keep its address

  char *in_reg = nl_gc_alloc(200);
  in_reg[9] = 4;
  __asm__ volatile("" : "+r"(in_reg)); // value must be live in a register
  collect_clean();
  __asm__ volatile("" : "+r"(in_reg));
  CHECK(in_reg[9] == 4 && was_live(in_reg),
        "block referenced only from a register survives");
  in_reg = NULL;

  // end pointer: loops keep `end` and forget base
  size_t esz = 0;
  char *volatile end_only = alloc_with_end(112, &esz);
  collect_clean();
  CHECK(esz >= 112 && was_live((char *)end_only - esz),
        "one-past-the-end pointer keeps block");

  // a pointer to middle of block, nothing else
  char *volatile mid_only = (char *)alloc_unlinked(1000) + 600;
  collect_clean();
  CHECK(was_live((char *)mid_only - 600), "interior pointer keeps block");

  // not enough: an address in NEXT block's header is not this block
  char *a = alloc_unlinked(64);
  char *b = alloc_unlinked(64);
  CHECK(b != NULL, "b");
  char *volatile bp = b;
  a = NULL;
  collect_clean();
  CHECK(bp != NULL, "neighbour kept alive while first is collected");
  CHECK(was_live((void *)bp), "b live");
}

__attribute__((noinline)) static void test_out_of_memory(void) {
  collect_clean();
  nl_gc_stats_t a = st();

  // 40 MB through a 2 MB arena, nothing kept: only works if OOM collects
  int failed = 0;
  for (int i = 0; i < 400; i++) {
    volatile char *p = nl_gc_alloc(100 * 1024);
    if (!p) {
      failed++;
    } else {
      p[0] = 1;
    }
  }

  CHECK(failed == 0, "allocation never fails while garbage is reclaimable (%d)",
        failed);
  CHECK(st().collections > a.collections, "collections ran on demand");

  // a leaky nl_malloc user is rescued too
  collect_clean();
  make_garbage(18, 100 * 1024); // ~1.8 MB of arena
  void *big = nl_malloc(900 * 1024);
  CHECK(big != NULL, "nl_malloc triggers collection when arena is full");
  nl_free(big);

  // blocks above mmap threshold
  collect_clean();
  nl_gc_stats_t b = st();
  for (int i = 0; i < 6; i++) {
    volatile char *p = nl_gc_alloc(3 * 1024 * 1024);
    CHECK(p != NULL, "large GC block %d", i);
    if (p) {
      p[3 * 1024 * 1024 - 1] = 1;
    }
  }

  collect_clean();
  CHECK(st().live_blocks <= b.live_blocks + 1,
        "large garbage blocks are unmapped (%zu vs %zu)", st().live_blocks,
        b.live_blocks);
  g_p = nl_gc_alloc(2 * 1024 * 1024);
  ((char *)g_p)[2 * 1024 * 1024 - 1] = 5;
  collect_clean();
  CHECK(((char *)g_p)[2 * 1024 * 1024 - 1] == 5, "large block kept while used");
  g_p = NULL;
  collect_clean();

  // after sweeping everything arena has merged back into big free runs
  void *whole = nl_malloc(1000 * 1024);
  CHECK(whole != NULL, "arena coalesced after sweep");
  nl_free(whole);
}

__attribute__((noinline)) static void test_free_safety(void) {
  nl_gc_stats_t a = st();

  void *p = nl_malloc(100);
  nl_free(p);
  nl_free(p);
  CHECK(st().double_frees == a.double_frees + 1, "double free is detected");

  void *q = nl_malloc(100);
  nl_free((char *)q + 16);
  nl_free((char *)q + 1);
  int local = 0;
  nl_free(&local);
  nl_free((void *)0x1234);
  nl_free((void *)(size_t)-16);
  CHECK(st().invalid_frees == a.invalid_frees + 5,
        "interior / stack / wild pointers are ignored (%zu)",
        st().invalid_frees - a.invalid_frees);
  CHECK(st().live_blocks == a.live_blocks + 1, "and real block is intact");
  nl_free(q);
  nl_free(NULL);
  CHECK(st().invalid_frees == a.invalid_frees + 5,
        "free(NULL) is not an error");

  // freeing stale pointer after block was recycled must not free  new owner...
  // it IS new owner's block, so this documents limit: it frees it once, and a
  // second free is detected
  void *r1 = nl_malloc(64);
  nl_free(r1);
  void *r2 = nl_malloc(64);
  nl_free(r1); // same address if recycled: legal from allocator's view
  nl_free(r2);
  CHECK(st().double_frees >= a.double_frees + 1, "stale free accounted");

  // mmap blocks
  void *m = nl_malloc(3 * 1024 * 1024);
  nl_free(m);
  nl_free(m); // memory is gone: must not fault
  CHECK(st().invalid_frees >= a.invalid_frees + 6,
        "double free of an mmap block");

  // explicit free of a collector block, then a collection
  collect_clean();
  nl_gc_stats_t b = st();
  void *g = nl_gc_alloc(500);
  nl_free(g);
  collect_clean();
  nl_free(g);
  CHECK(st().live_blocks == b.live_blocks,
        "explicit free of GC block, no double release");
}

static void *foreign_thread(void *arg) {
  void **out = arg;
  *out = nl_gc_alloc(777);
  ((char *)*out)[0] = 11;

  return NULL;
}

__attribute__((noinline)) static void test_threads(void) {
  collect_clean();
  nl_gc_stats_t a = st();
  static void *volatile foreign;
  nl_thread_t t;
  CHECK(nl_thread_create(&t, foreign_thread, (void *)&foreign) == 0, "thread");
  nl_thread_join(&t);
  CHECK(foreign != NULL, "foreign thread gets memory");
  CHECK(st().foreign_allocs == a.foreign_allocs + 1, "counted as foreign");

  void *f = foreign;
  foreign = NULL; // nothing references it any more
  f = f ? f : NULL;
  collect_clean();
  nl_gc_stats_t b = st();
  nl_free(f);
  CHECK(st().invalid_frees == b.invalid_frees,
        "block from another thread was never collected");
}

// Random graph with mutation: everything reachable from table must stay intact,
// whatever collector does in between

typedef struct gnode {
  struct gnode *edge[3];
  unsigned long serial[3]; // serial target had when edge was made
  unsigned long my_serial;
} gnode_t;

#define TAB 48
static gnode_t *g_tab[TAB];
static unsigned long g_serial;
static unsigned long g_rng = 88172645463325252UL;

static unsigned long rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;

  return g_rng;
}

static int verify_from(gnode_t *n, int depth) {
  if (depth == 0) {
    return 1;
  }

  for (int i = 0; i < 3; i++) {
    if (n->edge[i]) {
      if (n->edge[i]->my_serial != n->serial[i]) {
        return 0;
      }

      if (!verify_from(n->edge[i], depth - 1)) {
        return 0;
      }
    }
  }

  return 1;
}

// Out of line: its spill slots must not linger in caller's frame
__attribute__((noinline)) static int verify_table(void) {
  int bad = 0;
  for (int i = 0; i < TAB; i++) {
    if (g_tab[i] && !verify_from(g_tab[i], 6)) {
      bad++;
    }
  }

  return bad;
}

__attribute__((noinline)) static void mutate(int steps) {
  for (int s = 0; s < steps; s++) {
    unsigned long r = rnd();
    int slot = (int)(r % TAB);
    switch ((r >> 8) % 4) {
    case 0:
    case 1: {
      gnode_t *n = nl_gc_alloc(sizeof(gnode_t) + (size_t)(rnd() % 400));
      if (!n) {
        break;
      }

      n->my_serial = ++g_serial;
      for (int i = 0; i < 3; i++) {
        gnode_t *t = g_tab[rnd() % TAB];
        n->edge[i] = t;
        n->serial[i] = t ? t->my_serial : 0;
      }

      g_tab[slot] = n;

      break;
    }
    case 2:
      g_tab[slot] = NULL;

      break;
    default: {
      // rewire an existing node (keeps serials truthful)
      gnode_t *n = g_tab[slot];
      gnode_t *t = g_tab[rnd() % TAB];
      if (n) {
        int e = (int)(rnd() % 3);
        n->edge[e] = t;
        n->serial[e] = t ? t->my_serial : 0;
      }

      break;
    }
    }
  }
}

__attribute__((noinline)) static void test_fuzz(void) {
  collect_clean();
  nl_gc_stats_t base = st();
  nl_gc_set_threshold(48 * 1024); // collections also happen by themselves
  int bad = 0;
  for (int round = 0; round < 400; round++) {
    mutate(200);
    if (round % 7 == 0) {
      collect_clean();
    }

    bad += verify_table();
  }

  nl_gc_set_threshold(0);
  CHECK(bad == 0, "reachable graph intact throughout (%d corruptions)", bad);
  CHECK(st().collections > base.collections + 50, "many collections ran");

  for (int i = 0; i < TAB; i++) {
    g_tab[i] = NULL;
  }

  collect_clean();
  CHECK(st().live_blocks <= base.live_blocks + 3,
        "everything reclaimed once table is cleared (%zu vs %zu)",
        st().live_blocks, base.live_blocks);
}

// Each test starts on a scrubbed stack: stale pointers left in unused slots of
// an earlier test's frames would otherwise be (harmlessly, but visibly) kept
// alive by conservative scan
#define RUN(t)                                                                 \
  do {                                                                         \
    clobber_stack();                                                           \
    t();                                                                       \
  } while (0)

int main(void) {
  RUN(test_basic);
  RUN(test_reachability);
  RUN(test_roots_and_kinds);
  RUN(test_atomic_holder);
  RUN(test_unregistered_mmap);
  RUN(test_stack_and_registers);
  RUN(test_out_of_memory);
  RUN(test_free_safety);
  RUN(test_threads);
  RUN(test_fuzz);

  nl_gc_stats_t s = st();
  fprintf(stderr, "gc: %zu collections, %zu blocks / %zu bytes reclaimed\n",
          s.collections, s.freed_blocks, s.freed_bytes);
  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
