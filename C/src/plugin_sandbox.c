#include "nolibc.h"

#include "plugin_sandbox.h"

int plugin_sandbox_enabled = 0;
int plugin_sandbox_timeout_ms = 1000;

#if defined(__LINUX_NOLIBC__) && defined(__x86_64__)
// Reset every catchable signal to SIG_DFL and clear blocked-set
static void plugin_sandbox_reset_signals(void) {
  struct {
    long handler;
    unsigned long flags;
    long restorer;
    unsigned long mask;
  } sa;
  nl_memset(&sa, 0, sizeof sa); /* handler=0 -> SIG_DFL, no SA_RESTORER */
  for (int sig = 1; sig < 32; sig++) {
    if (sig == 9 || sig == 19) {
      continue; /* SIGKILL/SIGSTOP: never catchable */
    }
    __sc6(13 /* rt_sigaction */, sig, (long)&sa, 0, 8, 0, 0);
  }

  unsigned long empty = 0;
  __sc4(14 /* rt_sigprocmask */, 2 /* SIG_SETMASK */, (long)&empty, 0, 8);
}

// x86-64 system call numbers used here (not everything is in nl_syscall.h)
#define SB_SYS_MMAP 9
#define SB_SYS_MUNMAP 11
#define SB_SYS_RT_SIGACTION 13
#define SB_SYS_RT_SIGPROCMASK 14
#define SB_SYS_FORK 57
#define SB_SYS_WAIT4 61
#define SB_SYS_KILL 62
#define SB_SYS_GETPPID 110
#define SB_SYS_PRCTL 157
#define SB_SYS_FUTEX 202
#define SB_SYS_EXIT_GROUP 231
#define SB_SYS_CLOSE_RANGE 436
#define SB_SYS_CLOSE 3

#define SB_PROT_RW 3
#define SB_MAP_SHARED_ANON (0x01 | 0x20)
#define SB_PR_SET_PDEATHSIG 1
#define SB_PR_SET_DUMPABLE 4
#define SB_SYS_SETRLIMIT 160
#define SB_RLIMIT_CORE 4
#define SB_FUTEX_WAIT 0
#define SB_FUTEX_WAKE 1 // No PRIVATE flag: word is shared between processes
#define SB_SIGKILL 9
#define SB_WNOHANG 1
#define SB_ECHILD 10

// Request states of futex word
#define SB_IDLE 0U
#define SB_REQ 1U
#define SB_DONE 2U
#define SB_STOP 3U

#define SB_HDR_BYTES 4096UL
#define SB_MIN_CAP (1280UL * 720UL * 3UL)
#define SB_MAX_CAP (1UL << 28) // Sanity limit on a frame

// Control block, first page of shared mapping
// Child can write all of it; app only ever reads `state` from it
typedef struct {
  volatile unsigned state;
  unsigned kind; // 0 luma, 1 colour
  int w, h, param;
} sb_ctl_t;

typedef struct {
  sb_ctl_t *ctl;
  uint8_t *data;
  size_t cap;
  size_t map_bytes;
  long pid;
  void *handle; // Library child was started from
} sb_t;

typedef struct {
  long sec, nsec;
} sb_ts_t;

static long futex(volatile unsigned *addr, int op, unsigned val,
                  const sb_ts_t *ts) {
  return __sc6(SB_SYS_FUTEX, (long)addr, op, (long)val, (long)ts, 0, 0);
}

// Child side

static void child_exit(int code) {
  for (;;) {
    __sc1(SB_SYS_EXIT_GROUP, code);
  }
}

static void child_main(sb_ctl_t *ctl, uint8_t *data, const filter_plugin_t *fp,
                       plugin_rgb_fn rgb, long ppid) {
  // Die with app
  __sc3(SB_SYS_PRCTL, SB_PR_SET_PDEATHSIG, SB_SIGKILL, 0);
  if (__sc1(SB_SYS_GETPPID, 0) != ppid) {
    child_exit(120); // app went away between fork and here
  }

  // Crash handlers (restore terminal, print a message) make system calls filter
  // forbids: a plugin crash would be reported as a forbidden call
  // Default handlers and an empty mask make it what it was
  for (int sig = 1; sig <= 64; sig++) {
    if (sig != 9 && sig != 19) { // SIGKILL, SIGSTOP: cannot be changed
      struct {
        long handler;
        unsigned long flags;
        long restorer;
        unsigned long mask;
      } dfl = {0, 0, 0, 0};
      __sc6(SB_SYS_RT_SIGACTION, sig, (long)&dfl, 0, 8, 0, 0);
    }
  }

  unsigned long none = 0;
  __sc6(SB_SYS_RT_SIGPROCMASK, 2 /* SIG_SETMASK */, (long)&none, 0, 8, 0, 0);

  // A forbidden call or crash ends child with a core-dump signal
  // Dumping (maybe through a pipe to a crash handler) can keep a dead child
  // from being reaped past timeout, then death is reported as "did not answer
  // in time"
  // No dump: child is gone moment it is killed
  struct {
    unsigned long cur;
    unsigned long max;
  } nocore = {0, 0};
  __sc3(SB_SYS_SETRLIMIT, SB_RLIMIT_CORE, (long)&nocore, 0);
  __sc3(SB_SYS_PRCTL, SB_PR_SET_DUMPABLE, 0, 0);

  // Nothing inherited: no camera, no terminal, no files
  if (__sc3(SB_SYS_CLOSE_RANGE, 3, 0xffffffffL, 0) < 0) {
    for (int fd = 3; fd < 1024; fd++) {
      __sc1(SB_SYS_CLOSE, fd);
    }
  }

  static const int allow[] = {SB_SYS_FUTEX, SB_SYS_MUNMAP, SB_SYS_EXIT_GROUP};
  static const nl_seccomp_rule_t rules[] = {{SB_SYS_MMAP, 2, 0x4 /* EXEC */}};
  if (nl_seccomp_lockdown_ex(allow, 3, rules, 1, NL_SECCOMP_KILL) < 0) {
    child_exit(121);
  }

  for (;;) {
    unsigned st;
    while ((st = __atomic_load_n(&ctl->state, __ATOMIC_ACQUIRE)) != SB_REQ) {
      if (st == SB_STOP) {
        child_exit(0);
      }

      futex(&ctl->state, SB_FUTEX_WAIT, st, (const sb_ts_t *)0);
    }

    int param = ctl->param;
    int w = ctl->w;
    int h = ctl->h;
    if (ctl->kind == 0) {
      fp->process(data, w, h, &param);
    } else if (rgb) {
      rgb(data, w, h, &param);
    }

    __atomic_store_n(&ctl->state, SB_DONE, __ATOMIC_RELEASE);
    futex(&ctl->state, SB_FUTEX_WAKE, 1, (const sb_ts_t *)0);
  }
}

// App side

static const char *describe(int status, char *buf, size_t n) {
  if ((status & 0x7f) == 0) {
    nl_snprintf(buf, n, "exited unexpectedly (code %d)", (status >> 8) & 0xff);

    return buf;
  }

  switch (status & 0x7f) {
  case 31: {
    return "tried a system call that is not allowed";
  }
  case 11: {
    return "crashed (segmentation fault)";
  }
  case 7: {
    return "crashed (bus error)";
  }
  case 4: {
    return "crashed (illegal instruction)";
  }
  case 8: {
    return "crashed (arithmetic error)";
  }
  case 6: {
    return "crashed (abort)";
  }
  case 9: {
    return "was killed";
  }
  default: {
    nl_snprintf(buf, n, "died (signal %d)", status & 0x7f);

    return buf;
  }
  }
}

// Reap child: kill it first unless it is already gone
static void reap(sb_t *sb, int kill_it) {
  if (sb->pid <= 0) {
    return;
  }

  if (kill_it) {
    __sc3(SB_SYS_KILL, sb->pid, SB_SIGKILL, 0);
  }

  int status = 0;
  for (;;) {
    long r = __sc6(SB_SYS_WAIT4, sb->pid, (long)&status, 0, 0, 0, 0);
    if (r != -4 /* EINTR */) {
      break;
    }
  }

  sb->pid = -1;
}

static void destroy(plugin_loader_t *pl) {
  sb_t *sb = (sb_t *)pl->sandbox;
  if (!sb) {
    return;
  }

  if (sb->pid > 0) {
    __atomic_store_n(&sb->ctl->state, SB_STOP, __ATOMIC_RELEASE);
    futex(&sb->ctl->state, SB_FUTEX_WAKE, 1, (const sb_ts_t *)0);
    int gone = 0;
    for (int i = 0; i < 50 && !gone; i++) {
      int status = 0;
      long r = __sc6(SB_SYS_WAIT4, sb->pid, (long)&status, SB_WNOHANG, 0, 0, 0);
      if (r == sb->pid || r == -SB_ECHILD) {
        gone = 1;
        sb->pid = -1;
      } else {
        sb_ts_t t = {0, 1000000};
        sb_ts_t *tp = &t;
        __sc6(35 /* nanosleep */, (long)tp, 0, 0, 0, 0, 0);
      }
    }

    if (!gone) {
      reap(sb, 1);
    }
  }

  if (sb->ctl) {
    __sc3(SB_SYS_MUNMAP, (long)sb->ctl, (long)sb->map_bytes, 0);
  }

  nl_free(sb);
  pl->sandbox = NULL;
}

void plugin_sandbox_stop(plugin_loader_t *pl) { destroy(pl); }

long plugin_sandbox_pid(const plugin_loader_t *pl) {
  const sb_t *sb = (const sb_t *)pl->sandbox;

  return sb ? sb->pid : -1;
}

int plugin_sandbox_supported(void) { return 1; }

// Switch plugin off for good, with a reason a person can read
static int fail(plugin_loader_t *pl, const char *why) {
  nl_snprintf(pl->status_msg, sizeof(pl->status_msg), "sandbox: %s %s",
              pl->plugin && pl->plugin->name ? pl->plugin->name : "plugin",
              why);
  destroy(pl);
  pl->disabled = 1; // Plugin and its name stay, so app can still list it

  return -1;
}

static int start(plugin_loader_t *pl, size_t need) {
  sb_t *sb = (sb_t *)nl_calloc(1, sizeof(*sb));
  if (!sb) {
    return -1;
  }

  sb->cap = need > SB_MIN_CAP ? need : SB_MIN_CAP;
  sb->map_bytes = SB_HDR_BYTES + ((sb->cap + 4095UL) & ~4095UL);
  long m = __sc6(SB_SYS_MMAP, 0, (long)sb->map_bytes, SB_PROT_RW,
                 SB_MAP_SHARED_ANON, -1, 0);
  if (m < 0 && m > -4096) {
    nl_free(sb);

    return -1;
  }

  sb->ctl = (sb_ctl_t *)m;
  sb->data = (uint8_t *)m + SB_HDR_BYTES;
  sb->handle = pl->dl_handle;
  sb->pid = -1;
  pl->sandbox = sb;
  plugin_set_sandbox_stop(plugin_sandbox_stop);

  long ppid = __sc1(39 /* getpid */, 0);
  long pid = __sc6(SB_SYS_FORK, 0, 0, 0, 0, 0, 0);
  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    plugin_sandbox_reset_signals();
    child_main(sb->ctl, sb->data, pl->plugin, pl->rgb_process, ppid);
  }

  sb->pid = pid;

  return 0;
}

// Wait for child to finish a request
// Return 0 done, -1 child died, -2 too slow
static int wait_done(sb_t *sb, int timeout_ms, int *status) {
  struct timespec t0 = {0};
  struct timespec now = {0};
  clock_gettime(CLOCK_MONOTONIC, &t0);

  for (;;) {
    if (__atomic_load_n(&sb->ctl->state, __ATOMIC_ACQUIRE) == SB_DONE) {
      return 0;
    }

    sb_ts_t slice = {0, 2000000}; // Look at child every 2 ms
    futex(&sb->ctl->state, SB_FUTEX_WAIT, SB_REQ, &slice);
    if (__atomic_load_n(&sb->ctl->state, __ATOMIC_ACQUIRE) == SB_DONE) {
      return 0;
    }

    long r = __sc6(SB_SYS_WAIT4, sb->pid, (long)status, SB_WNOHANG, 0, 0, 0);
    if (r == sb->pid) {
      sb->pid = -1;

      return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - t0.tv_sec) * 1000L +
              (now.tv_nsec - t0.tv_nsec) / 1000000L;
    if (ms >= timeout_ms) {
      return -2;
    }
  }
}

int plugin_run(plugin_loader_t *pl, int rgb, uint8_t *buf, int w, int h,
               int *param) {
  if (!pl->plugin || pl->disabled || (rgb && !pl->rgb_process)) {
    return 0;
  }

  if (!plugin_sandbox_enabled) {
    if (rgb) {
      pl->rgb_process(buf, w, h, param);
    } else {
      pl->plugin->process(buf, w, h, param);
    }

    return 0;
  }

  if (w <= 0 || h <= 0 || !buf || !param) {
    return 0;
  }

  size_t need = (size_t)w * (size_t)h * (rgb ? 3U : 1U);
  if (need > SB_MAX_CAP) {
    return fail(pl, "was given a frame that is too large");
  }

  sb_t *sb = (sb_t *)pl->sandbox;
  if (sb && (sb->handle != pl->dl_handle || need > sb->cap)) {
    destroy(pl); // Other library in slot, or a bigger picture
    sb = NULL;
  }

  if (!sb) {
    if (start(pl, need) < 0) {
      return fail(pl, "could not be started in a sandbox");
    }

    sb = (sb_t *)pl->sandbox;
  }

  nl_memcpy(sb->data, buf, need);
  sb->ctl->kind = rgb ? 1U : 0U;
  sb->ctl->w = w;
  sb->ctl->h = h;
  sb->ctl->param = *param;
  __atomic_store_n(&sb->ctl->state, SB_REQ, __ATOMIC_RELEASE);
  futex(&sb->ctl->state, SB_FUTEX_WAKE, 1, (const sb_ts_t *)0);

  int status = 0;
  int r = wait_done(sb, plugin_sandbox_timeout_ms, &status);
  if (r == -1) {
    char tmp[48];

    return fail(pl, describe(status, tmp, sizeof(tmp)));
  }

  if (r == -2) {
    reap(sb, 1);

    return fail(pl, "did not answer in time");
  }

  // Exactly bytes sent, whatever child did to rest
  nl_memcpy(buf, sb->data, need);
  __atomic_store_n(&sb->ctl->state, SB_IDLE, __ATOMIC_RELEASE);

  return 0;
}

#else // Not Linux x86-64 nolibc: fail closed

void plugin_sandbox_stop(plugin_loader_t *pl) { (void)pl; }

long plugin_sandbox_pid(const plugin_loader_t *pl) {
  (void)pl;

  return -1;
}

int plugin_sandbox_supported(void) { return 0; }

int plugin_run(plugin_loader_t *pl, int rgb, uint8_t *buf, int w, int h,
               int *param) {
  if (!pl->plugin || pl->disabled || (rgb && !pl->rgb_process)) {
    return 0;
  }

  if (!plugin_sandbox_enabled) {
    if (rgb) {
      pl->rgb_process(buf, w, h, param);
    } else {
      pl->plugin->process(buf, w, h, param);
    }

    return 0;
  }

  nl_snprintf(pl->status_msg, sizeof(pl->status_msg),
              "sandbox: not supported on this platform");
  pl->disabled = 1;

  return -1;
}

#endif
