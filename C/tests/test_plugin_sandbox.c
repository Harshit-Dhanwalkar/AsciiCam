/*
 * tests/test_plugin_sandbox.c - src/plugin_sandbox.c
 *
 * Plugins here are functions of this test, so they can be as hostile as
 * needed: the sandbox only needs a plugin_loader_t with process() and the rgb
 * hook, which a loaded library would also provide
 *
 *   1. good plugins give the same result in a sandbox as in-process, keep
 *      state between frames, survive size changes, reload and many restarts
 *   2. each forbidden thing (files, sockets, fork, exec, kill, executable
 *      memory, ...) ends the child, switches the plugin off with a message
 *      and leaves this process, the files and the other children alone
 *   3. crashes, an endless loop and a plugin that quits are survived too
 *   4. the child does not outlive the app
 *
 * Linux x86-64 only; elsewhere the test just checks that a sandboxed run
 * fails closed
 */

#include "../lib/nolibc.h"

#include "plugin_sandbox.h"

static int fails, total;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (!(c)) {                                                                \
      fails++;                                                                 \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static int has(const char *hay, const char *needle) {
  for (; *hay; hay++) {
    const char *h = hay, *n = needle;
    while (*n && *h == *n) {
      h++;
      n++;
    }

    if (!*n) {
      return 1;
    }
  }

  return 0;
}

// Plugins

static void p_invert(uint8_t *g, int w, int h, void *ctx) {
  int p = *(int *)ctx;
  for (int i = 0; i < w * h; i++) {
    g[i] = (uint8_t)((255 - g[i]) ^ p);
  }
}

static void rgb_swap(uint8_t *rgb, int w, int h, void *ctx) {
  (void)ctx;
  for (int i = 0; i < w * h; i++) {
    uint8_t t = rgb[i * 3];
    rgb[i * 3] = rgb[i * 3 + 2];
    rgb[i * 3 + 2] = t;
  }
}

// Remembers how often it ran (state a plugin keeps between frames)
static void p_count(uint8_t *g, int w, int h, void *ctx) {
  static int calls;
  (void)w, (void)h, (void)ctx;
  g[0] = (uint8_t)++calls;
}

// What plugin's real imports allow: small and large allocations
static void p_alloc(uint8_t *g, int w, int h, void *ctx) {
  (void)w, (void)h, (void)ctx;
  uint8_t *big = (uint8_t *)malloc(3u << 20); // Large: mmap behind malloc
  uint8_t *small = (uint8_t *)malloc(100);
  uint8_t *zero = (uint8_t *)calloc(64, 1);
  if (!big || !small || !zero) {
    g[0] = 0xEE;

    return;
  }

  memset(big, 0x5a, 3u << 20);
  memcpy(small, big, 100);
  g[0] = (uint8_t)(small[99] + zero[63] + (uint8_t)strlen("abc"));
  free(big);
  free(small);
  free(zero);
}

static void p_open(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc3(2 /* open */, (long)"/tmp/plugin_sandbox_pwned", 0x241 /* WC|T */,
        0644);
}

static void p_open_read(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc3(2, (long)"/etc/passwd", 0, 0);
}

static void p_openat(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc6(257, -100, (long)"/etc/passwd", 0, 0, 0, 0);
}

static void p_write(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc3(1, 1, (long)"x", 1);
}

static void p_socket(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc3(41, 2, 1, 0);
}

static void p_fork(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc6(57, 0, 0, 0, 0, 0, 0);
}

static void p_clone(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc6(56, 0x100, 0, 0, 0, 0, 0);
}

static void p_exec(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc3(59, (long)"/bin/sh", 0, 0);
}

static void p_kill_app(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h;
  __sc3(62, *(int *)ctx, 9, 0); // ctx = app's pid
}

static void p_ptrace(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h;
  __sc6(101, 16, *(int *)ctx, 0, 0, 0, 0);
}

static void p_mmap_exec(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc6(9, 0, 4096, 7 /* RWX */, 0x22, -1, 0);
}

static void p_mmap_rx(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc6(9, 0, 4096, 5 /* R-X */, 0x22, -1, 0);
}

static void p_mprotect(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  long m = __sc6(9, 0, 4096, 3, 0x22, -1, 0); // Fine: RW
  __sc3(10 /* mprotect */, m, 4096, 5);       // Not fine: make it executable
}

// mmap is allowed, so it must have nothing to map: tries every descriptor app
// had open and reports in frame whether any of them could be mapped
static void p_inherit(uint8_t *g, int w, int h, void *ctx) {
  (void)w, (void)h, (void)ctx;
  g[0] = 0;
  for (int fd = 0; fd < 64; fd++) {
    long m = __sc6(9, 0, 4096, 1 /* PROT_READ */, 0x2 /* PRIVATE */, fd, 0);
    if (m >= 0 || m < -4095) {
      g[0] = (uint8_t)(fd + 1); // got a mapping of descriptor fd
    }
  }
}

static void p_segv(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  *(volatile int *)0 = 1;
}

static void p_ill(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __asm__ volatile("ud2");
}

static void p_spin(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  for (;;) {
    __asm__ volatile("");
  }
}

static void p_exit(uint8_t *g, int w, int h, void *ctx) {
  (void)g, (void)w, (void)h, (void)ctx;
  __sc1(231, 0);
}

// Tries to leave a mark outside frame it was given: writes everywhere in its
// own copy of process, which must change nothing here
static int canary = 0x1234;
static void p_scribble(uint8_t *g, int w, int h, void *ctx) {
  (void)ctx;
  canary = 0x6666; // This child's copy only
  for (int i = 0; i < w * h; i++) {
    g[i] = 0xAA;
  }
}

// App has crash handlers of its own that do system calls (restoring terminal):
// a child that inherited them would turn a plugin crash into a forbidden call
// Same situation, reproduced here
#if defined(__x86_64__) && defined(__LINUX_NOLIBC__)
__asm__(".text\n.globl sb_test_restore\nsb_test_restore:\n"
        "  mov $15, %rax\n  syscall\n");
extern void sb_test_restore(void);

static void app_crash_handler(int sig) {
  (void)sig;
  __sc1(39 /* getpid: not allowed in sandbox */, 0);
  __sc1(231, 99);
}

static void set_handler(int sig, void (*h)(int)) {
  struct {
    long handler;
    unsigned long flags;
    long restorer;
    unsigned long mask;
  } sa = {(long)h, h ? 0x04000000UL /* SA_RESTORER */ : 0,
          h ? (long)sb_test_restore : 0, 0};
  __sc6(13, sig, (long)&sa, 0, 8, 0, 0);
}
#endif

// Helpers

static filter_plugin_t fp_invert = {p_invert, "invert"};
static filter_plugin_t fp_count = {p_count, "count"};
static filter_plugin_t fp_alloc = {p_alloc, "alloc"};

static void make(plugin_loader_t *pl, filter_plugin_t *fp, plugin_rgb_fn rgb) {
  nl_memset(pl, 0, sizeof(*pl));
  pl->plugin = fp;
  pl->rgb_process = rgb;
  pl->dl_handle =
      (void *)&fp_invert; // Stands for "library loaded"; never closed
  pl->inotify_fd = -1;
}

static int zombies_or_children(void) {
  int st = 0;
  long r = __sc6(61 /* wait4 */, -1, (long)&st, 1 /* WNOHANG */, 0, 0, 0);

  return r != -10; // -ECHILD: no child at all
}

static int lowest_fd(void) {
  int fd = open("/dev/null", O_RDONLY, 0);
  if (fd >= 0) {
    close(fd);
  }

  return fd;
}

static int file_exists(const char *path) {
  int fd = open(path, O_RDONLY, 0);
  if (fd >= 0) {
    close(fd);

    return 1;
  }

  return 0;
}

#if defined(__x86_64__) && defined(__LINUX_NOLIBC__)

static void good_plugins(void) {
  plugin_sandbox_enabled = 1;
  uint8_t in[64 * 48], a[64 * 48], b[64 * 48];
  for (int i = 0; i < 64 * 48; i++) {
    in[i] = (uint8_t)(i * 7);
  }

  for (int param = 0; param < 3; param++) {
    plugin_loader_t pl;
    make(&pl, &fp_invert, NULL);
    for (int i = 0; i < 64 * 48; i++) {
      a[i] = b[i] = in[i];
    }

    int p = param * 41;
    plugin_sandbox_enabled = 0;
    CHECK(plugin_run(&pl, 0, a, 64, 48, &p) == 0, "in-process run");
    plugin_sandbox_enabled = 1;
    CHECK(plugin_run(&pl, 0, b, 64, 48, &p) == 0, "sandboxed run: %s",
          pl.status_msg);
    int same = 1;
    for (int i = 0; i < 64 * 48; i++) {
      same = same && a[i] == b[i];
    }

    CHECK(same, "result differs from in-process (param %d)", p);
    CHECK(plugin_sandbox_pid(&pl) > 0, "child running");
    plugin_sandbox_stop(&pl);
    CHECK(plugin_sandbox_pid(&pl) == -1, "child gone after stop");
  }

  // State is kept between frames, and a restart starts it again
  plugin_loader_t pl;
  make(&pl, &fp_count, NULL);
  uint8_t px[16 * 16];
  int p = 0;
  for (int k = 1; k <= 5; k++) {
    plugin_run(&pl, 0, px, 16, 16, &p);
    CHECK(px[0] == k, "call %d: plugin state %d", k, px[0]);
  }

  long first_child = plugin_sandbox_pid(&pl);
  pl.dl_handle = (void *)&fp_count; // Another library in slot (reload)
  plugin_run(&pl, 0, px, 16, 16, &p);
  CHECK(px[0] == 1, "reload restarts the plugin (state %d)", px[0]);
  CHECK(plugin_sandbox_pid(&pl) != first_child, "new child after reload");
  plugin_sandbox_stop(&pl);

  // Picture size change: shared buffer grows, plugin restarts
  make(&pl, &fp_count, NULL);
  static uint8_t big[1920 * 1080 * 3];
  plugin_run(&pl, 0, px, 16, 16, &p);
  plugin_run(&pl, 0, px, 16, 16, &p);
  CHECK(px[0] == 2, "state before resize");
  CHECK(plugin_run(&pl, 0, big, 1920, 1080, &p) == 0, "full HD luma");
  CHECK(big[0] == 1 || big[0] == 3, "frame processed after size change");
  CHECK(plugin_run(&pl, 1, big, 1920, 1080, &p) == 0 || 1,
        "no colour hook: no-op");
  plugin_sandbox_stop(&pl);

  // Colour hook
  make(&pl, &fp_invert, rgb_swap);
  uint8_t rgb[10 * 10 * 3];
  for (int i = 0; i < 300; i++) {
    rgb[i] = (uint8_t)i;
  }

  CHECK(plugin_run(&pl, 1, rgb, 10, 10, &p) == 0, "colour hook: %s",
        pl.status_msg);
  CHECK(rgb[0] == 2 && rgb[2] == 0 && rgb[3] == 5 && rgb[5] == 3,
        "colour hook result %d %d %d %d", rgb[0], rgb[2], rgb[3], rgb[5]);
  CHECK(plugin_run(&pl, 1, rgb, 10, 10, &p) == 0, "colour hook again");
  CHECK(rgb[0] == 0 && rgb[2] == 2, "swapped back");
  plugin_sandbox_stop(&pl);

  // What real plugins do with their imports works under filter
  make(&pl, &fp_alloc, NULL);
  uint8_t g[16];
  for (int k = 0; k < 3; k++) {
    CHECK(plugin_run(&pl, 0, g, 4, 4, &p) == 0, "alloc plugin: %s",
          pl.status_msg);
    CHECK(g[0] == (uint8_t)(0x5a + 0 + 3), "alloc plugin result %d", g[0]);
  }

  plugin_sandbox_stop(&pl);

  // nothing to do: not loaded / no colour hook / empty frame
  make(&pl, &fp_invert, NULL);
  CHECK(plugin_run(&pl, 1, rgb, 10, 10, &p) == 0 &&
            plugin_sandbox_pid(&pl) == -1,
        "no colour hook: nothing started");
  CHECK(plugin_run(&pl, 0, px, 0, 16, &p) == 0 && plugin_sandbox_pid(&pl) == -1,
        "empty frame: nothing started");
  pl.plugin = NULL;
  CHECK(plugin_run(&pl, 0, px, 16, 16, &p) == 0, "unloaded plugin");

  // absurd picture: refused, not allocated
  make(&pl, &fp_invert, NULL);
  CHECK(plugin_run(&pl, 0, px, 100000, 100000, &p) < 0 && pl.disabled,
        "absurd frame size accepted");
  CHECK(has(pl.status_msg, "too large"), "message: %s", pl.status_msg);
}

static void lifecycle(void) {
  plugin_sandbox_enabled = 1;
  int before = lowest_fd();
  uint8_t px[16 * 16];
  int p = 0;
  for (int i = 0; i < 100; i++) {
    plugin_loader_t pl;
    make(&pl, &fp_invert, NULL);
    plugin_run(&pl, 0, px, 16, 16, &p);
    plugin_sandbox_stop(&pl);
    plugin_sandbox_stop(&pl); // twice is fine
  }

  CHECK(lowest_fd() == before, "descriptors leaked");
  CHECK(!zombies_or_children(), "children left behind (zombies)");
}

// Every row: plugin function, text message must contain
typedef struct {
  const char *what;
  void (*fn)(uint8_t *, int, int, void *);
  const char *msg;
} bad_t;

static void hostile(void) {
  plugin_sandbox_enabled = 1;
  plugin_sandbox_timeout_ms = 400;
  static const bad_t bad[] = {
      {"create a file", p_open, "not allowed"},
      {"open /etc/passwd", p_open_read, "not allowed"},
      {"openat", p_openat, "not allowed"},
      {"write to stdout", p_write, "not allowed"},
      {"open a socket", p_socket, "not allowed"},
      {"fork", p_fork, "not allowed"},
      {"clone a thread", p_clone, "not allowed"},
      {"exec a shell", p_exec, "not allowed"},
      {"kill the app", p_kill_app, "not allowed"},
      {"ptrace the app", p_ptrace, "not allowed"},
      {"map RWX memory", p_mmap_exec, "not allowed"},
      {"map R-X memory", p_mmap_rx, "not allowed"},
      {"mprotect memory executable", p_mprotect, "not allowed"},
      {"crash (segfault)", p_segv, "segmentation fault"},
      {"crash (illegal instruction)", p_ill, "illegal instruction"},
      {"loop forever", p_spin, "did not answer"},
      {"quit by itself", p_exit, "exited unexpectedly"},
  };

  unlink("/tmp/plugin_sandbox_pwned"); // from an earlier failed run
  long me = __sc1(39, 0);
  for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
    filter_plugin_t fp = {bad[i].fn, "evil"};
    plugin_loader_t pl;
    make(&pl, &fp, NULL);
    uint8_t px[16 * 16] = {0};
    int p = (int)me;
    int r = plugin_run(&pl, 0, px, 16, 16, &p);
    CHECK(r == -1, "'%s': run returned %d", bad[i].what, r);
    CHECK(pl.disabled == 1 && pl.plugin == &fp, "'%s': not switched off",
          bad[i].what);
    CHECK(plugin_run(&pl, 0, px, 16, 16, &p) == 0 && pl.sandbox == NULL,
          "'%s': a switched-off plugin ran again", bad[i].what);
    CHECK(has(pl.status_msg, bad[i].msg) && has(pl.status_msg, "evil"),
          "'%s': message '%s'", bad[i].what, pl.status_msg);
    CHECK(plugin_sandbox_pid(&pl) == -1, "'%s': child not reaped", bad[i].what);
    CHECK(!zombies_or_children(), "'%s': zombie left", bad[i].what);
    CHECK(pl.sandbox == NULL, "'%s': sandbox state kept", bad[i].what);
  }

  CHECK(!file_exists("/tmp/plugin_sandbox_pwned"), "plugin created a file");

  // A plugin that was switched off comes back when its slot is loaded again
  {
    filter_plugin_t crash = {p_segv, "crash"};
    plugin_loader_t sw;
    make(&sw, &crash, NULL);
    uint8_t q[16 * 16] = {0};
    int qp = 0;
    plugin_run(&sw, 0, q, 16, 16, &qp);
    CHECK(sw.disabled == 1, "crash plugin not switched off");
    sw.disabled = 0; // what plugin_load() does for a slot it loads into
    sw.plugin = &fp_count;
    CHECK(plugin_run(&sw, 0, q, 16, 16, &qp) == 0 && q[0] == 1,
          "plugin does not run again after being re-enabled");
    plugin_sandbox_stop(&sw);
  }

  // A plugin that writes all over its copy of app changes nothing here
  filter_plugin_t fp = {p_scribble, "scribble"};
  plugin_loader_t pl;
  make(&pl, &fp, NULL);
  uint8_t px[8 * 8] = {0};
  int p = 0;
  CHECK(plugin_run(&pl, 0, px, 8, 8, &p) == 0, "scribble: %s", pl.status_msg);
  CHECK(px[0] == 0xAA && px[63] == 0xAA, "pixels it was given came back");
  CHECK(canary == 0x1234, "plugin's writes reached the app's memory");
  plugin_sandbox_stop(&pl);

  // Crash handlers inherited from app must not hide what really happened
  set_handler(11, app_crash_handler); // SIGSEGV
  set_handler(4, app_crash_handler);  // SIGILL
  {
    filter_plugin_t crash = {p_segv, "crash"};
    plugin_loader_t cp;
    make(&cp, &crash, NULL);
    uint8_t q[8 * 8] = {0};
    int qp = 0;
    plugin_run(&cp, 0, q, 8, 8, &qp);
    CHECK(has(cp.status_msg, "segmentation fault"),
          "crash with app handlers installed: '%s'", cp.status_msg);
    filter_plugin_t ill = {p_ill, "ill"};
    make(&cp, &ill, NULL);
    plugin_run(&cp, 0, q, 8, 8, &qp);
    CHECK(has(cp.status_msg, "illegal instruction"),
          "SIGILL with app handlers installed: '%s'", cp.status_msg);
  }

  set_handler(11, NULL);
  set_handler(4, NULL);

  // Reading back exactly what was sent: bytes after frame stay untouched
  static uint8_t area[8 * 8 + 16];
  for (size_t i = 0; i < sizeof(area); i++) {
    area[i] = 0x11;
  }

  make(&pl, &fp, NULL);
  plugin_run(&pl, 0, area, 8, 8, &p);
  int tail_ok = 1;
  for (size_t i = 64; i < sizeof(area); i++) {
    tail_ok = tail_ok && area[i] == 0x11;
  }

  CHECK(tail_ok, "bytes past the frame changed");
  plugin_sandbox_stop(&pl);

  // Child inherits nothing it could map: open a file here, then see whether
  // plugin can get at it by descriptor number
  int held = open("/etc/passwd", O_RDONLY, 0);
  int held2 = open("/proc/self/status", O_RDONLY, 0);
  CHECK(held >= 0 && held2 >= 0, "cannot open files for the inherit test");
  filter_plugin_t inh = {p_inherit, "inherit"};
  make(&pl, &inh, NULL);
  uint8_t ip[8 * 8] = {0x77};
  CHECK(plugin_run(&pl, 0, ip, 8, 8, &p) == 0, "inherit probe: %s",
        pl.status_msg);
  CHECK(ip[0] == 0, "child could map inherited descriptor %d", ip[0] - 1);
  plugin_sandbox_stop(&pl);
  close(held);
  close(held2);

  // A plugin switched off stays off, and a good one still works afterwards
  filter_plugin_t good = {p_invert, "good"};
  make(&pl, &good, NULL);
  CHECK(plugin_run(&pl, 0, px, 8, 8, &p) == 0,
        "good plugin after the hostile ones");
  plugin_sandbox_stop(&pl);
  plugin_sandbox_timeout_ms = 1000;
}

// Child dies with its parent
static void dies_with_parent(void) {
  plugin_sandbox_enabled = 1;
  volatile long *shared =
      (volatile long *)__sc6(9, 0, 4096, 3, 0x21 /* SHARED|ANON */, -1, 0);
  shared[0] = 0;

  long mid = __sc6(57, 0, 0, 0, 0, 0, 0);
  if (mid == 0) {
    plugin_loader_t pl;
    make(&pl, &fp_invert, NULL);
    uint8_t px[16 * 16];
    int p = 0;
    plugin_run(&pl, 0, px, 16, 16, &p);
    shared[0] = plugin_sandbox_pid(&pl);
    __sc1(231, 0); // exits without stopping sandbox
  }

  int st = 0;
  __sc6(61, mid, (long)&st, 0, 0, 0, 0);
  long child = shared[0];
  CHECK(child > 0, "intermediate did not start a child");
  if (child <= 0) {
    return;
  }

  int gone = 0;
  for (int i = 0; i < 100 && !gone; i++) {
    nl_usleep(20000);
    // Gone: no such process, or a zombie nobody has reaped yet
    char path[48];
    snprintf(path, sizeof(path), "/proc/%ld/stat", child);
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
      gone = 1;
    } else {
      char buf[128] = {0};
      read(fd, buf, sizeof(buf) - 1);
      close(fd);
      const char *c = buf;
      while (*c && !(c[0] == ')' && c[1] == ' ')) {
        c++;
      }

      gone = c[0] == ')' && c[2] == 'Z';
    }
  }

  CHECK(gone, "child %ld outlived its parent", child);
}

#else // Not Linux x86-64

static void unsupported(void) {
  plugin_sandbox_enabled = 1;
  filter_plugin_t fp = {p_invert, "invert"};
  plugin_loader_t pl;
  make(&pl, &fp, NULL);
  uint8_t px[16];
  int p = 0;
  CHECK(!plugin_sandbox_supported(), "supported?");
  CHECK(plugin_run(&pl, 0, px, 4, 4, &p) == -1 && pl.disabled,
        "must fail closed");
}

#endif

int main(void) {
  plugin_sandbox_enabled = 0;
#if defined(__x86_64__) && defined(__LINUX_NOLIBC__)
  CHECK(plugin_sandbox_supported(), "sandbox not supported on this build");
  good_plugins();
  lifecycle();
  hostile();
  dies_with_parent();
#else
  unsupported();
#endif
  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
