#ifndef NL_THREAD_H
#define NL_THREAD_H

/*
 * nl_thread.h - futex + clone(2)-based threading primitives
 *
 * Provides nl_mutex_t / nl_cond_t / nl_thread_t and matching operations,
 * replacing pthreads so Linux build only needs -ldl (not -lpthread)
 */

#include "nl_syscall.h" /* SYS_*, PROT_READ/WRITE, MAP_FAILED, __sc* */
#include <stddef.h>
#include <stdint.h>

#ifdef __LINUX_NOLIBC__

/* mmap flags absent from nl_syscall.h (only MAP_SHARED/MAP_FAILED )
 */
#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

/* Futex op codes */
#define _NL_FUTEX_WAIT 0
#define _NL_FUTEX_WAKE 1

/*
 * _nl_futex_wait / _nl_futex_wake
 *
 * futex(uaddr, FUTEX_WAIT, val, timeout=NULL, uaddr2=NULL, val3=0)
 *   Sleeps if *uaddr == val.  Returns immediately with EAGAIN if value has
 *   already changed - this is no-lost-wakeup guarantee
 *
 * futex(uaddr, FUTEX_WAKE, n, ...)
 *   Wakes up to n threads sleeping on uaddr
 */
static inline void _nl_futex_wait(volatile int *addr, int val) {
  __sc6(SYS_futex, (long)addr, _NL_FUTEX_WAIT, (long)val, 0, 0, 0);
}
static inline void _nl_futex_wake(volatile int *addr, int n) {
  __sc6(SYS_futex, (long)addr, _NL_FUTEX_WAKE, (long)n, 0, 0, 0);
}

/* Mutex
 *
 * Two-state: 0 = unlocked, 1 = locked.
 *
 * Lock  : atomic-exchange to 1; if old value was already 1, sleep via
 *         futex_wait until another thread unlocks
 * Unlock: store 0 then wake one  waiter. Always waking adds one syscall on
 *         uncontended fast path but keeps code correct and minimal. At  20-30
 *         fps video frame rates overhead is negligible
 *
 * Zero-initialisation is valid - a zero-filled nl_mutex_t is unlocked
 */
typedef struct {
  volatile int state;
} nl_mutex_t;

#define NL_MUTEX_INITIALIZER {0}

static inline void nl_mutex_init(nl_mutex_t *m) { m->state = 0; }

static inline void nl_mutex_lock(nl_mutex_t *m) {
  while (__atomic_exchange_n(&m->state, 1, __ATOMIC_ACQUIRE))
    _nl_futex_wait(&m->state, 1);
}
static inline void nl_mutex_unlock(nl_mutex_t *m) {
  __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
  _nl_futex_wake(&m->state, 1);
}

/* Condition variable
 *
 * Sequence-counter design (standard Linux condvar trick):
 *
 *  - cond_wait     : snapshots seq, drops mutex, calls futex_wait(addr, snap).
 *                    If a signal already bumped seq between snapshot and
 *                    syscall, futex sees *addr != val and returns EAGAIN
 *                    immediately - no wakeup is ever lost regardless of
 *                    scheduling order
 *  - cond_signal   : bumps seq, wakes one sleeping thread
 *  - cond_broadcast: bumps seq, wakes all sleeping threads
 *
 * Usage contract (identical to POSIX):
 *   nl_mutex_lock(&m);
 *   while (!condition_is_true)
 *       nl_cond_wait(&c, &m);   // re-check after every wakeup
 *   // ... use shared state ...
 *   nl_mutex_unlock(&m);
 *
 * Zero-initialisation is valid - seq = 0 is a valid starting counter
 */
typedef struct {
  volatile int seq;
} nl_cond_t;

#define NL_COND_INITIALIZER {0}

static inline void nl_cond_init(nl_cond_t *c) { c->seq = 0; }

static inline void nl_cond_signal(nl_cond_t *c) {
  __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
  _nl_futex_wake(&c->seq, 1);
}
static inline void nl_cond_broadcast(nl_cond_t *c) {
  __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
  _nl_futex_wake(&c->seq, 0x7fffffff); // INT_MAX - wake all
}
static inline void nl_cond_wait(nl_cond_t *c, nl_mutex_t *m) {
  int s = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
  nl_mutex_unlock(m);
  _nl_futex_wait(&c->seq, s); // returns at once if seq already != s
  nl_mutex_lock(m);
}

/* Thread
 *
 * CLONE flags for a proper POSIX-style thread:
 *
 *   VM | FS | FILES | SIGHAND: share address space, fs root/cwd, open file
 *                              table, signal dispositions
 *  THREAD                    : same thread group (tgid seen by ps/kill)
 *  SYSVSEM                   : share SysV semaphore undo
 *  list PARENT_SETTID        : kernel writes child TID into *parent_tidptr
 *                              before clone() returns to parent
 *  CHILD_CLEARTID            : on thread exit: kernel zeroes *child_tidptr and
 *                              does futex_wake(child_tidptr, 1) - this  is how
 *                              nl_thread_join() wakes up
 */
#define _NL_CLONE_VM 0x00000100u
#define _NL_CLONE_FS 0x00000200u
#define _NL_CLONE_FILES 0x00000400u
#define _NL_CLONE_SIGHAND 0x00000800u
#define _NL_CLONE_THREAD 0x00010000u
#define _NL_CLONE_SYSVSEM 0x00040000u
#define _NL_CLONE_PARENT_SETTID 0x00100000u
#define _NL_CLONE_CHILD_CLEARTID 0x00200000u

#define _NL_THREAD_FLAGS                                                       \
  (_NL_CLONE_VM | _NL_CLONE_FS | _NL_CLONE_FILES | _NL_CLONE_SIGHAND |         \
   _NL_CLONE_THREAD | _NL_CLONE_SYSVSEM | _NL_CLONE_PARENT_SETTID |            \
   _NL_CLONE_CHILD_CLEARTID)

// Default per-thread stack size (2 MiB)
#define NL_THREAD_STACK_SIZE (2u * 1024u * 1024u)

typedef struct {
  void *(*fn)(void *); /* thread entry point                    */
  void *arg;           /* argument passed to fn                 */
  void *stack_base;    /* base address of mmap'd stack          */
  size_t stack_size;   /* size of mmap'd region; freed on join  */
  /*
   * tid - written by CLONE_PARENT_SETTID (parent side, before clone() returns)
   * and zeroed + futex-woken by CLONE_CHILD_CLEARTID when child calls SYS_exit
   * nl_thread_join() spins on it
   */
  volatile int tid;
} nl_thread_t;

/*
 * _nl_clone - always_inline wrapper for clone(2) syscall
 *
 * Must be always_inline. After clone(2) both parent and child continue
 * executing from very next instruction, each with their own RSP: parent keeps
 * its original stack frame; child starts with RSP = stack_top. If this were
 * emitted as a real (non-inline) function, child would execute that function's
 * epilogue ("ret") on its fresh, zero-filled stack and jump to address 0.
 * always_inline guarantees one inline "syscall" instruction with no surrounding
 * frame at any optimisation level
 *
 * clone(2) x86-64 register mapping:
 *   rdi = flags   rsi = new_stack   rdx = parent_tidptr
 *   r10 = child_tidptr              r8  = tls (0, unused)
 */
static __attribute__((always_inline)) long _nl_clone(unsigned long flags,
                                                     void *stack_top,
                                                     volatile int *parent_tid,
                                                     volatile int *child_tid) {
  long r;
  register long _r10 __asm__("r10") = (long)child_tid;
  register long _r8 __asm__("r8") = 0L; /* tls unused */
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "0"((long)SYS_clone), "D"((long)flags), "S"(stack_top),
                     "d"(parent_tid), "r"(_r10), "r"(_r8)
                   : "rcx", "r11", "memory");

  return r;
}

/*
 * nl_thread_create - spawn fn(arg) on a freshly mmap'd stack.
 *
 * clone(2) with CLONE_VM makes child start at same PC as parent sharing entire
 * address space
 * rax is 0 in child, child-TID in parent. Branch on that: child calls fn() then
 * SYS_exit(60) - Not exit_group(231), which would kill all threads
 * CHILD_CLEARTID then zeros t->tid (in nl_thread_t, NOT on mmap stack) and
 * calls futex_wake
 *
 * Returns 0 on success, -1 on error
 */
static inline int nl_thread_create(nl_thread_t *t, void *(*fn)(void *),
                                   void *arg) {
  t->fn = fn;
  t->arg = arg;
  t->tid = 0;
  t->stack_size = NL_THREAD_STACK_SIZE;

  /* mmap an anonymous stack. Passes top of region to clone (grows down) */
  long mret = __sc6(SYS_mmap, 0, (long)t->stack_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mret <= 0) {
    return -1;
  }

  t->stack_base = (void *)mret;
  void *stack_top = (char *)t->stack_base + t->stack_size;

  long ret = _nl_clone(_NL_THREAD_FLAGS, stack_top, &t->tid, &t->tid);

  if (ret < 0) {
    __sc2(SYS_munmap, (long)t->stack_base, (long)t->stack_size);
    t->stack_base = (void *)0;
    return -1;
  }

  if (ret == 0) {
    /*  child thread */
    t->fn(t->arg);
    /* Per-thread exit. CHILD_CLEARTID: zeros t->tid, wakes join() */
    __sc1(SYS_exit, 0);
    __builtin_unreachable();
  }

  /* parent: ret == child TID, already written into t->tid  */
  return 0;
}

/*
 * nl_thread_join - wait for thread to exit, then free its stack
 *
 * futex val-check prevents a missed wakeup when thread exits before
 * we reach futex_wait (kernel sees *addr != val and returns EAGAIN at once)
 */
static inline int nl_thread_join(nl_thread_t *t) {
  int tid;
  while ((tid = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE)) != 0) {
    _nl_futex_wait(&t->tid, tid);
  }

  if (t->stack_base) {
    __sc2(SYS_munmap, (long)t->stack_base, (long)t->stack_size);
    t->stack_base = (void *)0;
  }

  return 0;
}

#else /* non-Linux: thin wrapper around pthreads */

#include <pthread.h>

typedef pthread_mutex_t nl_mutex_t;
typedef pthread_cond_t nl_cond_t;

#define NL_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define NL_COND_INITIALIZER PTHREAD_COND_INITIALIZER

static inline void nl_mutex_init(nl_mutex_t *m) { pthread_mutex_init(m, NULL); }
static inline void nl_mutex_lock(nl_mutex_t *m) { pthread_mutex_lock(m); }
static inline void nl_mutex_unlock(nl_mutex_t *m) { pthread_mutex_unlock(m); }

static inline void nl_cond_init(nl_cond_t *c) { pthread_cond_init(c, NULL); }
static inline void nl_cond_signal(nl_cond_t *c) { pthread_cond_signal(c); }
static inline void nl_cond_broadcast(nl_cond_t *c) {
  pthread_cond_broadcast(c);
}
static inline void nl_cond_wait(nl_cond_t *c, nl_mutex_t *m) {
  pthread_cond_wait(c, m);
}

typedef struct {
  pthread_t handle;
  void *stack_base;  /* NULL - pthread manages its own stack */
  size_t stack_size; /* 0 */
  volatile int tid;  /* 0 - unused; pthread_join is used instead */
} nl_thread_t;

static inline int nl_thread_create(nl_thread_t *t, void *(*fn)(void *),
                                   void *arg) {
  t->stack_base = (void *)0;
  t->stack_size = 0;
  t->tid = 0;
  return pthread_create(&t->handle, NULL, fn, arg);
}
static inline int nl_thread_join(nl_thread_t *t) {
  return pthread_join(t->handle, NULL);
}

#endif /* __LINUX_NOLIBC__ */
#endif /* NL_THREAD_H */
