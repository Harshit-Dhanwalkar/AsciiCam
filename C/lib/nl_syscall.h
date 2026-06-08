#ifndef NL_SYSCALL_H
#define NL_SYSCALL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;
typedef int sig_atomic_t;

// errno
extern int errno;
#define EINTR 4
#define EAGAIN 11

// File descriptors
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// File open flags
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_NONBLOCK 04000
#define O_CLOEXEC 02000000

// mmap constants
#define MAP_FAILED ((void *)-1)
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1

// Syscall numbers (x86-64 Linux)
#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_rt_sigaction 13
#define SYS_ioctl 16
#define SYS_select 23
#define SYS_nanosleep 35
#define SYS_unlink 87
#define SYS_clock_gettime 228
#define SYS_exit_group 231
#define SYS_time 201
#define SYS_inotify_init1 294
#define SYS_inotify_add_watch 254

// Raw syscall asm wrappers
static inline long __sc1(long n, long a1) {
  long r;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "0"(n), "D"(a1)
                   : "rcx", "r11", "memory");
  return r;
}
static inline long __sc2(long n, long a1, long a2) {
  long r;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "0"(n), "D"(a1), "S"(a2)
                   : "rcx", "r11", "memory");
  return r;
}
static inline long __sc3(long n, long a1, long a2, long a3) {
  long r;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "0"(n), "D"(a1), "S"(a2), "d"(a3)
                   : "rcx", "r11", "memory");
  return r;
}
static inline long __sc6(long n, long a1, long a2, long a3, long a4, long a5,
                         long a6) {
  long r;
  register long r10 __asm__("r10") = a4;
  register long r8 __asm__("r8") = a5;
  register long r9 __asm__("r9") = a6;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8),
                     "r"(r9)
                   : "rcx", "r11", "memory");
  return r;
}

#endif
