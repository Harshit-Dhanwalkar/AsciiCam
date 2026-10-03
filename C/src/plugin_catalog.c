/*
 * Plugin catalog: shared logic on top of five per-platform primitives
 *
 *   pc_list_c_files  names of *.c files in a directory
 *   pc_make_dirs     mkdir -p
 *   pc_spawn         start `make` on plugin rule, output to a log file
 *   pc_reap          non-blocking (or blocking) wait for that child
 *   pc_kill          terminate and reap child
 *
 * System headers must come before nolibc.h on libc platforms: nolibc redirects
 * names like strlen/signal with macros that would rewrite their declarations
 */

#include "platform.h"

#if defined(__LINUX_NOLIBC__)
#define PC_NOLIBC_LINUX 1
#elif defined(PLATFORM_WINDOWS)
#define PC_WINDOWS 1
#else
#define PC_POSIX 1
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "nolibc.h"

#include "plugin_catalog.h"

// Extension Makefile's plugin rule produces on this platform. Overridable so
// POSIX code can be exercised on a Linux host: -DPC_SO_EXT=\"so\"
#ifndef PC_SO_EXT
#if defined(PC_WINDOWS)
#define PC_SO_EXT "dll"
#elif defined(PLATFORM_MACOS)
#define PC_SO_EXT "dylib"
#else
#define PC_SO_EXT "so"
#endif
#endif

#define PC_FILE_LEN 64
#define SCAN_FILE_BYTES 32768

#ifndef O_BINARY
#define O_BINARY 0
#endif

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

static void set_err(catalog_entry_t *e, const char *msg) {
  cat_copy(e->err, sizeof(e->err), msg);
}

static void strip_trailing_slash(char *d) {
  size_t n = nl_strlen(d);
  while (n > 1 && (d[n - 1] == '/' || d[n - 1] == '\\')) {
    d[--n] = '\0';
  }
}

void catalog_init(plugin_catalog_t *c, const char *src_dir,
                  const char *cache_dir, char **envp) {
  nl_memset(c, 0, sizeof(*c));
  cat_copy(c->src_dir, sizeof(c->src_dir), src_dir);
  cat_copy(c->cache_dir, sizeof(c->cache_dir), cache_dir);
  c->envp = envp;

  // "dir/" and "dir" must build same paths
  strip_trailing_slash(c->src_dir);
  strip_trailing_slash(c->cache_dir);
}

int catalog_find(const plugin_catalog_t *c, const char *name) {
  for (int i = 0; i < c->count; i++) {
    if (nl_strcmp(c->e[i].name, name) == 0) {
      return i;
    }
  }

  return -1;
}

int catalog_supported(void) { return 1; }

/* Platform primitives  */

#if defined(PC_NOLIBC_LINUX)

#define NL_WNOHANG 1
#define NL_O_DIRECTORY 0200000
#define NL_SIGTERM 15

struct nl_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[];
};

static int pc_list_c_files(const char *dir, char names[][PC_FILE_LEN],
                           int max) {
  int fd = open(dir, O_RDONLY | NL_O_DIRECTORY, 0);
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
      if (len < PC_FILE_LEN && count < max) {
        cat_copy(names[count++], PC_FILE_LEN, d->d_name);
      }
    }
  }

  close(fd);

  return count;
}

static void pc_make_dirs(const char *path) {
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

static int pc_spawn(const plugin_catalog_t *c, const catalog_entry_t *e,
                    pc_proc_t *out, char *err, size_t errsz) {
  int logfd = open(e->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (logfd < 0) {
    cat_copy(err, errsz, "cannot open build log in cache dir");

    return -1;
  }

  // Use the Makefile's own plugin rule so flags always match a normal build
  char a_filter[CATALOG_PATH_LEN + 16];
  char a_build[CATALOG_PATH_LEN + 16];
  nl_snprintf(a_filter, sizeof(a_filter), "FILTERDIR=%s", c->src_dir);
  nl_snprintf(a_build, sizeof(a_build), "BUILDDIR=%s", c->cache_dir);

  static char *empty_env[] = {(char *)0};
  char **envp = c->envp ? c->envp : empty_env;

  // /usr/bin/env resolves make through PATH; without an environment there is
  // no PATH, so fall back to the usual absolute location
  const char *prog = c->envp ? "/usr/bin/env" : "/usr/bin/make";
  char *argv_env[] = {
      "env", "make", "-s", a_filter, a_build, (char *)e->so_path, (char *)0};
  char *argv_make[] = {"make",   "-s", a_filter, a_build, (char *)e->so_path,
                       (char *)0};
  char **argv = c->envp ? argv_env : argv_make;

  long pid = __sc1(SYS_fork, 0);
  if (pid < 0) {
    close(logfd);
    cat_copy(err, errsz, "fork failed");

    return -1;
  }

  if (pid == 0) {
    // child: no C library state to worry about, just rewire fds and exec
    int nul = open("/dev/null", O_RDONLY, 0);
    if (nul >= 0) {
      __sc2(SYS_dup2, nul, 0);
    }
    __sc2(SYS_dup2, logfd, 1);
    __sc2(SYS_dup2, logfd, 2);
    __sc3(SYS_execve, (long)prog, (long)argv, (long)envp);
    __sc1(SYS_exit_group, 127);
    __builtin_unreachable();
  }

  close(logfd);
  *out = pid;

  return 0;
}

// 1 finished (*code = exit status, -1 if killed by a signal), 0 running,
// -1 lost track of the child
static int pc_reap(pc_proc_t h, int block, int *code) {
  int status = 0;
  long r = __sc4(SYS_wait4, (long)h, (long)&status, block ? 0 : NL_WNOHANG, 0);
  if (r == 0) {
    return 0;
  }
  if (r < 0) {
    return -1;
  }

  *code = ((status & 0x7f) == 0) ? ((status >> 8) & 0xff) : -1;

  return 1;
}

static void pc_kill(pc_proc_t h) {
  int code;
  __sc2(SYS_kill, (long)h, NL_SIGTERM);
  pc_reap(h, 1, &code);
}

#elif defined(PC_POSIX)

extern char **environ;

static int pc_list_c_files(const char *dir, char names[][PC_FILE_LEN],
                           int max) {
  DIR *d = opendir(dir);
  if (!d) {
    return 0;
  }

  int count = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    size_t len = nl_strlen(de->d_name);
    if (len < PC_FILE_LEN && count < max) {
      cat_copy(names[count++], PC_FILE_LEN, de->d_name);
    }
  }
  closedir(d);

  return count;
}

static void pc_make_dirs(const char *path) {
  char tmp[CATALOG_PATH_LEN];
  cat_copy(tmp, sizeof(tmp), path);

  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

static int pc_spawn(const plugin_catalog_t *c, const catalog_entry_t *e,
                    pc_proc_t *out, char *err, size_t errsz) {
  char a_filter[CATALOG_PATH_LEN + 16];
  char a_build[CATALOG_PATH_LEN + 16];
  nl_snprintf(a_filter, sizeof(a_filter), "FILTERDIR=%s", c->src_dir);
  nl_snprintf(a_build, sizeof(a_build), "BUILDDIR=%s", c->cache_dir);

  char *argv[] = {"make",   "-s", a_filter, a_build, (char *)e->so_path,
                  (char *)0};

  // posix_spawn rather than fork: macOS has AVFoundation threads running, and
  // spawn is the supported way to start a child from a threaded process
  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0) {
    cat_copy(err, errsz, "cannot set up build process");

    return -1;
  }
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, e->log_path,
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);

  pid_t pid = 0;
  int rc =
      posix_spawnp(&pid, "make", &fa, NULL, argv, c->envp ? c->envp : environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    cat_copy(err, errsz, "build tool not found (need make)");

    return -1;
  }

  *out = (pc_proc_t)pid;

  return 0;
}

static int pc_reap(pc_proc_t h, int block, int *code) {
  int status = 0;
  pid_t r = waitpid((pid_t)h, &status, block ? 0 : WNOHANG);
  if (r == 0) {
    return 0;
  }
  if (r < 0) {
    return -1;
  }

  *code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  return 1;
}

static void pc_kill(pc_proc_t h) {
  int code;
  kill((pid_t)h, 15 /* SIGTERM */);
  pc_reap(h, 1, &code);
}

#else /* PC_WINDOWS */

static int pc_list_c_files(const char *dir, char names[][PC_FILE_LEN],
                           int max) {
  char pattern[CATALOG_PATH_LEN + 8];
  nl_snprintf(pattern, sizeof(pattern), "%s/*.c", dir);

  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return 0;
  }

  int count = 0;
  do {
    size_t len = nl_strlen(fd.cFileName);
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        len < PC_FILE_LEN && count < max) {
      cat_copy(names[count++], PC_FILE_LEN, fd.cFileName);
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);

  return count;
}

static void pc_make_dirs(const char *path) {
  char tmp[CATALOG_PATH_LEN];
  cat_copy(tmp, sizeof(tmp), path);

  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/' || *p == '\\') {
      char sep = *p;
      *p = '\0';
      CreateDirectoryA(tmp, NULL);
      *p = sep;
    }
  }
  CreateDirectoryA(tmp, NULL);
}

static int pc_spawn(const plugin_catalog_t *c, const catalog_entry_t *e,
                    pc_proc_t *out, char *err, size_t errsz) {
  (void)c->envp; // child inherits this process's environment

  SECURITY_ATTRIBUTES sa;
  nl_memset(&sa, 0, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE hlog = CreateFileA(e->log_path, GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hlog == INVALID_HANDLE_VALUE) {
    cat_copy(err, errsz, "cannot open build log in cache dir");

    return -1;
  }
  HANDLE hnul =
      CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

  STARTUPINFOA si;
  nl_memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = hnul;
  si.hStdOutput = hlog;
  si.hStdError = hlog;

  // Git Bash / MSYS ships make; MinGW toolchains ship mingw32-make
  static const char *tools[] = {"make", "mingw32-make"};
  PROCESS_INFORMATION pi;
  int started = 0;

  for (int t = 0; t < 2 && !started; t++) {
    char cmd[4 * CATALOG_PATH_LEN];
    nl_snprintf(cmd, sizeof(cmd),
                "%s -s \"FILTERDIR=%s\" \"BUILDDIR=%s\" \"%s\"", tools[t],
                c->src_dir, c->cache_dir, e->so_path);
    nl_memset(&pi, 0, sizeof(pi));
    started = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, NULL, &si, &pi) != 0;
  }

  CloseHandle(hlog);
  if (hnul != INVALID_HANDLE_VALUE) {
    CloseHandle(hnul);
  }

  if (!started) {
    cat_copy(err, errsz, "build tool not found (need make)");

    return -1;
  }

  CloseHandle(pi.hThread);
  *out = (pc_proc_t)(size_t)pi.hProcess;

  return 0;
}

static int pc_reap(pc_proc_t h, int block, int *code) {
  HANDLE p = (HANDLE)(size_t)h;
  DWORD w = WaitForSingleObject(p, block ? INFINITE : 0);
  if (w == WAIT_TIMEOUT) {
    return 0;
  }
  if (w != WAIT_OBJECT_0) {
    return -1;
  }

  DWORD ec = 1;
  GetExitCodeProcess(p, &ec);
  CloseHandle(p);
  *code = (int)ec;

  return 1;
}

static void pc_kill(pc_proc_t h) {
  HANDLE p = (HANDLE)(size_t)h;
  int code;
  TerminateProcess(p, 1);
  if (pc_reap(h, 1, &code) < 0) {
    CloseHandle(p);
  }
}

#endif

/* Shared logic  */

// "dir/name.ext" must fit a catalog path buffer, or it would be silently cut
static int path_fits(const char *dir, const char *name, size_t extra) {
  return nl_strlen(dir) + nl_strlen(name) + extra + 2 <= CATALOG_PATH_LEN;
}

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

  int fd = open(path, O_RDONLY | O_BINARY, 0);
  if (fd < 0) {
    return 0;
  }

  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
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

static int file_exists(const char *path) {
  int fd = open(path, O_RDONLY | O_BINARY, 0);
  if (fd < 0) {
    return 0;
  }

  close(fd);

  return 1;
}

// Plugin stems (file name minus ".c") in dir that look like plugins, sorted
static int list_sources(const char *dir, char names[][CATALOG_NAME_LEN],
                        int max) {
  static char files[CATALOG_MAX * 4][PC_FILE_LEN];
  int nfiles = pc_list_c_files(dir, files, CATALOG_MAX * 4);
  int count = 0;

  for (int i = 0; i < nfiles && count < max; i++) {
    size_t len = nl_strlen(files[i]);
    if (len < 3 || files[i][len - 2] != '.' || files[i][len - 1] != 'c') {
      continue;
    }

    char stem[CATALOG_NAME_LEN];
    if (len - 2 >= sizeof(stem)) {
      continue;
    }

    nl_memcpy(stem, files[i], len - 2);
    stem[len - 2] = '\0';
    if (!name_ok(stem)) {
      continue;
    }

    char path[CATALOG_PATH_LEN];
    if (!path_fits(dir, files[i], 0)) {
      continue;
    }

    nl_snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
    if (!file_has_plugin_get(path)) {
      continue;
    }

    cat_copy(names[count++], CATALOG_NAME_LEN, stem);
  }

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

int catalog_scan(plugin_catalog_t *c) {
  static char names[CATALOG_MAX][CATALOG_NAME_LEN];
  int found = list_sources(c->src_dir, names, CATALOG_MAX);

  for (int i = 0; i < found && c->count < CATALOG_MAX; i++) {
    if (catalog_find(c, names[i]) >= 0) {
      continue;
    }

    // longest derived path is "<cache>/<name>.<ext>" or ".log"
    if (!path_fits(c->src_dir, names[i], 2) ||
        !path_fits(c->cache_dir, names[i], 6)) {
      continue;
    }

    catalog_entry_t *e = &c->e[c->count++];
    nl_memset(e, 0, sizeof(*e));
    cat_copy(e->name, sizeof(e->name), names[i]);

    nl_snprintf(e->src_path, sizeof(e->src_path), "%s/%s.c", c->src_dir,
                names[i]);
    nl_snprintf(e->so_path, sizeof(e->so_path), "%s/%s.%s", c->cache_dir,
                names[i], PC_SO_EXT);
    nl_snprintf(e->log_path, sizeof(e->log_path), "%s/%s.log", c->cache_dir,
                names[i]);

    e->state = file_exists(e->so_path) ? PC_READY : PC_AVAILABLE;
  }

  return c->count;
}

int catalog_start_build(plugin_catalog_t *c, int idx) {
  if (idx < 0 || idx >= c->count) {
    return -1;
  }

  catalog_entry_t *e = &c->e[idx];
  if (e->state == PC_COMPILING || e->state == PC_ACTIVE) {
    return 0;
  }

  pc_make_dirs(c->cache_dir);

  char err[CATALOG_ERR_LEN];
  pc_proc_t pid = 0;
  if (pc_spawn(c, e, &pid, err, sizeof(err)) < 0) {
    e->state = PC_FAILED;
    set_err(e, err);

    return -1;
  }

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

  int fd = open(log_path, O_RDONLY | O_BINARY, 0);
  ssize_t n = -1;
  if (fd >= 0) {
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
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
      if (*q == '/' || *q == '\\') {
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

    int code = 0;
    int r = pc_reap(e->pid, 0, &code);
    if (r == 0) {
      continue; // still running
    }

    e->pid = 0;
    if (r < 0) {
      e->state = PC_FAILED;
      set_err(e, "lost track of build process");
    } else if (code == 0 && file_exists(e->so_path)) {
      e->state = PC_READY;
      e->err[0] = '\0';
    } else {
      e->state = PC_FAILED;
      log_first_error(e->log_path, code, e->err, sizeof(e->err));
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

    pc_kill(e->pid);
    e->pid = 0;
    e->state = PC_FAILED;
    set_err(e, "cancelled");
  }
}
