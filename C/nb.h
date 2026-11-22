#ifndef NB_H
#define NB_H

/*
 * nb.h - tiny build-in-C helper
 * Build script is a C program that includes this header; it rebuilds itself
 * when its source changes
 * Needs only a C compiler on Linux / macOS (POSIX) or Windows (MinGW-w64, Win32
 * API only)
 * NOTE: no make, no cmake, no sh, no ls / install / rm are required
 */

// TODO: Use lib/
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdde f.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#define NB_WIN 1
#define NB_EXE ".exe"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#define NB_WIN 0
#define NB_EXE ""
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef struct {
  const char **v;
  size_t n, cap;
} nb_cmd;

// Terminal colors for [nb] tag, by severity:
//   info    -> blue      pass  -> green
//   fail    -> red       error -> red
//   warning -> yellow
// Codes are emitted only when stderr is a TTY
#define NB_C_RESET "\033[0m"
#define NB_C_INFO "\033[34m" // blue
#define NB_C_PASS "\033[32m" // green
#define NB_C_FAIL "\033[31m" // red
#define NB_C_WARN "\033[33m" // yellow

static const char *nb_color(const char *code) {
  static int tty = -1;
  if (tty < 0) {
#if NB_WIN
    // Colors only when stderr is a console that accepts ANSI sequences
    // (ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004)
    DWORD m = 0;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    tty = GetConsoleMode(h, &m) && SetConsoleMode(h, m | 0x0004);
#else
    tty = isatty(2);
#endif
  }

  return tty ? code : "";
}

// Single entry point for every severity-tagged log line
// Color is applied to [nb] tag only; message text stays uncolored so colored
// output is still greppable
// Whole line is reset before newline
static void nb_logv(const char *color, const char *fmt, ...) {
  fprintf(stderr, "%s[nb]%s ", nb_color(color), nb_color(NB_C_RESET));
  va_list a;
  va_start(a, fmt);
  vfprintf(stderr, fmt, a);
  va_end(a);
  fputc('\n', stderr);
}

// Default logger -> info / blue
#define nb_log(...) nb_logv(NB_C_INFO, __VA_ARGS__)
// Severity-specific variants
#define nb_log_info(...) nb_logv(NB_C_INFO, __VA_ARGS__)
#define nb_log_pass(...) nb_logv(NB_C_PASS, __VA_ARGS__)
#define nb_log_fail(...) nb_logv(NB_C_FAIL, __VA_ARGS__)
#define nb_log_error(...) nb_logv(NB_C_FAIL, __VA_ARGS__)
#define nb_log_warn(...) nb_logv(NB_C_WARN, __VA_ARGS__)

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
  fprintf(stderr, "%s[nb]%s $", nb_color(NB_C_INFO), nb_color(NB_C_RESET));
  for (size_t i = 0; i < c->n; i++) {
    fprintf(stderr, " %s", c->v[i]);
  }

  fputc('\n', stderr);
}

// Process handle: a pid on POSIX, a HANDLE on Windows
#if NB_WIN
typedef HANDLE nb_proc;
#define NB_NO_PROC NULL
#else
typedef pid_t nb_proc;
#define NB_NO_PROC ((pid_t) - 1)
#endif

// Build a Windows command line from argv, quoted way MSVC runtime and
// CommandLineToArgvW read it back (backslashes only matter before a quote)
// Plain C, so it is same on every host and can be tested anywhere
// A '/' in argv[0] is turned into '\' because CreateProcess wants that
#if defined(__GNUC__)
__attribute__((unused))
#endif
static char *nb_cmdline(const nb_cmd *c) {
  size_t total = 1;
  for (size_t i = 0; i < c->n; i++) {
    total += 2 * strlen(c->v[i]) + 3;
  }

  char *out = (char *)malloc(total);
  if (!out) {
    abort();
  }

  char *o = out;
  for (size_t i = 0; i < c->n; i++) {
    const char *a = c->v[i];
    if (i == 0 && strchr(a, '/')) {
      char *w = nb_fmt("%s", a);
      for (char *s = w; *s; s++) {
        if (*s == '/') {
          *s = '\\';
        }
      }

      a = w;
    }

    if (i) {
      *o++ = ' ';
    }

    int quote = !*a || strpbrk(a, " \t\n\v\"") != NULL;
    if (quote) {
      *o++ = '"';
    }

    for (const char *p = a;; p++) {
      size_t bs = 0;
      while (*p == '\\') {
        p++;
        bs++;
      }

      if (!*p) {
        // Trailing backslashes must not escape closing quote
        for (size_t k = 0; k < bs * (quote ? 2 : 1); k++) {
          *o++ = '\\';
        }

        break;
      } else if (*p == '"') {
        for (size_t k = 0; k < bs * 2 + 1; k++) {
          *o++ = '\\';
        }

        *o++ = '"';
      } else {
        for (size_t k = 0; k < bs; k++) {
          *o++ = '\\';
        }

        *o++ = *p;
      }
    }

    if (quote) {
      *o++ = '"';
    }
  }

  *o = 0;

  return out;
}

// Start a command
// Returns a process handle or NB_NO_PROC
static nb_proc nb_start(const nb_cmd *c, const char *stdout_path) {
  nb_show(c);
#if NB_WIN
  char *cmd = nb_cmdline(c);
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof si);
  memset(&pi, 0, sizeof pi);
  si.cb = sizeof si;

  HANDLE out = INVALID_HANDLE_VALUE;
  if (stdout_path) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    out = CreateFileA(stdout_path, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out != INVALID_HANDLE_VALUE) {
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
      si.hStdOutput = out;
      si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
  }

  // lpApplicationName NULL: command is searched on PATH (.exe implied)
  BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, out != INVALID_HANDLE_VALUE,
                           0, NULL, NULL, &si, &pi);
  DWORD err = GetLastError();
  if (out != INVALID_HANDLE_VALUE) {
    CloseHandle(out);
  }

  free(cmd);
  if (!ok) {
    fprintf(stderr, "%s[nb]%s cannot run %s: Windows error %lu\n",
            nb_color(NB_C_FAIL), nb_color(NB_C_RESET), c->v[0],
            (unsigned long)err);

    return NB_NO_PROC;
  }

  CloseHandle(pi.hThread);

  return pi.hProcess;
#else
  pid_t p = fork();
  if (p == 0) {
    if (stdout_path) {
      int fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) {
        dup2(fd, 1);
      }
    }

    execvp(c->v[0], (char *const *)c->v);
    fprintf(stderr, "%s[nb]%s cannot run %s: %s\n", nb_color(NB_C_FAIL),
            nb_color(NB_C_RESET), c->v[0], strerror(errno));
    _exit(127);
  }

  return p;
#endif
}

// Wait for a command; returns its exit code, or -1 if it did not exit normally
static int nb_wait_code(nb_proc p) {
#if NB_WIN
  DWORD code = 1;
  if (p == NB_NO_PROC) {
    return -1;
  }

  if (WaitForSingleObject(p, INFINITE) != WAIT_OBJECT_0 ||
      !GetExitCodeProcess(p, &code)) {
    CloseHandle(p);

    return -1;
  }

  CloseHandle(p);

  return (int)code;
#else
  int st = 0;
  if (p < 0 || waitpid(p, &st, 0) < 0) {
    return -1;
  }

  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

static int nb_wait(nb_proc p) { return nb_wait_code(p) == 0; }

static int nb_run(const nb_cmd *c) { return nb_wait(nb_start(c, NULL)); }

// Run commands in parallel, up to jobs at a time (jobs <= 64)
typedef struct {
  nb_proc pid[64];
  int jobs;
  int running;
  int ok;
} nb_procs;

static void nb_procs_init(nb_procs *p, int jobs) {
  p->jobs = jobs < 1 ? 1 : jobs > 64 ? 64 : jobs;
  p->running = 0;
  p->ok = 1;
}

static void nb_procs_reap_one(nb_procs *p) {
#if NB_WIN
  DWORD r = WaitForMultipleObjects((DWORD)p->running, p->pid, FALSE, INFINITE);
  if (r >= WAIT_OBJECT_0 + (DWORD)p->running) {
    // Wait itself failed: give up on what's left instead of looping forever
    for (int i = 0; i < p->running; i++) {
      CloseHandle(p->pid[i]);
    }

    p->running = 0;
    p->ok = 0;

    return;
  }

  int i = (int)(r - WAIT_OBJECT_0);
  DWORD code = 1;
  if (!GetExitCodeProcess(p->pid[i], &code) || code != 0) {
    p->ok = 0;
  }

  CloseHandle(p->pid[i]);
  p->pid[i] = p->pid[--p->running];
#else
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
#endif
}

static void nb_procs_add(nb_procs *p, const nb_cmd *c) {
  while (p->running >= p->jobs) {
    nb_procs_reap_one(p);
  }

  nb_proc pid = nb_start(c, NULL);
  if (pid == NB_NO_PROC) {
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
#if NB_WIN
  // '/' and '\' both separate; the "C:" of a drive path just fails harmlessly
  for (char *s = t + 1; *s; s++) {
    if (*s == '/' || *s == '\\') {
      char sep = *s;
      *s = 0;
      CreateDirectoryA(t, NULL);
      *s = sep;
    }
  }

  return CreateDirectoryA(t, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
  for (char *s = t + 1; *s; s++) {
    if (*s == '/') {
      *s = 0;
      mkdir(t, 0755);
      *s = '/';
    }
  }

  return mkdir(t, 0755) == 0 || errno == EEXIST;
#endif
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

// rename(2) that replaces an existing target on every platform (plain rename()
// refuses to on Windows)
static int nb_rename(const char *from, const char *to) {
#if NB_WIN
  return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return rename(from, to) == 0;
#endif
}

static int nb_cmp_str_(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Append "dir/name" to out for every regular file in dir whose name ends in
// suffix (NULL or "" = all files), in sorted order; names starting with '.'
// are skipped. This is what `ls dir/*.h` did
static void nb_glob(nb_cmd *out, const char *dir, const char *suffix) {
  size_t start = out->n;
  const char *sfx = suffix ? suffix : "";
  size_t sl = strlen(sfx);
#if NB_WIN
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(nb_fmt("%s/*", dir), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }

  do {
    size_t n = strlen(fd.cFileName);
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        fd.cFileName[0] == '.' || n < sl ||
        strcmp(fd.cFileName + n - sl, sfx)) {
      continue;
    }

    nb_push(out, nb_fmt("%s/%s", dir, fd.cFileName));
  } while (FindNextFileA(h, &fd));

  FindClose(h);
#else
  DIR *d = opendir(dir);
  if (!d) {
    return;
  }

  struct dirent *e;
  while ((e = readdir(d))) {
    size_t n = strlen(e->d_name);
    if (e->d_name[0] == '.' || n < sl || strcmp(e->d_name + n - sl, sfx)) {
      continue;
    }

    char *p = nb_fmt("%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
      nb_push(out, p);
    }
  }

  closedir(d);
#endif
  if (out->n > start) {
    qsort(out->v + start, out->n - start, sizeof *out->v, nb_cmp_str_);
  }
}

// rm -rf. A missing path counts as success
static int nb_rm_tree(const char *path) {
#if NB_WIN
  DWORD a = GetFileAttributesA(path);
  if (a == INVALID_FILE_ATTRIBUTES) {
    return 1;
  }

  if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL); // read-only files
    return DeleteFileA(path) != 0;
  }

  // A junction or symlink is removed, never followed
  if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
    int ok = 1;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(nb_fmt("%s/*", path), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
          ok &= nb_rm_tree(nb_fmt("%s/%s", path, fd.cFileName));
        }
      } while (FindNextFileA(h, &fd));

      FindClose(h);
    }

    if (!ok) {
      return 0;
    }
  }

  return RemoveDirectoryA(path) != 0;
#else
  nb_cmd c = {0};
  nb_cmd_add(&c, "rm", "-rf", path);

  return nb_run(&c);
#endif
}

// Copy a file; `to` may be a directory. mode ("755") is ignored on Windows
static int nb_copy(const char *from, const char *to, const char *mode) {
#if NB_WIN
  (void)mode;
  const char *dst = to;
  DWORD a = GetFileAttributesA(to);
  if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
    const char *b = strrchr(from, '/');
    const char *b2 = strrchr(from, '\\');
    if (b2 > b) {
      b = b2;
    }

    dst = nb_fmt("%s/%s", to, b ? b + 1 : from);
  }

  fprintf(stderr, "%s[nb]%s copy %s -> %s\n", nb_color(NB_C_INFO),
          nb_color(NB_C_RESET), from, dst);

  return CopyFileA(from, dst, FALSE) != 0;
#else
  nb_cmd c = {0};
  nb_cmd_add(&c, "install", "-m", mode, from, to);

  return nb_run(&c);
#endif
}

// Rebuild this build program when its source (or this header) is newer, then
// re-run it
// Call first thing in main()
static void nb_rebuild_self(int argc, char **argv, const char *cc,
                            const char *src) {
  const char *in[] = {src, "nb.h"};
#if NB_WIN
  // A running .exe cannot be overwritten but it can be renamed: build next to
  // it, move old one aside, move new one in, run it and pass its exit code on
  // Old copy is removed on next start
  char self[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, self, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return; // Cannot tell where it live: keep running
  }

  // "<name>_old.exe" / "<name>_new.exe"
  char *base = nb_fmt("%s", self);
  size_t bl = strlen(base);
  if (bl > 4 && !_stricmp(base + bl - 4, ".exe")) {
    base[bl - 4] = 0;
  }

  char *old = nb_fmt("%s_old.exe", base);
  DeleteFileA(old);
  if (!nb_needs_rebuild(self, in, 2)) {
    return;
  }

  char *fresh = nb_fmt("%s_new.exe", base);
  nb_cmd c = {0};
  nb_cmd_add(&c, cc, "-o", fresh, src);
  if (!nb_run(&c)) {
    exit(1);
  }

  if (!nb_rename(self, old) || !nb_rename(fresh, self)) {
    nb_log_error("cannot replace %s", self);
    nb_rename(old, self);
    exit(1);
  }

  nb_cmd r = {0};
  nb_push(&r, self);
  for (int i = 1; i < argc; i++) {
    nb_push(&r, argv[i]);
  }

  int code = nb_wait_code(nb_start(&r, NULL));
  exit(code < 0 ? 1 : code);
#else
  if (nb_needs_rebuild(argv[0], in, 2)) {
    nb_cmd c = {0};
    nb_cmd_add(&c, cc, "-o", argv[0], src);
    if (!nb_run(&c)) {
      exit(1);
    }

    nb_cmd r = {0};
    for (int i = 0; i < argc; i++) {
      nb_push(&r, argv[i]);
    }

    execv(r.v[0], (char *const *)r.v);
    exit(1);
  }
#endif
}

#define NB_REBUILD_SELF(argc, argv, cc)                                        \
  nb_rebuild_self((argc), (argv), (cc), __FILE__)
#endif
