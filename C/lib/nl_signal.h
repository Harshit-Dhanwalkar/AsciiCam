#ifndef NL_SIGNAL_H
#define NL_SIGNAL_H

#include "nl_syscall.h"

#define SA_RESTORER 0x04000000
#define SIGINT 2
#define SIGTERM 15

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
  sa.sa_flags = SA_RESTORER;
  sa.sa_restorer = __nl_restore;
  return (int)__sc3(SYS_rt_sigaction, sig, (long)&sa, 0);
}

#define signal(sig, handler) nl_signal(sig, handler)

#endif
