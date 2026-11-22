/*
 * tests/pty_harness.h - run a program in a pseudo-terminal and talk to it
 *
 * NOTE: These are host-side test programs, not part of nolibc build
 * POSIX only: posix_openpt() and others (no -lutil)
 *
 *   pty_proc p;
 *   pty_spawn(&p, "build/asciicam", argv, 40, 100); // NULL-terminated argv
 *   pty_read(&p, &buf, 20);                         // wait up to 20 ms
 *   pty_write(&p, "q", 1);
 *   int code = pty_finish(&p, 2.0);                 // exit code, -1 if killed
 */

#ifndef PTY_HARNESS_H
#define PTY_HARNESS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  pid_t pid;
  int fd; // Master side: what program prints and what it reads as keys
  int eof;
} pty_proc;

// Growing byte buffer for what a program printed
typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
} pty_buf;

static double pty_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void pty_buf_add(pty_buf *b, const void *p, size_t n) {
  if (b->len + n > b->cap) {
    size_t c = b->cap ? b->cap : 65536;
    while (c < b->len + n) {
      c *= 2;
    }

    b->data = (unsigned char *)realloc(b->data, c);
    if (!b->data) {
      abort();
    }

    b->cap = c;
  }

  memcpy(b->data + b->len, p, n);
  b->len += n;
}

// Number of times needle occurs in buffer (non-overlapping)
static size_t pty_buf_count(const pty_buf *b, const char *needle) {
  size_t n = strlen(needle), count = 0;
  for (size_t i = 0; n && i + n <= b->len;) {
    if (!memcmp(b->data + i, needle, n)) {
      count++;
      i += n;
    } else {
      i++;
    }
  }

  return count;
}

// Start argv[0] with a terminal of rows x cols
// Size is set in child before exec: setting it from parent races with startup
// and can trigger a SIGWINCH auto-fit
// Returns 0, or -1 if no terminal could be made
static int pty_spawn(pty_proc *p, const char *path, char *const argv[],
                     int rows, int cols) {
  memset(p, 0, sizeof *p);
  int m = posix_openpt(O_RDWR | O_NOCTTY);
  if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0) {
    return -1;
  }

  char *sname = ptsname(m);
  if (!sname) {
    return -1;
  }

  struct winsize ws;
  memset(&ws, 0, sizeof ws);
  ws.ws_row = (unsigned short)rows;
  ws.ws_col = (unsigned short)cols;

  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    close(m);
    setsid();
    int s = open(sname, O_RDWR); // Becomes controlling terminal
    if (s < 0) {
      _exit(127);
    }

    ioctl(s, TIOCSCTTY, 0);
    ioctl(s, TIOCSWINSZ, &ws);
    dup2(s, 0);
    dup2(s, 1);
    dup2(s, 2);
    if (s > 2) {
      close(s);
    }

    execv(path, argv);
    _exit(127);
  }

  ioctl(m, TIOCSWINSZ, &ws);
  p->pid = pid;
  p->fd = m;

  return 0;
}

// Wait up to wait_ms for output and append what arrives to b
// Returns bytes added, 0 when nothing came, -1 once program closed terminal
// (p->eof is set)
static long pty_read(pty_proc *p, pty_buf *b, int wait_ms) {
  if (p->eof) {
    return -1;
  }

  struct pollfd pf = {p->fd, POLLIN, 0};
  int r = poll(&pf, 1, wait_ms);
  if (r <= 0) {
    return 0;
  }

  unsigned char tmp[65536];
  ssize_t n = read(p->fd, tmp, sizeof tmp);
  if (n <= 0) { // EIO on Linux once slave side is gone
    p->eof = 1;

    return -1;
  }

  pty_buf_add(b, tmp, (size_t)n);

  return (long)n;
}

static void pty_write(pty_proc *p, const void *data, size_t n) {
  const unsigned char *d = (const unsigned char *)data;
  while (n > 0) {
    ssize_t w = write(p->fd, d, n);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }

      return;
    }

    d += w;
    n -= (size_t)w;
  }
}

static int pty_exited(pty_proc *p, int *code) {
  int st = 0;
  pid_t r = waitpid(p->pid, &st, WNOHANG);
  if (r == p->pid) {
    if (code) {
      *code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }

    return 1;
  }

  return r < 0 && errno == ECHILD ? 1 : 0;
}

// Wait up to `seconds` for program to exit, then kill it
// Returns its exit code, 128 + signal if a signal ended it, or -1 if it had to
// be killed
static int pty_finish(pty_proc *p, double seconds) {
  double t0 = pty_now();
  int st = 0;
  for (;;) {
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == p->pid) {
      close(p->fd);
      p->fd = -1;

      return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }

    if (r < 0 && errno != EINTR) {
      close(p->fd);
      p->fd = -1;

      return -1;
    }

    if (pty_now() - t0 > seconds) {
      kill(p->pid, SIGKILL);
      waitpid(p->pid, &st, 0);
      close(p->fd);
      p->fd = -1;

      return -1;
    }

    usleep(5000);
  }
}

#endif
