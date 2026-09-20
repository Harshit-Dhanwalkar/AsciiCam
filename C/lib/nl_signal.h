#ifndef NL_SIGNAL_H
#define NL_SIGNAL_H

#ifdef __LINUX_NOLIBC__
#include "nl_syscall.h"

#define SA_RESTORER 0x04000000
#define SA_RESTART 0x10000000
#define SIGINT 2
#define SIGTERM 15
#define SIGWINCH 28

struct nl_sigaction {
  void (*sa_handler)(int);
  unsigned long sa_flags;
  void (*sa_restorer)(void);
  unsigned long sa_mask[16];
};

static void __nl_restore(void) {
  __asm__ volatile("mov $15, %%rax\nsyscall" ::: "rax", "memory");
}

static inline int nl_signal(int sig, void (*handler)(int)) {
  struct nl_sigaction sa = {0};
  sa.sa_handler = handler;
  sa.sa_flags = SA_RESTORER | SA_RESTART; // restart interrupted syscalls
  sa.sa_restorer = __nl_restore;

  return (int)__sc4(SYS_rt_sigaction, sig, (long)&sa, 0, 8); // sigsetsize = 8
}

#define signal(sig, handler) nl_signal(sig, handler)

#else // macOS / Windows: use system libc's signal() instead of raw
      // rt_sigaction. The struct nl_sigaction / SA_RESTORER trick above is
      // x86-64 Linux ABI-specific
#include <signal.h>

static inline int nl_signal(int sig, void (*handler)(int)) {
  return (signal(sig, handler) == SIG_ERR) ? -1 : 0;
}

#endif

#endif
