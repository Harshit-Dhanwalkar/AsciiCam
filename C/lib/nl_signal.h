#ifndef NL_SIGNAL_H
#define NL_SIGNAL_H

#ifdef __LINUX_NOLIBC__
#include "nl_syscall.h"
#include "nl_types.h"

#define SA_RESTORER 0x04000000
#define SA_RESTART 0x10000000
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGABRT 6
#define SIGSEGV 11
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

#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif

static inline int nl_signal(int sig, void (*handler)(int)) {
  return (signal(sig, handler) == SIG_ERR) ? -1 : 0;
}

#if defined(_WIN32)
// Windows C runtime only knows SIGINT, SIGTERM, SIGABRT, SIGSEGV, SIGFPE,
// SIGILL and SIGBREAK; any other number is refused (and with UCRT can abort
// process), so do not pass SIGHUP / SIGQUIT / SIGWINCH to signal()
// A terminal reports through them arrives as console control events instead;
// this turns each one into a call of handler(sig):
//   Ctrl+C -> SIGINT, Ctrl+Break -> SIGTERM,
//   window closed / logoff / shutdown -> SIGHUP
// Handler runs on its own thread, so it must stick to a what signal handler may
// do
// Windows ends process once handler returns for "closed" events
#include <windows.h>

static void (*nl_ctrl_handler_)(int);

static inline BOOL WINAPI nl_ctrl_thunk_(DWORD ev) {
  if (!nl_ctrl_handler_) {
    return FALSE;
  }

  nl_ctrl_handler_(ev == CTRL_C_EVENT       ? SIGINT
                   : ev == CTRL_BREAK_EVENT ? SIGTERM
                                            : SIGHUP);

  return TRUE;
}

static inline int nl_console_ctrl_install(void (*handler)(int)) {
  nl_ctrl_handler_ = handler;

  return SetConsoleCtrlHandler(nl_ctrl_thunk_, TRUE) ? 0 : -1;
}
#endif

#endif

#endif
