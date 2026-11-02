#ifndef NB_H
#define NB_H

/*
 * nb.h - tiny build-in-C helper
 * Build script is a C program that includes this header; it rebuilds itself
 * when its source changes
 * Needs only a C compiler and a POSIX system (Linux / macOS); no make, no cmake
 */

// TODO: Use lib/
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
  const char **v;
  size_t n, cap;
} nb_cmd;

#define nb_log(...)                                                            \
  do {                                                                         \
    fprintf(stderr, "[nb] " __VA_ARGS__);                                      \
    fputc('\n', stderr);                                                       \
  } while (0)

static void nb_push(nb_cmd *c, const char *s) {
  if (c->n + 1 >= c->cap) {
    c->cap = c->cap ? c->cap * 2 : 32;
    c->v = (const char **)realloc((void *)c->v, c->cap * sizeof(*c->v));
    if (!c->v) {
      abort();
    }
  }

  c->v[c->n++] = s;
  c->v[c->n] = NULL;
}

#define nb_cmd_add(c, ...)                                                     \
  do {                                                                         \
    const char *nb_a_[] = {__VA_ARGS__};                                       \
    for (size_t nb_i_ = 0; nb_i_ < sizeof(nb_a_) / sizeof(*nb_a_); nb_i_++) {  \
      nb_push((c), nb_a_[nb_i_]);                                              \
    }                                                                          \
  } while (0)

// Add each space-separated word of `words` as its own argument (strings in
// include/plugin_build.h are written that way)
static void nb_add_words(nb_cmd *c, const char *words) {
  const char *p = words;
  while (*p) {
    while (*p == ' ') {
      p++;
    }

    const char *q = p;
    while (*q && *q != ' ') {
      q++;
    }

    if (q > p) {
      char *w = (char *)malloc((size_t)(q - p) + 1);
      if (!w) {
        abort();
      }

      memcpy(w, p, (size_t)(q - p));
      w[q - p] = 0;
      nb_push(c, w);
    }

    p = q;
  }
}

// printf into memory that lives until exit
static char *nb_fmt(const char *fmt, ...) {
  va_list a, b;
  va_start(a, fmt);
  va_copy(b, a);
  int n = vsnprintf(NULL, 0, fmt, a);
  va_end(a);
  char *s = (char *)malloc((size_t)n + 1);
  vsnprintf(s, (size_t)n + 1, fmt, b);
  va_end(b);

  return s;
}

static void nb_show(const nb_cmd *c) {
  fputs("[nb] $", stderr);
  for (size_t i = 0; i < c->n; i++) {
    fprintf(stderr, " %s", c->v[i]);
  }

  fputc('\n', stderr);
}

// Start a command; returns pid or -1
static pid_t nb_start(const nb_cmd *c, const char *stdout_path) {
  nb_show(c);
  pid_t p = fork();
  if (p == 0) {
    if (stdout_path) {
      int fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) {
        dup2(fd, 1);
      }
    }

    execvp(c->v[0], (char *const *)c->v);
    fprintf(stderr, "[nb] cannot run %s: %s\n", c->v[0], strerror(errno));
    _exit(127);
  }

  return p;
}

static int nb_wait(pid_t p) {
  int st = 0;
  if (p < 0 || waitpid(p, &st, 0) < 0) {
    return 0;
  }

  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int nb_run(const nb_cmd *c) { return nb_wait(nb_start(c, NULL)); }

// Run commands in parallel, up to jobs at a time (jobs <= 64)
typedef struct {
  pid_t pid[64];
  int jobs, running, ok;
} nb_procs;

static void nb_procs_init(nb_procs *p, int jobs) {
  p->jobs = jobs < 1 ? 1 : jobs > 64 ? 64 : jobs;
  p->running = 0;
  p->ok = 1;
}

static void nb_procs_reap_one(nb_procs *p) {
  int st = 0;
  pid_t d = wait(&st);
  for (int i = 0; i < p->running; i++) {
    if (p->pid[i] == d) {
      p->pid[i] = p->pid[--p->running];

      break;
    }
  }

  if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
    p->ok = 0;
  }
}

static void nb_procs_add(nb_procs *p, const nb_cmd *c) {
  while (p->running >= p->jobs) {
    nb_procs_reap_one(p);
  }

  pid_t pid = nb_start(c, NULL);
  if (pid < 0) {
    p->ok = 0;

    return;
  }

  p->pid[p->running++] = pid;
}

static int nb_procs_wait(nb_procs *p) {
  while (p->running > 0) {
    nb_procs_reap_one(p);
  }

  return p->ok;
}

static int nb_mkdir(const char *path) {
  char *t = nb_fmt("%s", path);
  for (char *s = t + 1; *s; s++) {
    if (*s == '/') {
      *s = 0;
      mkdir(t, 0755);
      *s = '/';
    }
  }

  return mkdir(t, 0755) == 0 || errno == EEXIST;
}

static long nb_mtime(const char *path) {
  struct stat st;

  return stat(path, &st) == 0 ? (long)st.st_mtime : -1;
}

// 1 if out is missing or older than any input
static int nb_needs_rebuild(const char *out, const char **in, size_t n) {
  long o = nb_mtime(out);
  if (o < 0) {
    return 1;
  }

  for (size_t i = 0; i < n; i++) {
    if (nb_mtime(in[i]) > o) {
      return 1;
    }
  }

  return 0;
}

static int nb_rm_tree(const char *path) {
  nb_cmd c = {0};
  nb_cmd_add(&c, "rm", "-rf", path);

  return nb_run(&c);
}

static int nb_copy(const char *from, const char *to, const char *mode) {
  nb_cmd c = {0};
  nb_cmd_add(&c, "install", "-m", mode, from, to);

  return nb_run(&c);
}

// Rebuild this build program when its source (or this header) is newer, then
// re-run it. Call first thing in main()
#define NB_REBUILD_SELF(argc, argv, cc)                                        \
  do {                                                                         \
    const char *nb_src_[] = {__FILE__, "nb.h"};                                \
    if (nb_needs_rebuild((argv)[0], nb_src_, 2)) {                             \
      nb_cmd nb_c_ = {0};                                                      \
      nb_cmd_add(&nb_c_, cc, "-o", (argv)[0], __FILE__);                       \
      if (!nb_run(&nb_c_)) {                                                   \
        exit(1);                                                               \
      }                                                                        \
      nb_cmd nb_r_ = {0};                                                      \
      for (int nb_i_ = 0; nb_i_ < (argc); nb_i_++) {                           \
        nb_push(&nb_r_, (argv)[nb_i_]);                                        \
      }                                                                        \
      execv(nb_r_.v[0], (char *const *)nb_r_.v);                               \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#endif
