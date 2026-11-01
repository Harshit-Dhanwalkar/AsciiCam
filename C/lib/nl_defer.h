#ifndef NL_DEFER_H
#define NL_DEFER_H

/*
 * Scope-bound cleanup (Rust's Drop, Go's defer) over compiler's
 * __attribute__((cleanup)): cleanup runs when variable goes out of scope on
 * every path - return, break, continue, fall through - so early returns cannot
 * leak
 *
 *   NL_AUTOFREE char *buf = nl_malloc(n);   // nl_free(buf) at end of scope
 *   NL_AUTOCLOSE int fd = open(path, ...);  // close(fd) if fd >= 0
 *   NL_DEFER(cleanup_fn, ptr);              // cleanup_fn(ptr) at end of scope
 *
 *   return NL_TAKE(buf);                    // hand ownership out: disarms
 *                                           // cleanup, returns value
 *
 * Several declarations in one scope are cleaned up in REVERSE order, like
 * destructors
 *
 * NOT run by: exit()/_exit()/abort(), longjmp over scope, a thread that is
 * killed, a crash. This is for ordinary control flow, not for emergencies
 * (terminal-restore signal path stays as it is)
 *
 * WARN: Needs GCC or Clang (cleanup attribute, statement expressions,
 * __COUNTER__)
 */

#include "nl_alloc.h"
#include "nl_io.h"

#define NL_CLEANUP(fn) __attribute__((cleanup(fn)))

static inline void nl_autofree_fn_(void *pp) {
  void **p = (void **)pp;
  nl_free(*p); // nl_free(NULL) is a no-op
  *p = (void *)0;
}

static inline void nl_autoclose_fn_(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

#define NL_AUTOFREE NL_CLEANUP(nl_autofree_fn_)
#define NL_AUTOCLOSE NL_CLEANUP(nl_autoclose_fn_)

typedef struct {
  void (*fn)(void *);
  void *arg;
} nl_defer_t_;

static inline void nl_defer_run_(nl_defer_t_ *d) {
  if (d->fn) {
    d->fn(d->arg);
  }
}

#define NL_CAT2_(a, b) a##b
#define NL_CAT_(a, b) NL_CAT2_(a, b)

// Run fn(arg) when enclosing scope ends. fn must take one void *
#define NL_DEFER(fn, arg)                                                      \
  nl_defer_t_ NL_CAT_(nl_defer_, __COUNTER__)                                  \
      NL_CLEANUP(nl_defer_run_) = {(void (*)(void *))(fn), (void *)(arg)}

// Return value of an owning variable and disarm its cleanup (set to NULL / -1):
// ownership moves to caller
#define NL_TAKE(var)                                                           \
  ({                                                                           \
    __typeof__(var) nl_take_tmp_ = (var);                                      \
    (var) = (__typeof__(var))_Generic((var), int: -1, default: 0);             \
    nl_take_tmp_;                                                              \
  })

#endif
