#ifndef NL_IO_H
#define NL_IO_H

/* nl_io.h
   I/O syscall wrappers: write, read, open, close, mmap, munmap, ioctl, select,
   inotify
 */

#include <termios.h>
#include <time.h>

#ifdef __LINUX_NOLIBC__
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif
#include "nl_syscall.h"
// Basic I/O
static inline ssize_t nl_write(int fd, const void *buf, size_t n) {
  return (ssize_t)__sc3(SYS_write, fd, (long)buf, (long)n);
}
static inline ssize_t nl_read(int fd, void *buf, size_t n) {
  return (ssize_t)__sc3(SYS_read, fd, (long)buf, (long)n);
}
static inline int nl_open(const char *path, int flags, int mode) {
  return (int)__sc3(SYS_open, (long)path, flags, mode);
}
static inline int nl_close(int fd) { return (int)__sc1(SYS_close, fd); }
static inline int nl_unlink(const char *path) {
  return (int)__sc1(SYS_unlink, (long)path);
}
static inline int nl_ioctl(int fd, unsigned long req, void *arg) {
  return (int)__sc3(SYS_ioctl, fd, (long)req, (long)arg);
}

// fprintf / stderr
#define stderr 2
#define fprintf(fd, fmt, ...)                                                  \
  do {                                                                         \
    char _fb[1024];                                                            \
    int _fn = nl_snprintf(_fb, sizeof(_fb), fmt, ##__VA_ARGS__);               \
    if (_fn > 0) {                                                             \
      size_t _nw =                                                             \
          (_fn < (int)sizeof(_fb) - 1) ? (size_t)_fn : sizeof(_fb) - 1;        \
      nl_write((int)(long)(fd), _fb, _nw);                                     \
    }                                                                          \
  } while (0)

// mmap / munmap
static inline void *nl_mmap(void *addr, size_t len, int prot, int flags, int fd,
                            long off) {
  return (void *)__sc6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
}
static inline int nl_munmap(void *addr, size_t len) {
  return (int)__sc2(SYS_munmap, (long)addr, (long)len);
}

// select
struct nl_timeval {
  long tv_sec;
  long tv_usec;
};
typedef struct {
  unsigned long fds_bits[16];
} nl_fd_set;

#define NL_FD_ZERO(s) __builtin_memset((s), 0, sizeof(*(s)))
#define NL_FD_SET(fd, s) ((s)->fds_bits[(fd) / 64] |= (1UL << ((fd) % 64)))
#define NL_FD_ISSET(fd, s) ((s)->fds_bits[(fd) / 64] & (1UL << ((fd) % 64)))

static inline int nl_select(int nfds, nl_fd_set *r, nl_fd_set *w, nl_fd_set *e,
                            struct nl_timeval *tv) {
  return (int)__sc6(SYS_select, nfds, (long)r, (long)w, (long)e, (long)tv, 0);
}

// clock / sleep
static inline int nl_clock_gettime(clockid_t id, struct timespec *ts) {
  return (int)__sc2(SYS_clock_gettime, id, (long)ts);
}
static inline int nl_nanosleep(const struct timespec *req,
                               struct timespec *rem) {
  long r = __sc2(SYS_nanosleep, (long)req, (long)rem);
  if (r < 0) {
    errno = (int)-r;
    return -1;
  }
  return 0;
}
static inline void nl_usleep(unsigned long us) {
  struct timespec ts = {(long)(us / 1000000), (long)((us % 1000000) * 1000)};
  nl_nanosleep(&ts, (struct timespec *)0);
}
static inline long nl_time(void) { return (long)__sc1(SYS_time, 0); }
static inline void nl_exit(int code) {
  __sc1(SYS_exit_group, code);
  __builtin_unreachable();
}

// inotify
static inline int nl_inotify_init1(int flags) {
  return (int)__sc1(SYS_inotify_init1, flags);
}
static inline int nl_inotify_add_watch(int fd, const char *path,
                                       uint32_t mask) {
  return (int)__sc3(SYS_inotify_add_watch, fd, (long)path, mask);
}

// termios via ioctl
#include <termios.h>
#define NL_TCGETS 0x5401
#define NL_TCSETS 0x5402
#define NL_TCSETSF 0x5404

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

static inline int nl_tcgetattr(int fd, struct termios *t) {
  return nl_ioctl(fd, NL_TCGETS, t);
}
static inline int nl_tcsetattr(int fd, int action, const struct termios *t) {
  unsigned long req = (action == 2) ? NL_TCSETSF : NL_TCSETS;
  return nl_ioctl(fd, req, (void *)t);
}

// Macro redirects
#define write(fd, buf, n) nl_write(fd, buf, n)
#define read(fd, buf, n) nl_read(fd, buf, n)
#define _open2(p, f) nl_open(p, f, 0)
#define _open3(p, f, m) nl_open(p, f, m)
#define _open_pick(_1, _2, _3, N, ...) N
#define open(...) _open_pick(__VA_ARGS__, _open3, _open2)(__VA_ARGS__)
#define close(fd) nl_close(fd)
#define unlink(p) nl_unlink(p)
#define mmap(a, l, p, f, fd, o) nl_mmap(a, l, p, f, fd, o)
#define munmap(a, l) nl_munmap(a, l)
#define ioctl(fd, req, arg) nl_ioctl(fd, req, (void *)(long)(arg))
#define clock_gettime(id, ts) nl_clock_gettime(id, ts)
#define nanosleep(req, rem) nl_nanosleep(req, rem)
#define usleep(us) nl_usleep(us)
#define time(p) nl_time()
#define tcgetattr(fd, t) nl_tcgetattr(fd, t)
#define tcsetattr(fd, act, t) nl_tcsetattr(fd, act, t)
#define inotify_init1(f) nl_inotify_init1(f)
#define inotify_add_watch(f, p, m) nl_inotify_add_watch(f, p, m)

#else
// #include <sys/mmap.h>
#include <sys/select.h>
#include <unistd.h>
#endif

#endif
