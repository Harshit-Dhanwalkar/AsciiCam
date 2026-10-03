#include "nolibc.h"

#include "plugin_catalog.h"

const char *catalog_state_name(pc_state_t s) {
  switch (s) {
  case PC_AVAILABLE:
    return "available";
  case PC_READY:
    return "built";
  case PC_COMPILING:
    return "compiling...";
  case PC_FAILED:
    return "failed";
  case PC_ACTIVE:
    return "active";
  }

  return "?";
}

static void cat_copy(char *dst, size_t n, const char *src) {
  nl_strncpy_safe(dst, src, n);
}

void catalog_init(plugin_catalog_t *c, const char *src_dir,
                  const char *cache_dir, char **envp) {
  nl_memset(c, 0, sizeof(*c));
  cat_copy(c->src_dir, sizeof(c->src_dir), src_dir);
  cat_copy(c->cache_dir, sizeof(c->cache_dir), cache_dir);
  c->envp = envp;

  // "dir/" and "dir" must build same paths
  for (char *d = c->src_dir; *d; d++) {
    if (d[0] == '/' && d[1] == '\0' && d != c->src_dir) {
      *d = '\0';
    }
  }
  for (char *d = c->cache_dir; *d; d++) {
    if (d[0] == '/' && d[1] == '\0' && d != c->cache_dir) {
      *d = '\0';
    }
  }
}

int catalog_find(const plugin_catalog_t *c, const char *name) {
  for (int i = 0; i < c->count; i++) {
    if (nl_strcmp(c->e[i].name, name) == 0) {
      return i;
    }
  }

  return -1;
}

#ifdef __LINUX_NOLIBC__

#define NL_WNOHANG 1
#define NL_O_DIRECTORY 0200000
#define NL_SIGTERM 15
#define NAME_MAX_FS 64
#define SCAN_FILE_BYTES 32768

int catalog_supported(void) { return 1; }

static int name_ok(const char *s) {
  if (!*s) {
    return 0;
  }

  for (; *s; s++) {
    char ch = *s;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
      return 0;
    }
  }

  return 1;
}

static int file_has_plugin_get(const char *path) {
  static char buf[SCAN_FILE_BYTES];
  static const char needle[] = "plugin_get";

  int fd = nl_open(path, O_RDONLY, 0);
  if (fd < 0) {
    return 0;
  }

  ssize_t n = nl_read(fd, buf, sizeof(buf) - 1);
  nl_close(fd);
  if (n < (ssize_t)(sizeof(needle) - 1)) {
    return 0;
  }

  for (ssize_t i = 0; i + (ssize_t)sizeof(needle) - 1 <= n; i++) {
    size_t k = 0;
    while (k < sizeof(needle) - 1 && buf[i + (ssize_t)k] == needle[k]) {
      k++;
    }
    if (k == sizeof(needle) - 1) {
      return 1;
    }
  }

  return 0;
}

struct nl_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[];
};

// Collect plugin stems (file name minus ".c") from dir, sorted
static int list_sources(const char *dir, char names[][CATALOG_NAME_LEN],
                        int max) {
  int fd = nl_open(dir, O_RDONLY | NL_O_DIRECTORY, 0);
  if (fd < 0) {
    return 0;
  }

  static char dbuf[8192] __attribute__((aligned(8)));
  int count = 0;

  for (;;) {
    long n = __sc3(SYS_getdents64, fd, (long)dbuf, (long)sizeof(dbuf));
    if (n <= 0) {
      break;
    }

    for (long off = 0; off < n;) {
      struct nl_dirent64 *d = (struct nl_dirent64 *)(dbuf + off);
      off += d->d_reclen;

      size_t len = nl_strlen(d->d_name);
      if (len < 3 || len >= NAME_MAX_FS) {
        continue;
      }
      if (d->d_name[len - 2] != '.' || d->d_name[len - 1] != 'c') {
        continue;
      }

      char stem[CATALOG_NAME_LEN];
      if (len - 2 >= sizeof(stem)) {
        continue;
      }
      nl_memcpy(stem, d->d_name, len - 2);
      stem[len - 2] = '\0';
      if (!name_ok(stem)) {
        continue;
      }

      char path[CATALOG_PATH_LEN];
      nl_snprintf(path, sizeof(path), "%s/%s", dir, d->d_name);
      if (!file_has_plugin_get(path)) {
        continue;
      }

      if (count < max) {
        cat_copy(names[count++], CATALOG_NAME_LEN, stem);
      }
    }
  }

  nl_close(fd);

  // insertion sort: directory order is arbitrary
  for (int i = 1; i < count; i++) {
    char tmp[CATALOG_NAME_LEN];
    cat_copy(tmp, sizeof(tmp), names[i]);
    int j = i - 1;
    while (j >= 0 && nl_strcmp(names[j], tmp) > 0) {
      cat_copy(names[j + 1], CATALOG_NAME_LEN, names[j]);
      j--;
    }
    cat_copy(names[j + 1], CATALOG_NAME_LEN, tmp);
  }

  return count;
}

static int file_exists(const char *path) {
  int fd = nl_open(path, O_RDONLY, 0);
  if (fd < 0) {
    return 0;
  }
  nl_close(fd);

  return 1;
}

int catalog_scan(plugin_catalog_t *c) {
  static char names[CATALOG_MAX][CATALOG_NAME_LEN];
  int found = list_sources(c->src_dir, names, CATALOG_MAX);

  for (int i = 0; i < found && c->count < CATALOG_MAX; i++) {
    if (catalog_find(c, names[i]) >= 0) {
      continue;
    }

    catalog_entry_t *e = &c->e[c->count++];
    nl_memset(e, 0, sizeof(*e));
    cat_copy(e->name, sizeof(e->name), names[i]);
    nl_snprintf(e->src_path, sizeof(e->src_path), "%s/%s.c", c->src_dir,
                names[i]);
    nl_snprintf(e->so_path, sizeof(e->so_path), "%s/%s.so", c->cache_dir,
                names[i]);
    nl_snprintf(e->log_path, sizeof(e->log_path), "%s/%s.log", c->cache_dir,
                names[i]);
    e->state = file_exists(e->so_path) ? PC_READY : PC_AVAILABLE;
  }

  return c->count;
}

// mkdir -p
static void make_dirs(const char *path) {
  char tmp[CATALOG_PATH_LEN];
  cat_copy(tmp, sizeof(tmp), path);

  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      __sc2(SYS_mkdir, (long)tmp, 0755);
      *p = '/';
    }
  }
  __sc2(SYS_mkdir, (long)tmp, 0755);
}

static void set_err(catalog_entry_t *e, const char *msg) {
  cat_copy(e->err, sizeof(e->err), msg);
}

int catalog_start_build(plugin_catalog_t *c, int idx) {
  if (idx < 0 || idx >= c->count) {
    return -1;
  }

  catalog_entry_t *e = &c->e[idx];
  if (e->state == PC_COMPILING || e->state == PC_ACTIVE) {
    return 0;
  }

  make_dirs(c->cache_dir);

  int logfd = nl_open(e->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (logfd < 0) {
    e->state = PC_FAILED;
    set_err(e, "cannot open build log in cache dir");

    return -1;
  }

  // Use Makefile's own plugin rule so flags always match a normal build
  char a_filter[CATALOG_PATH_LEN + 16];
  char a_build[CATALOG_PATH_LEN + 16];
  nl_snprintf(a_filter, sizeof(a_filter), "FILTERDIR=%s", c->src_dir);
  nl_snprintf(a_build, sizeof(a_build), "BUILDDIR=%s", c->cache_dir);

  static char *empty_env[] = {(char *)0};
  char **envp = c->envp ? c->envp : empty_env;

  // /usr/bin/env resolves make through PATH; without an environment there is
  // no PATH, so fall back to usual absolute location
  const char *prog = c->envp ? "/usr/bin/env" : "/usr/bin/make";
  char *argv_env[] = {"env",   "make",     "-s",     a_filter,
                      a_build, e->so_path, (char *)0};
  char *argv_make[] = {"make", "-s", a_filter, a_build, e->so_path, (char *)0};
  char **argv = c->envp ? argv_env : argv_make;

  long pid = __sc1(SYS_fork, 0);
  if (pid < 0) {
    nl_close(logfd);
    e->state = PC_FAILED;
    set_err(e, "fork failed");

    return -1;
  }

  if (pid == 0) {
    // child: no C library state to worry about, just rewire fds and exec
    int nul = nl_open("/dev/null", O_RDONLY, 0);
    if (nul >= 0) {
      __sc2(SYS_dup2, nul, 0);
    }
    __sc2(SYS_dup2, logfd, 1);
    __sc2(SYS_dup2, logfd, 2);
    __sc3(SYS_execve, (long)prog, (long)argv, (long)envp);
    __sc1(SYS_exit_group, 127);
    __builtin_unreachable();
  }

  nl_close(logfd);
  e->pid = pid;
  e->state = PC_COMPILING;
  e->err[0] = '\0';

  return 0;
}

// Pull most useful line out of build log for status display
static void log_first_error(const char *log_path, int exit_code, char *out,
                            size_t outsz) {
  static char buf[8192];
  out[0] = '\0';

  int fd = nl_open(log_path, O_RDONLY, 0);
  ssize_t n = -1;
  if (fd >= 0) {
    n = nl_read(fd, buf, sizeof(buf) - 1);
    nl_close(fd);
  }

  if (n <= 0) {
    if (exit_code == 127) {
      cat_copy(out, outsz, "build tool not found (need make)");
    } else {
      cat_copy(out, outsz, "build failed (no output)");
    }

    return;
  }

  buf[n] = '\0';

  const char *best = (const char *)0;
  const char *first = (const char *)0;
  const char *p = buf;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      p++;
    }
    if (!*p) {
      break;
    }

    const char *line = p;
    const char *q = p;
    while (*q && *q != '\n') {
      q++;
    }

    if (!first) {
      first = line;
    }

    for (const char *s = line; s + 5 <= q; s++) {
      if (s[0] == 'e' && s[1] == 'r' && s[2] == 'r' && s[3] == 'o' &&
          s[4] == 'r') {
        best = line;

        break;
      }
    }

    if (best) {
      break;
    }

    p = q;
  }

  const char *src = best ? best : (first ? first : buf);
  if (best) {
    // "/long/dir/x.c:3:5: error: ..." -> "x.c:3:5: error: ..." so message
    // itself still fits in a narrow toast
    const char *q = best;
    while (*q && *q != '\n' &&
           !(q[0] == 'e' && q[1] == 'r' && q[2] == 'r' && q[3] == 'o' &&
             q[4] == 'r')) {
      if (*q == '/') {
        src = q + 1;
      }

      q++;
    }
  }

  size_t i = 0;
  while (src[i] && src[i] != '\n' && src[i] != '\r' && i + 1 < outsz) {
    out[i] = src[i];
    i++;
  }

  out[i] = '\0';
}

int catalog_poll(plugin_catalog_t *c, int *idx) {
  for (int i = 0; i < c->count; i++) {
    catalog_entry_t *e = &c->e[i];
    if (e->state != PC_COMPILING) {
      continue;
    }

    int status = 0;
    long r = __sc4(SYS_wait4, e->pid, (long)&status, NL_WNOHANG, 0);
    if (r == 0) {
      continue; // still running
    }

    e->pid = 0;
    if (r < 0) {
      e->state = PC_FAILED;
      set_err(e, "lost track of build process");
    } else {
      int exited = (status & 0x7f) == 0;
      int code = (status >> 8) & 0xff;
      if (exited && code == 0 && file_exists(e->so_path)) {
        e->state = PC_READY;
        e->err[0] = '\0';
      } else {
        e->state = PC_FAILED;
        log_first_error(e->log_path, exited ? code : -1, e->err,
                        sizeof(e->err));
      }
    }

    if (idx) {
      *idx = i;
    }

    return 1;
  }

  return 0;
}

void catalog_cleanup(plugin_catalog_t *c) {
  for (int i = 0; i < c->count; i++) {
    catalog_entry_t *e = &c->e[i];
    if (e->state != PC_COMPILING) {
      continue;
    }

    __sc2(SYS_kill, e->pid, NL_SIGTERM);
    int status = 0;
    __sc4(SYS_wait4, e->pid, (long)&status, 0, 0);
    e->pid = 0;
    e->state = PC_FAILED;
    set_err(e, "cancelled");
  }
}

#else // macOS / Windows

int catalog_supported(void) { return 0; }

int catalog_scan(plugin_catalog_t *c) { return c->count; }

int catalog_start_build(plugin_catalog_t *c, int idx) {
  if (idx >= 0 && idx < c->count) {
    c->e[idx].state = PC_FAILED;
    cat_copy(c->e[idx].err, sizeof(c->e[idx].err),
             "runtime build not supported on this platform yet");
  }

  return -1;
}

int catalog_poll(plugin_catalog_t *c, int *idx) {
  (void)c;
  (void)idx;

  return 0;
}

void catalog_cleanup(plugin_catalog_t *c) { (void)c; }

#endif
