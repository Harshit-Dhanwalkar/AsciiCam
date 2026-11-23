/*
 * tests/e2e.c - end-to-end checks of main loop
 *
 *   cc -O1 -o build/tests/e2e tests/e2e.c
 *   build/tests/e2e build/tests/asciicam_fake [build/asciicam [section ...]]
 *
 * Runs program in a pseudo-terminal against software camera
 * (tests/fake_capture.c) and checks what it draws
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mini_zlib.h"
#include "pty_harness.h"
#include "screen.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>

static const char *BIN; // Program with software camera
static int failures;

static void rm_rf(const char *path);

static void check(int cond, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("%s", cond ? "ok   " : "FAIL ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
  fflush(stdout);
  if (!cond) {
    failures++;
  }
}

/* Argument lists */

typedef struct {
  const char *v[48];
  int n;
} args_t;

// A(NULL-terminated strings)
static args_t A(const char *first, ...) {
  args_t a;
  memset(&a, 0, sizeof a);
  va_list ap;
  va_start(ap, first);
  for (const char *s = first; s && a.n < 46; s = va_arg(ap, const char *)) {
    a.v[a.n++] = s;
  }

  va_end(ap);

  return a;
}

// a + one more argument when extra is not NULL
static args_t plus(args_t a, const char *extra) {
  if (extra && a.n < 46) {
    a.v[a.n++] = extra;
  }

  return a;
}

static args_t cat(args_t a, args_t b) {
  for (int i = 0; i < b.n && a.n < 46; i++) {
    a.v[a.n++] = b.v[i];
  }

  return a;
}

/* Output helpers */

static int starts_with_str(const char *s, size_t n, const char *prefix) {
  size_t l = strlen(prefix);

  return n >= l && !memcmp(s, prefix, l);
}

static int contains(const unsigned char *p, size_t n, const char *needle) {
  return memmem(p, n, needle, strlen(needle)) != NULL;
}

// Last "frame: WxH" program printed
static int last_frame(const unsigned char *b, size_t n, int *w, int *h) {
  int found = 0;
  for (size_t i = 0; i + 7 < n; i++) {
    if (memcmp(b + i, "frame: ", 7)) {
      continue;
    }

    size_t j = i + 7;
    int a = 0, nd = 0;
    while (j < n && b[j] >= '0' && b[j] <= '9') {
      a = a * 10 + (b[j++] - '0');
      nd++;
    }

    if (!nd || j >= n || b[j] != 'x') {
      continue;
    }

    j++;
    int c = 0, md = 0;
    while (j < n && b[j] >= '0' && b[j] <= '9') {
      c = c * 10 + (b[j++] - '0');
      md++;
    }

    if (md) {
      *w = a;
      *h = c;
      found = 1;
    }
  }

  return found;
}

// Frames are drawn starting with a cursor move to their top-left cell:
// ESC[?2026h ESC[ y;x H (or just H for 1,1)
// Last one wins
static int last_origin(const unsigned char *b, size_t n, int *x, int *y) {
  static const char pre[] = "\x1b[?2026h\x1b[";
  size_t pl = sizeof pre - 1;
  int found = 0;
  for (size_t i = 0; i + pl < n; i++) {
    if (memcmp(b + i, pre, pl)) {
      continue;
    }

    size_t j = i + pl;
    if (j < n && b[j] == 'H') {
      *x = 1;
      *y = 1;
      found = 1;

      continue;
    }

    int r = 0, rd = 0, c = 0, cd = 0;
    while (j < n && b[j] >= '0' && b[j] <= '9') {
      r = r * 10 + (b[j++] - '0');
      rd++;
    }

    if (!rd || j >= n || b[j] != ';') {
      continue;
    }

    j++;
    while (j < n && b[j] >= '0' && b[j] <= '9') {
      c = c * 10 + (b[j++] - '0');
      cd++;
    }

    if (cd && j < n && b[j] == 'H') {
      *y = r;
      *x = c;
      found = 1;
    }
  }

  return found;
}

// "(w, h)" or "None", for messages
static const char *pair_str(int ok, int a, int b) {
  static char ring[4][32];
  static int k;
  char *s = ring[k++ & 3];
  if (ok) {
    snprintf(s, 32, "(%d, %d)", a, b);
  } else {
    snprintf(s, 32, "None");
  }

  return s;
}

static int frame_is(const unsigned char *b, size_t n, int w, int h) {
  int fw;
  int fh;

  return last_frame(b, n, &fw, &fh) && fw == w && fh == h;
}

static const char *frame_str(const unsigned char *b, size_t n) {
  int fw = 0;
  int fh = 0;
  int ok = last_frame(b, n, &fw, &fh);

  return pair_str(ok, fw, fh);
}

static int origin_is(const unsigned char *b, size_t n, int x, int y) {
  int ox;
  int oy;

  return last_origin(b, n, &ox, &oy) && ox == x && oy == y;
}

static const char *origin_str(const unsigned char *b, size_t n) {
  int ox = 0;
  int oy = 0;
  int ok = last_origin(b, n, &ox, &oy);

  return pair_str(ok, ox, oy);
}

/* run(): whole program lifetime in a terminal of 50 rows x 120 columns */

typedef struct {
  long frames;
  int code;
  pty_buf out;
  double latency; // Seconds between quit key and exit; <0: no key sent
} run_t;

// Run for `seconds`, then send `key` (or after quit_after when >= 0)
// Program is killed if it has not exited 6 s after deadline
static run_t run(args_t a, double seconds, double quit_after, const char *key) {
  run_t r;
  memset(&r, 0, sizeof r);
  r.latency = -1;
  char *argv[50];
  argv[0] = (char *)BIN;
  for (int i = 0; i < a.n; i++) {
    argv[i + 1] = (char *)a.v[i];
  }

  argv[a.n + 1] = NULL;

  pty_proc p;
  if (pty_spawn(&p, BIN, argv, 50, 120) != 0) {
    fprintf(stderr, "cannot start %s in a pty\n", BIN);
    exit(1);
  }

  double start = pty_now(), sent_at = -1;
  double deadline = start + seconds + 6;
  int code = -1;
  int done = 0;
  while (pty_now() < deadline) {
    if (pty_read(&p, &r.out, 20) < 0) {
      break;
    }

    double now = pty_now();
    if (sent_at < 0 &&
        now - start >= (quit_after >= 0 ? quit_after : seconds)) {
      pty_write(&p, key, strlen(key));
      sent_at = now;
    }

    if (pty_exited(&p, &code)) {
      done = 1;

      break;
    }
  }

  if (!done) {
    // pty EOF arrives as child exits, often before waitpid sees it
    for (int i = 0; i < 100 && !done; i++) {
      if (pty_exited(&p, &code)) {
        done = 1;
      } else {
        usleep(20000);
      }
    }
  }

  r.code = done ? code : pty_finish(&p, 0.0); // kills it
  if (sent_at >= 0) {
    r.latency = pty_now() - sent_at;
  }

  if (p.fd >= 0) {
    close(p.fd);
  }

  r.frames = (long)pty_buf_count(&r.out, "\x1b[?2026h");

  return r;
}

/* session(): send keys and mouse reports to a live program */

typedef struct {
  unsigned char d[48];
  size_t n;
  double wait;
} step_t;

static step_t S(const char *s, double wait) {
  step_t t;
  memset(&t, 0, sizeof t);
  t.n = strlen(s);
  memcpy(t.d, s, t.n);
  t.wait = wait;

  return t;
}

// SGR mouse report: button b at column x, row y; release uses 'm'
static step_t M(int b, int x, int y, int release, double wait) {
  step_t t;
  memset(&t, 0, sizeof t);
  t.n = (size_t)snprintf((char *)t.d, sizeof t.d, "\x1b[<%d;%d;%d%c", b, x, y,
                         release ? 'm' : 'M');
  t.wait = wait;

  return t;
}

typedef struct {
  pty_buf out;
  size_t marks[64]; // output length at each step boundary
  int nmarks;
} sess_t;

static void pump(pty_proc *p, pty_buf *out, double t) {
  double end = pty_now() + t;
  while (pty_now() < end) {
    if (pty_read(p, out, 20) < 0) {
      return;
    }
  }
}

// Steps are (bytes, seconds to wait after sending); a final "q" is appended
// marks[0] is after `settle`, marks[i] after step i - 1 and its wait
static sess_t session(args_t a, const step_t *steps, int nsteps,
                      double settle) {
  sess_t s;
  memset(&s, 0, sizeof s);
  char *argv[50];
  argv[0] = (char *)BIN;
  for (int i = 0; i < a.n; i++) {
    argv[i + 1] = (char *)a.v[i];
  }

  argv[a.n + 1] = NULL;

  pty_proc p;
  if (pty_spawn(&p, BIN, argv, 50, 120) != 0) {
    fprintf(stderr, "cannot start %s in a pty\n", BIN);
    exit(1);
  }

  pump(&p, &s.out, settle);
  s.marks[s.nmarks++] = s.out.len;
  for (int i = 0; i <= nsteps; i++) {
    step_t st = i < nsteps ? steps[i] : S("q", 0.4);
    pty_write(&p, st.d, st.n);
    pump(&p, &s.out, st.wait);
    if (s.nmarks < 64) {
      s.marks[s.nmarks++] = s.out.len;
    }
  }

  for (int i = 0; i < 100; i++) {
    if (pty_exited(&p, NULL)) {
      break;
    }

    usleep(20000);
  }

  pty_finish(&p, 0.0); // kills it if it is still there

  return s;
}

// One byte at a time, 4 ms apart (a slow link, tmux or ssh)
static int trickle(step_t *dst, int at, step_t whole) {
  for (size_t i = 0; i < whole.n; i++) {
    char one[2] = {(char)whole.d[i], 0};
    dst[at++] = S(one, 0.004);
  }

  return at;
}

/* Checks */

static void smoke(void) {
  static const char *labels[] = {"threaded", "inline -T"};
  static const char *extras[] = {NULL, "-T"};

  for (int v = 0; v < 2; v++) {
    const char *label = labels[v];
    const char *extra = extras[v];

    run_t r = run(plus(A("-d", "fake", "-r", "20", NULL), extra), 2.0, -1, "q");
    check(r.frames >= 25, "%s: frames drawn in 2 s (%ld)", label, r.frames);
    check(r.code == 0, "%s: 'q' exits 0 (got %d)", label, r.code);
    check(contains(r.out.data, r.out.len, "\x1b[?25h"),
          "%s: cursor restored on exit", label);
    free(r.out.data);

    r = run(plus(A("-d", "fake:pad", "-r", "20", "-C", "-w", "320", "-h", "240",
                   NULL),
                 extra),
            1.5, -1, "q");
    check(r.frames >= 15 && r.code == 0,
          "%s: padded rows + colour (%ld frames, exit %d)", label, r.frames,
          r.code);
    check(contains(r.out.data, r.out.len, "\x1b[38;2;"),
          "%s: truecolor escapes emitted", label);
    free(r.out.data);
  }
}

// Wedged camera: capture thread keeps UI responsive, inline capture does not
static void stalled_camera(void) {
  static const char *labels[] = {"threaded", "inline -T"};
  static const char *extras[] = {NULL, "-T"};
  double lat[2] = {-1, -1};

  for (int v = 0; v < 2; v++) {
    run_t r = run(plus(A("-d", "fake:stall=10", "-r", "20", NULL), extras[v]),
                  3.0, 1.5, "q");
    lat[v] = r.latency;
    check(r.code == 0, "%s: stalled camera, 'q' still exits 0 (got %d)",
          labels[v], r.code);
    if (r.latency >= 0) {
      printf("     %s: quit latency with a stalled camera = %.2fs\n", labels[v],
             r.latency);
    }

    free(r.out.data);
  }

  check(lat[0] >= 0 && lat[0] < 0.6,
        "threaded: quits promptly with a stalled camera (%.3f)", lat[0]);
}

// Mouse: move and resize frame in a live session
static void mouse_tests(void) {
  static const char *labels[] = {"threaded", "inline -T"};
  static const char *extras[] = {NULL, "-T"};
  const args_t ARGS = A("-d", "fake", "-W", "40", "-H", "20", "-r", "20", NULL);

  for (int v = 0; v < 2; v++) {
    const char *label = labels[v];
    const char *extra = extras[v];
    sess_t s;
    size_t *m;
    const unsigned char *after;
    size_t an;

    // Move frame by dragging its body
    step_t move[] = {M(0, 10, 5, 0, 0.15), M(32, 30, 15, 0, 0.3),
                     M(0, 30, 15, 1, 0.5)};
    s = session(plus(ARGS, extra), move, 3, 0.6);
    m = s.marks;
    const unsigned char *during = s.out.data + m[1];
    size_t dn = m[3] - m[1];
    check(contains(during, dn, "MOVING frame (1,1) -> (21,11)"),
          "%s: move shows pending position", label);
    check(contains(during, dn, "\x1b[11;21H+-"),
          "%s: outline drawn at pending position", label);
    after = s.out.data + m[s.nmarks - 3];
    an = s.out.len - m[s.nmarks - 3];
    check(origin_is(after, an, 21, 11), "%s: frame redrawn at (21,11), got %s",
          label, origin_str(after, an));
    check(frame_is(after, an, 40, 20), "%s: size unchanged by a move (%s)",
          label, frame_str(after, an));
    free(s.out.data);

    // Then resize from bottom-right corner (60,30)
    step_t br[] = {M(0, 10, 5, 0, 0.15),   M(32, 30, 15, 0, 0.2),
                   M(0, 30, 15, 1, 0.4),   M(0, 60, 30, 0, 0.15),
                   M(32, 70, 35, 0, 0.25), M(0, 70, 35, 1, 0.5)};
    s = session(plus(ARGS, extra), br, 6, 0.6);
    m = s.marks;
    after = s.out.data + m[s.nmarks - 3];
    an = s.out.len - m[s.nmarks - 3];
    check(frame_is(after, an, 50, 25), "%s: BR corner resize -> 50x25 (%s)",
          label, frame_str(after, an));
    check(origin_is(after, an, 21, 11), "%s: BR resize keeps top-left (%s)",
          label, origin_str(after, an));
    free(s.out.data);

    // ... and from top-left corner (21,11)
    step_t tl[] = {M(0, 10, 5, 0, 0.15),  M(32, 30, 15, 0, 0.2),
                   M(0, 30, 15, 1, 0.4),  M(0, 21, 11, 0, 0.15),
                   M(32, 16, 8, 0, 0.25), M(0, 16, 8, 1, 0.5)};
    s = session(plus(ARGS, extra), tl, 6, 0.6);
    m = s.marks;
    after = s.out.data + m[s.nmarks - 3];
    an = s.out.len - m[s.nmarks - 3];
    check(frame_is(after, an, 45, 23), "%s: TL corner resize -> 45x23 (%s)",
          label, frame_str(after, an));
    check(origin_is(after, an, 16, 8),
          "%s: TL resize keeps bottom-right fixed (%s)", label,
          origin_str(after, an));
    free(s.out.data);

    // Colour at full camera size: RGB copy and colour output buffer used to
    // fill allocator arena, so every resize silently failed
    step_t col[] = {M(0, 100, 40, 0, 0.15), M(32, 90, 35, 0, 0.25),
                    M(0, 90, 35, 1, 0.5)};
    s = session(plus(A("-d", "fake", "-w", "640", "-h", "480", "-C", "-W",
                       "100", "-H", "40", "-r", "20", NULL),
                     extra),
                col, 3, 0.6);
    m = s.marks;
    after = s.out.data + m[s.nmarks - 3];
    an = s.out.len - m[s.nmarks - 3];
    check(frame_is(after, an, 90, 35),
          "%s: colour 640x480 BR resize -> 90x35 (%s)", label,
          frame_str(after, an));
    free(s.out.data);

    // A click without dragging changes nothing; 'a' goes back to top-left
    step_t click[] = {M(0, 10, 5, 0, 0.15), M(0, 10, 5, 1, 0.3),
                      M(0, 10, 5, 0, 0.15), M(32, 30, 15, 0, 0.2),
                      M(0, 30, 15, 1, 0.4), S("a", 0.6)};
    s = session(plus(ARGS, extra), click, 6, 0.6);
    m = s.marks;
    check(origin_is(s.out.data + m[0], m[2] - m[0], 1, 1),
          "%s: plain click does not move frame", label);
    const unsigned char *seg = s.out.data + m[s.nmarks - 3];
    size_t sn = m[s.nmarks - 2] - m[s.nmarks - 3];
    check(origin_is(seg, sn, 1, 1), "%s: 'a' returns frame to top-left", label);
    check(frame_is(seg, sn, 120, 46), "%s: 'a' fits terminal (%s)", label,
          frame_str(seg, sn));
    free(s.out.data);

    // A terminal may deliver one mouse report in pieces (slow link, tmux, ssh):
    // same drag byte by byte must resize just same
    step_t tr[160];
    int nt = 0;
    nt = trickle(tr, nt, M(0, 40, 20, 0, 0));
    tr[nt++] = S("", 0.15);
    nt = trickle(tr, nt, M(32, 30, 15, 0, 0));
    tr[nt++] = S("", 0.2);
    nt = trickle(tr, nt, M(0, 30, 15, 1, 0));
    tr[nt++] = S("", 0.5);
    s = session(plus(ARGS, extra), tr, nt, 0.6);
    check(frame_is(s.out.data, s.out.len, 30, 15),
          "%s: drag delivered byte by byte still resizes (%s)", label,
          frame_str(s.out.data, s.out.len));
    check(contains(s.out.data, s.out.len, "frame: 30x15"),
          "%s: no hotkeys triggered by a split report", label);
    free(s.out.data);
  }
}

/* Real binary: built-in demo picture, plugins, --net, --vcam, --play */

// Last position of needle in buffer, or -1
static long rfind_mem(const unsigned char *p, size_t n, const char *needle) {
  size_t l = strlen(needle);
  for (size_t i = n; l && i >= l; i--) {
    if (!memcmp(p + i - l, needle, l)) {
      return (long)(i - l);
    }
  }

  return -1;
}

// Check for picture change: Distinct 200-byte runs that follow a frame start
// (first part of each frame: cursor move and top rows)
static int distinct_frame_heads(const pty_buf *b) {
  static const char mark[] = "\x1b[?2026h";
  size_t ml = sizeof mark - 1;
  const unsigned char *heads[512];
  int nh = 0, distinct = 0;
  for (size_t i = 0; i + ml + 200 <= b->len;) {
    if (memcmp(b->data + i, mark, ml)) {
      i++;

      continue;
    }

    const unsigned char *h = b->data + i + ml;
    int seen = 0;
    for (int k = 0; k < nh && !seen; k++) {
      seen = !memcmp(heads[k], h, 200);
    }

    if (!seen) {
      distinct++;
      if (nh < 512) {
        heads[nh++] = h;
      }
    }

    i += ml + 200; // A regex findall would continue after what it matched
  }

  return distinct;
}

// Mean (R - B) over foreground colour escapes ESC[38;2;R;G;Bm
// Returns 0 when there are none
static int warmth(const pty_buf *b, double *out) {
  static const char pre[] = "\x1b[38;2;";
  size_t pl = sizeof pre - 1;
  double sum = 0;
  long count = 0;
  for (size_t i = 0; i + pl < b->len; i++) {
    if (memcmp(b->data + i, pre, pl)) {
      continue;
    }

    size_t j = i + pl;
    int v[3] = {0, 0, 0};
    int ok = 1;
    for (int c = 0; c < 3 && ok; c++) {
      int nd = 0;
      while (j < b->len && b->data[j] >= '0' && b->data[j] <= '9') {
        v[c] = v[c] * 10 + (b->data[j++] - '0');
        nd++;
      }

      ok = nd > 0 && j < b->len && b->data[j] == (c < 2 ? ';' : 'm');
      j++;
    }

    if (ok) {
      sum += v[0] - v[2];
      count++;
    }
  }

  if (!count) {
    return 0;
  }

  *out = sum / (double)count;

  return 1;
}

static const char *num_str(int ok, double v) {
  static char ring[4][24];
  static int k;
  char *s = ring[k++ & 3];
  if (ok) {
    snprintf(s, 24, "%.0f", v);
  } else {
    snprintf(s, 24, "None");
  }

  return s;
}

static void real_demo_and_plugins(const char *real_abs) {
  static const char *flags[2][2] = {{"--demo", NULL}, {"-d", "demo"}};
  for (int f = 0; f < 2; f++) {
    char label[32];
    snprintf(label, sizeof label, "%s%s%s", flags[f][0], flags[f][1] ? " " : "",
             flags[f][1] ? flags[f][1] : "");
    run_t r =
        run(cat(A(flags[f][0], flags[f][1], NULL), A("-r", "20", "-C", NULL)),
            1.5, -1, "q");
    check(r.frames >= 15 && r.code == 0,
          "real binary %s: picture drawn (%ld frames, exit %d)", label,
          r.frames, r.code);
    check(contains(r.out.data, r.out.len, "\x1b[38;2;"),
          "real binary %s: colour", label);
    free(r.out.data);
  }

  run_t r = run(A("--demo", "-r", "20", NULL), 1.0, -1, "q");
  check(distinct_frame_heads(&r.out) > 3,
        "real binary --demo: picture animates");
  free(r.out.data);

  // Every effect plugin loads through real loader and keeps picture moving
  // (plugin lookups are relative to binary: build/<name>.so)
  static const char *plugins[] = {
      "chromatic", "crt",        "dither_bayer", "dither_fs",   "fisheye",
      "framediff", "glitch",     "kaleidoscope", "matrix_rain", "motion",
      "neon_edge", "pixel_sort", "ripple",       "sepia",       "thermal",
      "trails",    "tv_static",  "vhs",          "vignette"};
  char dir[4096];
  snprintf(dir, sizeof dir, "%s", real_abs);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
  }

  for (size_t i = 0; i < sizeof plugins / sizeof plugins[0]; i++) {
    char so[4200];
    snprintf(so, sizeof so, "%s/%s.so", dir, plugins[i]);
    struct stat st;
    if (stat(so, &st) != 0) {
      check(0, "plugin %s: %s was built", plugins[i], so);

      continue;
    }

    r = run(A("--demo", "-r", "20", "-p", so, NULL), 1.0, -1, "q");
    check(r.frames >= 8 && r.code == 0 &&
              !contains(r.out.data, r.out.len, "Failed to load"),
          "plugin %s: loads and draws (%ld frames, exit %d)", plugins[i],
          r.frames, r.code);
    free(r.out.data);
  }

  // Colour hooks: in colour mode (-C) plugins may repaint picture
  r = run(A("--demo", "-r", "20", "-C", NULL), 1.2, -1, "q");
  double w0 = 0;
  int have0 = warmth(&r.out, &w0);
  free(r.out.data);
  static const char *hooks[] = {"sepia", "thermal"};
  for (int i = 0; i < 2; i++) {
    char so[4200];
    snprintf(so, sizeof so, "%s/%s.so", dir, hooks[i]);
    r = run(A("--demo", "-r", "20", "-C", "-p", so, NULL), 1.2, -1, "q");
    double w = 0;
    int have = warmth(&r.out, &w);
    int pass = have && have0 &&
               (i == 0 ? w > w0 + 15 : (w > w0 ? w - w0 : w0 - w) > 15);
    check(r.frames >= 8 && r.code == 0 && pass,
          "plugin %s repaints colour pictures (warmth %s -> %s, %ld frames)",
          hooks[i], num_str(have0, w0), num_str(have, w), r.frames);
    free(r.out.data);
  }

  // --sandbox: plugins run in a locked-down child, picture and colour hooks
  // still work and several plugins can be stacked
  static const char *sb[] = {"glitch", "vhs", "ripple"};
  for (int i = 0; i < 3; i++) {
    char so[4200];
    snprintf(so, sizeof so, "%s/%s.so", dir, sb[i]);
    r = run(A("--demo", "-r", "20", "--sandbox", "-p", so, NULL), 1.2, -1, "q");
    check(r.frames >= 8 && r.code == 0 &&
              !contains(r.out.data, r.out.len, "switched off"),
          "--sandbox plugin %s: loads and draws (%ld frames, exit %d)", sb[i],
          r.frames, r.code);
    free(r.out.data);
  }
  for (int i = 0; i < 2; i++) {
    char so[4200];
    snprintf(so, sizeof so, "%s/%s.so", dir, hooks[i]);
    r = run(A("--demo", "-r", "20", "-C", "--sandbox", "-p", so, NULL), 1.5, -1,
            "q");
    double w = 0;
    int have = warmth(&r.out, &w);
    int pass = have && have0 &&
               (i == 0 ? w > w0 + 15 : (w > w0 ? w - w0 : w0 - w) > 15);
    check(r.frames >= 8 && r.code == 0 && pass,
          "--sandbox plugin %s repaints colour pictures (warmth %s -> %s)",
          hooks[i], num_str(have0, w0), num_str(have, w));
    free(r.out.data);
  }
  {
    char so1[4200];
    char so2[4200];
    snprintf(so1, sizeof so1, "%s/glitch.so", dir);
    snprintf(so2, sizeof so2, "%s/vignette.so", dir);
    r = run(A("--demo", "-r", "20", "--sandbox", "-p", so1, "-p", so2, NULL),
            1.2, -1, "q");
    check(r.frames >= 8 && r.code == 0,
          "--sandbox with two plugins stacked (%ld frames, exit %d)", r.frames,
          r.code);
    free(r.out.data);
  }

  // Mirror + colour keeps drawing (colour planes are flipped with luma)
  r = run(A("--demo", "-r", "20", "-C", "-f", NULL), 1.2, -1, "q");
  check(r.frames >= 8 && r.code == 0 &&
            contains(r.out.data, r.out.len, "\x1b[38;2;"),
        "--demo -C -f: mirrored colour picture drawn (%ld frames, exit %d)",
        r.frames, r.code);
  free(r.out.data);
}

static void real_net_and_vcam(void) {
  // Slow-link mode: draws, and a bad value is refused up front
  static const char *levels[] = {"0", "300"};
  for (int i = 0; i < 2; i++) {
    run_t r =
        run(A("--demo", "-r", "20", "--net", levels[i], NULL), 1.0, -1, "q");
    check(r.frames >= 8 && r.code == 0,
          "--net %s: draws and exits cleanly (%ld frames, exit %d)", levels[i],
          r.frames, r.code);
    free(r.out.data);
  }

  run_t r = run(A("--demo", "--net", "fast", NULL), 1.0, -1, "q");
  check(r.code == 2 && contains(r.out.data, r.out.len, "--net needs"),
        "--net with a bad value is refused");
  free(r.out.data);
  r = run(A("--demo", "--net", NULL), 1.0, -1, "q");
  check(r.code == 2, "--net without a value is refused");
  free(r.out.data);

  // Virtual camera: raw YUYV frames to a file, one whole frame per picture
  char vdir[] = "/tmp/asciicam_vcam_XXXXXX";
  if (!mkdtemp(vdir)) {
    check(0, "--vcam: cannot make a scratch directory");

    return;
  }

  char vfile[128];
  snprintf(vfile, sizeof vfile, "%s/cam.yuv", vdir);
  const int vw = 640, vh = 360;
  r = run(A("--demo", "-C", "-r", "20", "--vcam", vfile, "--vcam-size",
            "640x360", NULL),
          1.5, -1, "q");
  struct stat st;
  long vsize = stat(vfile, &st) == 0 ? (long)st.st_size : -1;
  long fsize = (long)vw * vh * 2;
  check(r.code == 0 && vsize > 0 && vsize % fsize == 0 && vsize / fsize >= 10,
        "--vcam: whole frames written (%ld bytes, %.1f frames, exit %d)", vsize,
        (double)vsize / (double)fsize, r.code);
  check(!contains(r.out.data, r.out.len, "Virtual camera:") || r.code == 0,
        "--vcam: clean exit");
  free(r.out.data);
  if (vsize >= fsize) {
    unsigned char *last = (unsigned char *)malloc((size_t)fsize);
    FILE *f = fopen(vfile, "rb");
    if (f && last && fseek(f, vsize - fsize, SEEK_SET) == 0 &&
        fread(last, 1, (size_t)fsize, f) == (size_t)fsize) {
      long bright = 0, dark = 0, chroma = 0;
      for (long i = 0; i < fsize; i += 2) {
        bright += last[i] > 100;
        dark += last[i] <= 20;
      }

      for (long i = 1; i < fsize; i += 4) {
        chroma += (last[i] > 128 ? last[i] - 128 : 128 - last[i]) > 20;
      }

      check(bright > 500 && dark > fsize / 8,
            "--vcam: picture drawn on a dark canvas (bright %ld, dark %ld)",
            bright, dark);
      check(chroma > 100,
            "--vcam: -C gives coloured pixels (%ld chroma samples)", chroma);
    } else {
      check(0, "--vcam: cannot read the last frame back");
    }

    if (f) {
      fclose(f);
    }

    free(last);
  }

  r = run(A("--demo", "--vcam", "/nonexistent-dir/cam", NULL), 1.0, -1, "q");
  check(r.code == 2 && contains(r.out.data, r.out.len, "--vcam: cannot open"),
        "--vcam with an unwritable path is refused");
  free(r.out.data);
  r = run(A("--demo", "--vcam", NULL), 1.0, -1, "q");
  check(r.code == 2, "--vcam without a value is refused");
  free(r.out.data);
  static const char *bad[] = {"641x360", "10x10", "abc", "1280x", "9000x9000"};
  for (int i = 0; i < 5; i++) {
    r = run(A("--demo", "--vcam", vfile, "--vcam-size", bad[i], NULL), 1.0, -1,
            "q");
    check(r.code == 2 && contains(r.out.data, r.out.len, "--vcam-size"),
          "--vcam-size %s is refused", bad[i]);
    free(r.out.data);
  }

  unlink(vfile);
  rmdir(vdir);
}

/* --play: replay a .cast with its timing, no camera (this is what runs over
 * ssh)
 * Output is kept as timed chunks so timing can be checked */

typedef struct {
  double t;
  unsigned char *d;
  size_t n;
} chunk_t;

typedef struct {
  chunk_t *c;
  int n, cap;
} chunks_t;

typedef struct {
  double at; // seconds after start
  const char *data;
} pstep_t;

static void chunks_add(chunks_t *ch, double t, const unsigned char *d,
                       size_t n) {
  if (ch->n == ch->cap) {
    ch->cap = ch->cap ? ch->cap * 2 : 64;
    ch->c = (chunk_t *)realloc(ch->c, (size_t)ch->cap * sizeof *ch->c);
    if (!ch->c) {
      abort();
    }
  }

  unsigned char *copy = (unsigned char *)malloc(n ? n : 1);
  memcpy(copy, d, n);
  ch->c[ch->n].t = t;
  ch->c[ch->n].d = copy;
  ch->c[ch->n].n = n;
  ch->n++;
}

static void chunks_free(chunks_t *ch) {
  for (int i = 0; i < ch->n; i++) {
    free(ch->c[i].d);
  }

  free(ch->c);
  memset(ch, 0, sizeof *ch);
}

// Run in a 30x100 terminal for up to `total` seconds, sending steps at their
// times
// Returns exit code, or -1 when it had to be killed
static int play(args_t a, const pstep_t *steps, int nsteps, double total,
                chunks_t *ch) {
  memset(ch, 0, sizeof *ch);
  char *argv[50];
  argv[0] = (char *)BIN;
  for (int i = 0; i < a.n; i++) {
    argv[i + 1] = (char *)a.v[i];
  }

  argv[a.n + 1] = NULL;
  pty_proc p;
  if (pty_spawn(&p, BIN, argv, 30, 100) != 0) {
    fprintf(stderr, "cannot start %s in a pty\n", BIN);
    exit(1);
  }

  double t0 = pty_now();
  int idx = 0, code = -1, done = 0;
  while (pty_now() - t0 < total && !done) {
    while (idx < nsteps && pty_now() - t0 >= steps[idx].at) {
      pty_write(&p, steps[idx].data, strlen(steps[idx].data));
      idx++;
    }

    pty_buf tmp = {0};
    long got = pty_read(&p, &tmp, 20);
    if (tmp.len) {
      chunks_add(ch, pty_now() - t0, tmp.data, tmp.len);
    }

    free(tmp.data);
    if (got < 0) {
      usleep(2000); // Terminal closed: wait for exit status
    }

    done = pty_exited(&p, &code);
  }

  if (!done) {
    pty_finish(&p, 0.0); // Kills it
    code = -1;
  } else if (p.fd >= 0) {
    close(p.fd);
    p.fd = -1;
  }

  return code;
}

// Time of first chunk that contains word, or -1
static double when(const chunks_t *ch, const char *word) {
  for (int i = 0; i < ch->n; i++) {
    if (contains(ch->c[i].d, ch->c[i].n, word)) {
      return ch->c[i].t;
    }
  }

  return -1;
}

// Everything program printed, joined
static pty_buf joined(const chunks_t *ch) {
  pty_buf b = {0};
  for (int i = 0; i < ch->n; i++) {
    pty_buf_add(&b, ch->c[i].d, ch->c[i].n);
  }

  return b;
}

static int count_chunks_with(const chunks_t *ch, const char *word) {
  int k = 0;
  for (int i = 0; i < ch->n; i++) {
    k += contains(ch->c[i].d, ch->c[i].n, word);
  }

  return k;
}

static void real_play(void) {
  char dir[] = "/tmp/asciicam_play_XXXXXX";
  if (!mkdtemp(dir)) {
    check(0, "--play: cannot make a scratch directory");

    return;
  }

  char cast[128], missing[128];
  snprintf(cast, sizeof cast, "%s/t.cast", dir);
  snprintf(missing, sizeof missing, "%s/missing.cast", dir);
  FILE *f = fopen(cast, "w");
  if (!f) {
    check(0, "--play: cannot write the recording");
    rmdir(dir);

    return;
  }

  // asciicast v2: a header line, then [time, "o", text] events
  fputs("{\"version\": 2, \"width\": 60, \"height\": 20}\n"
        "[0.0, \"o\", \"\\u001b[2J\\u001b[Hone\"]\n"
        "[0.5, \"o\", \"\\u001b[Htwo\"]\n"
        "[1.0, \"o\", \"\\u001b[Hthree \\u00e9\"]\n",
        f);
  fclose(f);

  chunks_t ch;
  int code = play(A("--play", cast, NULL), NULL, 0, 3.0, &ch);
  double t_two = when(&ch, "two"), t_three = when(&ch, "three");
  check(code == 0 && t_two >= 0 && t_three >= 0,
        "--play: plays to the end, exit %d", code);
  check(t_two > 0.4 && t_two < 0.8 && t_three > 0.9 && t_three < 1.4,
        "--play: keeps recorded timing (0.5 s / 1.0 s, got %.2f / %.2f)", t_two,
        t_three);
  pty_buf all = joined(&ch);
  check(contains(all.data, all.len, "\xc3\xa9"),
        "--play: UTF-8 bytes arrive intact");
  long wrap_off = rfind_mem(all.data, all.len, "\x1b[?7l");
  long wrap_on = rfind_mem(all.data, all.len, "\x1b[?7h");
  check(wrap_off >= 0 && wrap_on > wrap_off,
        "--play: wrap is off while playing and back on after");
  check(ch.n > 0 &&
            contains(ch.c[ch.n - 1].d, ch.c[ch.n - 1].n, "\x1b[?1049l") &&
            contains(all.data, all.len, "\x1b[?25h"),
        "--play: screen and cursor restored");
  free(all.data);
  chunks_free(&ch);

  code = play(A("--play", cast, "--speed", "2", NULL), NULL, 0, 3.0, &ch);
  t_three = when(&ch, "three");
  check(code == 0 && t_three >= 0 && t_three < 0.8,
        "--play --speed 2: twice as fast (%.2f)", t_three);
  chunks_free(&ch);

  pstep_t pause[] = {{0.7, " "}, {2.2, " "}};
  code = play(A("--play", cast, NULL), pause, 2, 4.0, &ch);
  t_three = when(&ch, "three");
  check(code == 0 && t_three > 2.3 && t_three < 3.0,
        "--play: space pauses and resumes (%.2f)", t_three);
  chunks_free(&ch);

  pstep_t quit_late[] = {{2.6, "q"}};
  code = play(A("--play", cast, "--loop", NULL), quit_late, 1, 5.0, &ch);
  check(code == 0 && count_chunks_with(&ch, "two") >= 2,
        "--play --loop: repeats, q quits with exit 0");
  chunks_free(&ch);

  pstep_t quit_early[] = {{0.2, "q"}};
  code = play(A("--play", cast, NULL), quit_early, 1, 3.0, &ch);
  check(code == 0 && when(&ch, "three") < 0, "--play: q stops early");
  chunks_free(&ch);

  code = play(A("--play", missing, NULL), NULL, 0, 3.0, &ch);
  all = joined(&ch);
  check(code == 1 && contains(all.data, all.len, "cannot read") &&
            !contains(all.data, all.len, "\x1b[?1049h"),
        "--play: missing file refused before touching the screen (exit %d)",
        code);
  free(all.data);
  chunks_free(&ch);

  code = play(A("--play", cast, "--speed", "fast", NULL), NULL, 0, 3.0, &ch);
  all = joined(&ch);
  check(code == 2 && contains(all.data, all.len, "--speed needs"),
        "--play: bad --speed refused");
  free(all.data);
  chunks_free(&ch);

  code = play(A("--play", NULL), NULL, 0, 3.0, &ch);
  check(code == 2, "--play without a file is refused");
  chunks_free(&ch);

  unlink(cast);
  rmdir(dir);
}

/* --image: a picture file (or a folder of them) as camera */

// Baseline JPEG, 64x32, solid (200, 40, 40), made with PIL at quality 90
// Kept as bytes so check runs everywhere, not only where PIL is installed
static const unsigned char JPEG_RED[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
    0x00, 0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03, 0x03, 0x03, 0x03, 0x04,
    0x03, 0x03, 0x04, 0x05, 0x08, 0x05, 0x05, 0x04, 0x04, 0x05, 0x0a, 0x07,
    0x07, 0x06, 0x08, 0x0c, 0x0a, 0x0c, 0x0c, 0x0b, 0x0a, 0x0b, 0x0b, 0x0d,
    0x0e, 0x12, 0x10, 0x0d, 0x0e, 0x11, 0x0e, 0x0b, 0x0b, 0x10, 0x16, 0x10,
    0x11, 0x13, 0x14, 0x15, 0x15, 0x15, 0x0c, 0x0f, 0x17, 0x18, 0x16, 0x14,
    0x18, 0x12, 0x14, 0x15, 0x14, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x03, 0x04,
    0x04, 0x05, 0x04, 0x05, 0x09, 0x05, 0x05, 0x09, 0x14, 0x0d, 0x0b, 0x0d,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x20, 0x00, 0x40, 0x03,
    0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00,
    0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00,
    0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
    0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81,
    0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24,
    0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56,
    0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a,
    0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
    0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9,
    0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00,
    0x1f, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00,
    0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31,
    0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08,
    0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
    0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xda, 0x00,
    0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xf1,
    0x4a, 0x28, 0xa2, 0xbf, 0x37, 0x3f, 0xb5, 0x42, 0x8a, 0x28, 0xa0, 0x02,
    0x8a, 0x28, 0xa0, 0x02, 0x8a, 0x28, 0xa0, 0x02, 0x8a, 0x28, 0xa0, 0x02,
    0x8a, 0x28, 0xa0, 0x02, 0x8a, 0x28, 0xa0, 0x02, 0x8a, 0x28, 0xa0, 0x0f,
    0xff, 0xd9,
};

static void px_white(int x, int y, unsigned char c[3], void *u) {
  (void)x;
  (void)y;
  (void)u;
  c[0] = c[1] = c[2] = 255;
}

static void px_half(int x, int y, unsigned char c[3],
                    void *u) { // right half white
  (void)y;
  (void)u;
  c[0] = c[1] = c[2] = x >= 32 ? 255 : 0;
}

static void px_red_blue(int x, int y, unsigned char c[3],
                        void *u) { // left red, right blue
  (void)y;
  (void)u;
  if (x < 10) {
    c[0] = 220;
    c[1] = 30;
    c[2] = 30;
  } else {
    c[0] = 30;
    c[1] = 30;
    c[2] = 220;
  }
}

static void px_solid(int x, int y, unsigned char c[3], void *u) {
  (void)x;
  (void)y;
  memcpy(c, u, 3);
}

static int write_file(const char *path, const void *d, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    return 0;
  }

  fwrite(d, 1, n, f);

  return fclose(f) == 0;
}

// Next ESC[38;2;R;G;Bm at or after *pos
static int next_colour(const unsigned char *b, size_t n, size_t *pos,
                       int rgb[3]) {
  static const char pre[] = "\x1b[38;2;";
  size_t pl = sizeof pre - 1;
  for (size_t i = *pos; i + pl < n; i++) {
    if (memcmp(b + i, pre, pl)) {
      continue;
    }

    size_t j = i + pl;
    int v[3] = {0, 0, 0};
    int ok = 1;
    for (int c = 0; c < 3 && ok; c++) {
      int nd = 0;
      while (j < n && b[j] >= '0' && b[j] <= '9') {
        v[c] = v[c] * 10 + (b[j++] - '0');
        nd++;
      }

      ok = nd > 0 && j < n && b[j] == (c < 2 ? ';' : 'm');
      j++;
    }

    if (ok) {
      rgb[0] = v[0];
      rgb[1] = v[1];
      rgb[2] = v[2];
      *pos = j;

      return 1;
    }
  }

  return 0;
}

// Which channel carries most colour in this stretch of output: 'r', 'g', 'b',
// or 0 when there is none
static char dominant(const unsigned char *b, size_t n) {
  long tot[3] = {0, 0, 0};
  size_t pos = 0;
  int rgb[3];
  int any = 0;
  while (next_colour(b, n, &pos, rgb)) {
    tot[0] += rgb[0];
    tot[1] += rgb[1];
    tot[2] += rgb[2];
    any = 1;
  }

  if (!any) {
    return 0;
  }

  int best = 0;
  for (int i = 1; i < 3; i++) {
    if (tot[i] > tot[best]) {
      best = i;
    }
  }

  return "rgb"[best];
}

typedef struct {
  char **row;
  int n;
} rows_t;

// Text rows of last complete frame: stretch between last two
// synchronized-update starts, escape sequences removed, empty lines dropped
static rows_t frame_rows(const pty_buf *b) {
  static const char mark[] = "\x1b[?2026h";
  size_t ml = sizeof mark - 1;
  long starts[4096];
  int ns = 0;
  for (size_t i = 0; i + ml <= b->len && ns < 4096;) {
    if (!memcmp(b->data + i, mark, ml)) {
      starts[ns++] = (long)i;
      i += ml;
    } else {
      i++;
    }
  }

  const unsigned char *p = b->data;
  size_t n = b->len;
  if (ns >= 2) {
    p = b->data + starts[ns - 2] + ml;
    n = (size_t)(starts[ns - 1] - starts[ns - 2]) - ml;
  }

  // Drop CSI sequences: ESC [ [0-9;?<]* letter
  unsigned char *text = (unsigned char *)malloc(n + 1);
  size_t tn = 0;
  for (size_t i = 0; i < n;) {
    if (p[i] == 0x1b && i + 1 < n && p[i + 1] == '[') {
      size_t j = i + 2;
      while (j < n && ((p[j] >= '0' && p[j] <= '9') || p[j] == ';' ||
                       p[j] == '?' || p[j] == '<')) {
        j++;
      }

      if (j < n &&
          ((p[j] >= 'A' && p[j] <= 'Z') || (p[j] >= 'a' && p[j] <= 'z'))) {
        i = j + 1;

        continue;
      }
    }

    text[tn++] = p[i++];
  }

  text[tn] = 0;
  rows_t r = {NULL, 0};
  size_t i = 0;
  while (i <= tn) {
    size_t j = i;
    while (j < tn && text[j] != '\n') {
      j++;
    }

    size_t a = i;
    size_t e = j;
    while (a < e && text[a] == '\r') {
      a++;
    }

    while (e > a && text[e - 1] == '\r') {
      e--;
    }

    if (e > a) {
      r.row = (char **)realloc(r.row, (size_t)(r.n + 1) * sizeof *r.row);
      r.row[r.n] = (char *)malloc(j - i + 1);
      memcpy(r.row[r.n], text + i, j - i);
      r.row[r.n][j - i] = 0;
      r.n++;
    }

    i = j + 1;
  }

  free(text);

  return r;
}

static void rows_free(rows_t *r) {
  for (int i = 0; i < r->n; i++) {
    free(r->row[i]);
  }

  free(r->row);
  r->row = NULL;
  r->n = 0;
}

static int count_char(const char *s, char c) {
  int k = 0;
  for (; *s; s++) {
    k += *s == c;
  }

  return k;
}

static void real_image(void) {
  char idir[] = "/tmp/asciicam_img_XXXXXX";
  if (!mkdtemp(idir)) {
    check(0, "--image: cannot make a scratch directory");

    return;
  }

  const args_t IARGS = A("-r", "20", "-m", "ascii", "-s", " .:-=+*#%@", "-W",
                         "60", "-H", "40", NULL);
  const args_t IARGS4 = A("-r", "20", "-m", "ascii", "-s", " .:-=+*#%@", NULL);
  char path[600];
#define P(name) (snprintf(path, sizeof path, "%s/%s", idir, name), path)

  // A white picture: every cell bright; same thing through -d image:<file>
  char white[600];
  snprintf(white, sizeof white, "%s/white.png", idir);
  mz_png_write(white, 64, 64, px_white, NULL, 0, MZ_DYNAMIC);
  run_t r = run(cat(A("--image", white, NULL), IARGS), 1.0, -1, "q");
  check(r.frames >= 10 && r.code == 0 && memchr(r.out.data, '@', r.out.len),
        "--image PNG: drawn (%ld frames, exit %d)", r.frames, r.code);
  free(r.out.data);
  char dimg[640];
  snprintf(dimg, sizeof dimg, "image:%s", white);
  r = run(cat(A("-d", dimg, NULL), IARGS), 1.0, -1, "q");
  check(r.code == 0 && memchr(r.out.data, '@', r.out.len),
        "-d image:<file> is the same thing");
  free(r.out.data);

  // PGM, all black: no bright cells
  unsigned char pgm[11 + 64] = "P5\n8 8\n255\n";
  memset(pgm + 11, 0, 64);
  P("black.pgm");
  write_file(path, pgm, sizeof pgm);
  r = run(cat(A("--image", path, NULL), IARGS), 1.0, -1, "q");
  check(r.code == 0 && !memchr(r.out.data, '@', r.out.len),
        "--image PGM, all black: no bright cells");
  free(r.out.data);

  // Left half black, right half white: bright cells only on right
  char half[600];
  snprintf(half, sizeof half, "%s/half.png", idir);
  mz_png_write(half, 64, 64, px_half, NULL, 0, MZ_DYNAMIC);
  r = run(cat(A("--image", half, NULL), IARGS), 1.0, -1, "q");
  rows_t rows = frame_rows(&r.out);
  int nb = 0, minfirst = 1 << 30, all_over_3 = 1;
  for (int i = 0; i < rows.n; i++) {
    const char *at = strchr(rows.row[i], '@');
    if (!at) {
      continue;
    }

    nb++;
    int first = (int)(at - rows.row[i]);
    if (first < minfirst) {
      minfirst = first;
    }

    all_over_3 &= count_char(rows.row[i], '@') > 3;
  }

  check(nb > 0 && minfirst > 5 && all_over_3,
        "left half black, right half white: bright cells only on the right");
  rows_free(&rows);
  free(r.out.data);

  // A wide picture keeps its proportions: bars above and below
  char wide[600];
  snprintf(wide, sizeof wide, "%s/wide.png", idir);
  mz_png_write(wide, 200, 20, px_white, NULL, 0, MZ_DYNAMIC);
  r = run(cat(A("--image", wide, NULL), IARGS), 1.0, -1, "q");
  rows = frame_rows(&r.out);
  int bright = 0;
  for (int i = 0; i < rows.n; i++) {
    bright += strchr(rows.row[i], '@') != NULL;
  }

  check(r.code == 0 && bright > 0 && bright < rows.n - 4,
        "wide picture keeps its proportions: bars above and below (%d of %d "
        "rows)",
        bright, rows.n);
  rows_free(&rows);
  free(r.out.data);

  // Colour on a picture
  r = run(cat(A("--image", half, "-C", NULL), IARGS4), 1.0, -1, "q");
  check(r.code == 0 && contains(r.out.data, r.out.len, "\x1b[38;2;"),
        "--image with -C: colour");
  free(r.out.data);

  // Errors say what is wrong
  r = run(cat(A("--image", P("nope.png"), NULL), IARGS), 0.5, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "cannot open"),
        "--image missing file: cannot open");
  free(r.out.data);
  P("notpic.txt");
  write_file(path, "this is text, not a picture\n", 28);
  r = run(cat(A("--image", path, NULL), IARGS), 0.5, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "not a PNG"),
        "--image not a picture: not a PNG");
  free(r.out.data);
  unsigned char broken[104] = {0xff, 0xd8, 0xff, 0xe0};
  P("x.jpg");
  write_file(path, broken, sizeof broken);
  r = run(cat(A("--image", path, NULL), IARGS), 0.5, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "JPEG"),
        "--image damaged JPEG: says so");
  free(r.out.data);
  P("ok.jpg");
  write_file(path, JPEG_RED, sizeof JPEG_RED);
  r = run(cat(A("--image", path, "-C", NULL), IARGS4), 1.0, -1, "q");
  check(r.code == 0 && contains(r.out.data, r.out.len, "\x1b[38;2;"),
        "--image JPEG: shown in colour");
  free(r.out.data);

  // Adam7 interlaced PNG: both halves arrive in right colours
  P("adam7.png");
  mz_png_write(path, 20, 20, px_red_blue, NULL, 1, MZ_FIXED);
  r = run(cat(A("--image", path, "-C", NULL), IARGS4), 1.0, -1, "q");
  int red = 0, blue = 0, rgb[3];
  size_t pos = 0;
  while (next_colour(r.out.data, r.out.len, &pos, rgb)) {
    red |= rgb[0] > 150 && rgb[2] < 100;
    blue |= rgb[2] > 150 && rgb[0] < 100;
  }

  check(r.code == 0 && red && blue,
        "--image Adam7 PNG: both halves arrive in the right colours");
  free(r.out.data);

  // A folder is a slideshow: . and , step, a broken picture is skipped
  char sdir[600];
  snprintf(sdir, sizeof sdir, "%s/show", idir);
  mkdir(sdir, 0755);
  static const struct {
    const char *name;
    unsigned char rgb[3];
  } slides[] = {{"a.png", {230, 30, 30}},
                {"b.png", {30, 230, 30}},
                {"c.png", {30, 30, 230}}};
  for (int i = 0; i < 3; i++) {
    char f[700];
    snprintf(f, sizeof f, "%s/%s", sdir, slides[i].name);
    mz_png_write(f, 16, 16, px_solid, (void *)slides[i].rgb, 0, MZ_FIXED);
  }

  char f[700];
  snprintf(f, sizeof f, "%s/d.png", sdir);
  write_file(f, "not a picture", 13);
  snprintf(f, sizeof f, "%s/notes.txt", sdir);
  write_file(f, "x", 1);

  step_t dots[] = {S(".", 0.7), S(".", 0.7), S(".", 0.7), S(",", 0.7)};
  sess_t se =
      session(cat(A("--image", sdir, "--slide", "0", "-C", NULL), IARGS4), dots,
              4, 0.6);
  char seq[8] = "";
  for (int k = 0; k < 5; k++) {
    size_t from = k == 0 ? 0 : se.marks[k - 1];
    size_t to = se.marks[k];
    char d = dominant(se.out.data + from, to - from);
    seq[k] = d ? d : '-';
  }

  seq[5] = 0;
  check(!strcmp(seq, "rgbrb"),
        "slideshow: start, . . . (broken one skipped), , -> ['%c', '%c', '%c', "
        "'%c', '%c']",
        seq[0], seq[1], seq[2], seq[3], seq[4]);
  check(contains(se.out.data + se.marks[0], se.marks[1] - se.marks[0], "2/4") &&
            contains(se.out.data + se.marks[0], se.marks[1] - se.marks[0],
                     "b.png"),
        "slideshow: a toast names the picture");
  check(!contains(se.out.data, se.out.len, "d.png"),
        "slideshow: the broken picture is never announced");
  free(se.out.data);

  // --slide 1: moves on by itself
  r = run(cat(A("--image", sdir, "--slide", "1", "-C", NULL), IARGS4), 3.2, -1,
          "q");
  int seen = 0;
  static const char mark[] = "\x1b[?2026h";
  size_t ml = sizeof mark - 1;
  for (size_t i = 0; i + ml <= r.out.len;) {
    if (memcmp(r.out.data + i, mark, ml)) {
      i++;

      continue;
    }

    size_t a = i + ml;
    size_t e = a;
    while (e + ml <= r.out.len && memcmp(r.out.data + e, mark, ml)) {
      e++;
    }

    if (e + ml > r.out.len) {
      e = r.out.len;
    }

    char d = dominant(r.out.data + a, e - a);
    seen |= d == 'r' ? 1 : d == 'g' ? 2 : d == 'b' ? 4 : 0;
    i = e;
  }

  char seen_str[32] = "";
  static const char names[3] = {'b', 'g', 'r'};
  static const int bits[3] = {4, 2, 1};
  for (int i = 0; i < 3; i++) {
    if (seen & bits[i]) {
      size_t u = strlen(seen_str);
      snprintf(seen_str + u, sizeof seen_str - u, "%s'%c'", u ? ", " : "",
               names[i]);
    }
  }

  check(r.code == 0 && __builtin_popcount((unsigned)seen) >= 3,
        "slideshow: moves on by itself, --slide 1 ([%s])", seen_str);
  free(r.out.data);

  P("empty");
  mkdir(path, 0755);
  r = run(cat(A("--image", path, NULL), IARGS), 0.5, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "no pictures"),
        "slideshow: empty folder says so");
  free(r.out.data);
  r = run(cat(A("--image", sdir, "--slide", "x", NULL), IARGS), 0.5, -1, "q");
  check(r.code == 2 && contains(r.out.data, r.out.len, "--slide needs"),
        "--slide with a bad number: usage error");
  free(r.out.data);
#undef P

  rm_rf(idir);
}

// Screen model itself: what a terminal would show for known byte strings
static void screen_case(const char *name, int rows, int cols, int lnm,
                        const char *in, const char *want) {
  screen_t s;
  screen_init(&s, rows, cols);
  s.lnm = lnm;
  screen_feed(&s, (const unsigned char *)in, strlen(in));
  char *t = screen_text(&s);
  int ok = !strcmp(t, want);
  check(ok, "screen model: %s", name);
  if (!ok) {
    printf("     got  [%s]\n     want [%s]\n", t, want);
  }

  free(t);
  screen_free(&s);
}

static void screen_tests(void) {
  screen_case("text and newline", 3, 5, 1, "ab\ncd", "ab\ncd\n\n");
  screen_case("LF alone keeps the column", 3, 6, 0, "ab\ncd", "ab\n  cd\n\n");
  screen_case("cursor position", 3, 6, 0, "\x1b[2;4Hx", "\n   x\n\n");
  screen_case("cursor position defaults", 3, 6, 0, "\x1b[Hq\x1b[;5Hz",
              "q   z\n\n\n");
  screen_case("cursor position clamps", 2, 4, 0, "\x1b[99;99Hx", "\n   x\n");
  screen_case("erase display", 2, 4, 1, "ab\ncd\x1b[2J", "\n\n");
  screen_case("erase below", 2, 4, 1, "abcd\nefgh\x1b[1;3H\x1b[J", "ab\n\n");
  screen_case("erase above", 2, 4, 1, "abcd\nefgh\x1b[2;2H\x1b[1J", "\n  gh\n");
  screen_case(
      "erase in line", 3, 6, 1,
      "abcdef\nabcdef\nabcdef\x1b[1;3H\x1b[K\x1b[2;3H\x1b[1K\x1b[3;3H\x1b[2K",
      "ab\n   def\n\n");
  screen_case("erase characters", 1, 6, 0, "abcdef\x1b[1;2H\x1b[3X",
              "a   ef\n");
  screen_case("auto-wrap waits for the next glyph", 3, 5, 0, "abcdefg",
              "abcde\nfg\n\n");
  screen_case("auto-wrap off overwrites the last column", 2, 5, 0,
              "\x1b[?7labcdefg", "abcdg\n\n");
  screen_case("auto-wrap back on", 2, 5, 0, "\x1b[?7l\x1b[?7habcdefg",
              "abcde\nfg\n");
  screen_case("a full line then a cursor move does not wrap", 2, 5, 0,
              "abcde\x1b[1;1HX", "Xbcde\n\n");
  screen_case("scrolls at the bottom", 3, 4, 1, "1\n2\n3\n4", "2\n3\n4\n");
  screen_case("colours and private modes change nothing", 1, 8, 0,
              "\x1b[1;38;2;1;2;3mhi\x1b[0m\x1b[?25l\x1b[?2026h!", "hi!\n");
  screen_case("unknown sequences are consumed", 1, 8, 0,
              "a\x1b[5;2~b\x1b[?1049hc", "abc\n");
  screen_case("OSC ends at BEL or ST", 1, 8, 0,
              "a\x1b]0;title\x07"
              "b\x1b]2;x\x1b\\"
              "c",
              "abc\n");
  screen_case("character set designation is skipped", 1, 8, 0, "a\x1b(Bb",
              "ab\n");
  screen_case("backspace and tab", 1, 20, 0, "abc\bX\tY", "abX     Y\n");
  screen_case("UTF-8", 1, 8, 0, "\xc3\xa9\xe2\x97\x8f\xe2\xa0\x81x",
              "\xc3\xa9\xe2\x97\x8f\xe2\xa0\x81x\n");
  screen_case("invalid UTF-8 becomes U+FFFD", 1, 8, 0,
              "a\xff"
              "b\xc3"
              "c",
              "a\xef\xbf\xbd"
              "b\xef\xbf\xbd"
              "c\n");

  screen_t s; // An escape sequence and a character cut in two by reads
  screen_init(&s, 1, 10);
  const char *part[] = {"\x1b[1;", "3Hx\xe2\x97", "\x8f"};
  for (int i = 0; i < 3; i++) {
    screen_feed(&s, (const unsigned char *)part[i], strlen(part[i]));
  }

  char *t = screen_text(&s);
  check(!strcmp(t, "  x\xe2\x97\x8f\n"),
        "screen model: a sequence and a character split across reads");
  free(t);
  screen_free(&s);
}

// TCP: --serve sends frames to viewers, --watch shows what a server sends
static int free_port(void) {
  int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t l = sizeof a;
  int port = -1;
  if (s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 &&
      getsockname(s, (struct sockaddr *)&a, &l) == 0) {
    port = ntohs(a.sin_port);
  }

  if (s >= 0) {
    close(s);
  }

  return port;
}

// Listening socket on 127.0.0.1:port (address reuse on), or -1
// Make it BEFORE starting program that connects: binding inside server thread
// raced with first connect
static int listen_on(int port) {
  int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int one = 1;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((unsigned short)port);
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0 ||
      listen(s, 1) != 0) {
    if (s >= 0) {
      close(s);
    }

    return -1;
  }

  return s;
}

// accept() with a time limit; -1 on timeout
// Every socket tests make is close-on-exec: program under test must not inherit
// a listening socket (it would keep port open after test closed it)
static int accept_within(int lfd, int ms) {
  struct pollfd pf = {lfd, POLLIN, 0};
  if (poll(&pf, 1, ms) <= 0) {
    return -1;
  }

  return accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
}

// Connect to program, retrying for up to 4 s while it starts up
static int connect_retry(int port) {
  for (int i = 0; i < 40; i++) {
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
      return s;
    }

    if (s >= 0) {
      close(s);
    }

    usleep(100000);
  }

  return -1;
}

typedef struct {
  int port;
  double delay; // Start after delay,
  double secs;  //  Read for seconds
  pty_buf got;
} viewer_t;

static void *viewer_thread(void *arg) {
  viewer_t *v = (viewer_t *)arg;
  usleep((useconds_t)(v->delay * 1e6));
  int fd = connect_retry(v->port);
  if (fd < 0) {
    return NULL;
  }

  struct timeval tv = {0, 300000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  double end = pty_now() + v->secs;
  unsigned char buf[65536];
  while (pty_now() < end) {
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n > 0) {
      pty_buf_add(&v->got, buf, (size_t)n);
    } else if (n == 0 ||
               (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      break;
    }
  }

  close(fd);

  return NULL;
}

static void send_all(int fd, const char *s) {
  size_t n = strlen(s);
  while (n > 0) {
    ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
    if (w <= 0) {
      return;
    }

    s += w;
    n -= (size_t)w;
  }
}

// One client: send a clear-screen and a word, stay 0.4 s, hang up
static void *serve_once_thread(void *arg) {
  int lfd = *(int *)arg;
  int c = accept_within(lfd, 8000);
  if (c >= 0) {
    send_all(c, "\x1b[2JHELLO-WATCH-FRAME");
    usleep(400000);
    close(c);
  }

  return NULL;
}

typedef struct {
  int first_lfd, port;
} restart_t;

// A server that goes away and comes back: BEFORE-RESTART, 2.5 s of nobody
// listening, then AFTER-RESTART
static void *restarting_thread(void *arg) {
  restart_t *r = (restart_t *)arg;
  static const char *words[] = {"\x1b[2JBEFORE-RESTART",
                                "\x1b[2JAFTER-RESTART"};
  for (int n = 0; n < 2; n++) {
    int lfd = n == 0 ? r->first_lfd : listen_on(r->port);
    if (lfd < 0) {
      return NULL;
    }

    int c = accept_within(lfd, 8000);
    if (c < 0) {
      close(lfd);

      return NULL;
    }

    send_all(c, words[n]);
    usleep(500000);
    close(c);
    close(lfd);
    usleep(2500000);
  }

  return NULL;
}

// Offset of first occurrence of needle at or after `from`, or -1
static long find_from(const unsigned char *p, size_t n, size_t from,
                      const char *needle) {
  if (from >= n) {
    return -1;
  }

  const void *m = memmem(p + from, n - from, needle, strlen(needle));

  return m ? (long)((const unsigned char *)m - p) : -1;
}

// What a 200 x 100 terminal shows after these bytes (rows joined by '\n')
static char *shown(const pty_buf *b) {
  screen_t s;
  screen_init(&s, 100, 200);
  s.lnm = 1; // A TCP stream has bare "\n" line ends
  screen_feed(&s, b->data, b->len);
  char *t = screen_text(&s);
  screen_free(&s);

  return t;
}

// Text rows of first frame (between first sync start and end), found by cutting
// escape sequences out, one per line, empty lines dropped
static char *first_frame_text(const pty_buf *b) {
  long a = find_from(b->data, b->len, 0, "\x1b[?2026h");
  if (a < 0) {
    return strdup("");
  }

  a += 8;
  long e = find_from(b->data, b->len, (size_t)a, "\x1b[?2026l");
  size_t end = e < 0 ? b->len : (size_t)e;
  pty_buf frame = {b->data + a, end - (size_t)a, end - (size_t)a};
  rows_t rows = frame_rows(&frame);
  size_t cap = 1;
  for (int i = 0; i < rows.n; i++) {
    cap += strlen(rows.row[i]) + 1;
  }

  char *out = (char *)calloc(cap, 1);
  for (int i = 0; i < rows.n; i++) {
    size_t l = strlen(rows.row[i]);
    while (l > 0 && (rows.row[i][l - 1] == ' ' || rows.row[i][l - 1] == '\r')) {
      l--;
    }

    strncat(out, rows.row[i], l);
    strcat(out, "\n");
  }

  rows_free(&rows);

  return out;
}

// Non-empty rows of a screen text, joined by '\n' (blank rows dropped)
static char *nonblank_rows(const char *text) {
  char *out = (char *)calloc(strlen(text) + 1, 1);
  const char *p = text;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t l = nl ? (size_t)(nl - p) : strlen(p);
    if (l) {
      strncat(out, p, l);
      strcat(out, "\n");
    }

    p += l + (nl ? 1 : 0);
  }

  return out;
}

static void real_serve_watch(void) {
  char idir[] = "/tmp/asciicam_serve_XXXXXX";
  if (!mkdtemp(idir)) {
    check(0, "--serve: cannot make a scratch directory");

    return;
  }

  char pic[600];
  snprintf(pic, sizeof pic, "%s/a.png", idir);
  static const unsigned char red[3] = {230, 30, 30};
  mz_png_write(pic, 16, 16, px_solid, (void *)red, 0, MZ_FIXED);
  const args_t IARGS = A("-r", "20", "-m", "ascii", "-s", " .:-=+*#%@", "-W",
                         "60", "-H", "40", NULL);
  const args_t IARGS4 = A("-r", "20", "-m", "ascii", "-s", " .:-=+*#%@", NULL);
  char portstr[16];

  // --serve: a viewer on TCP gets a clear-screen, then whole frames
  int port = free_port();
  snprintf(portstr, sizeof portstr, "%d", port);
  viewer_t v1 = {port, 0.0, 1.5, {0}};
  pthread_t th;
  pthread_create(&th, NULL, viewer_thread, &v1);
  run_t r = run(cat(A("--image", pic, "-C", "--serve", portstr, NULL), IARGS4),
                3.0, -1, "q");
  pthread_join(th, NULL);
  const unsigned char *g = v1.got.data;
  size_t gn = v1.got.len;
  check(r.code == 0 &&
            starts_with_str((const char *)g, gn, "\x1b[0m\x1b[?25l\x1b[2J"),
        "--serve: viewer gets a clear-screen first");
  size_t on_n = pty_buf_count(&v1.got, "\x1b[?2026h");
  size_t off_n = pty_buf_count(&v1.got, "\x1b[?2026l");
  check(on_n >= 5 && on_n == off_n,
        "--serve: frames arrive whole, one sync pair each");
  check(contains(g, gn, "\x1b[38;2;"),
        "--serve: the colours go to the viewer too");
  check(contains(r.out.data, r.out.len, "viewer connected"),
        "--serve: the host sees a toast when a viewer joins");
  free(r.out.data);
  free(v1.got.data);

  // A viewer who comes late is brought up to date and later frames are only
  // changes: a still picture costs one whole picture, not one per frame
  port = free_port();
  snprintf(portstr, sizeof portstr, "%d", port);
  viewer_t first = {port, 0.0, 2.2, {0}};
  viewer_t late = {port, 1.0, 1.4, {0}};
  pthread_t t1;
  pthread_t t2;
  pthread_create(&t1, NULL, viewer_thread, &first);
  pthread_create(&t2, NULL, viewer_thread, &late);
  r = run(cat(A("--image", pic, "-C", "--serve", portstr, NULL), IARGS4), 3.5,
          -1, "q");
  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  long end_first = find_from(first.got.data, first.got.len, 100, "\x1b[?2026l");
  check(first.got.len > 1000 && (long)first.got.len < 6 * end_first + 4000,
        "--serve: a still picture is sent once, then only changes (%zu bytes)",
        first.got.len);
  check(contains(late.got.data, late.got.len, "\x1b[38;2;") &&
            pty_buf_count(&late.got, "\x1b[?2026h") >= 2,
        "--serve: a viewer that comes late gets the whole picture");

  char *s_first = shown(&first.got), *s_late = shown(&late.got);
  char *flat = nonblank_rows(s_late);
  check(!strcmp(s_first, s_late) && flat[0],
        "--serve: the late viewer ends up with the same picture");

  char *ref = first_frame_text(&first.got), *got_rows = nonblank_rows(s_first);
  check(!strcmp(ref, got_rows) && ref[0],
        "--serve: the screen model shows the frame's own text rows");
  free(ref);
  free(got_rows);
  free(flat);
  free(s_first);
  free(s_late);
  free(first.got.data);
  free(late.got.data);
  free(r.out.data);

  r = run(cat(A("--serve", "x", NULL), IARGS), 0.5, -1, "q");
  check(r.code == 2 && contains(r.out.data, r.out.len, "--serve needs a port"),
        "--serve with a bad address: usage error");
  free(r.out.data);
  r = run(A("--serve", NULL), 0.5, -1, "q");
  check(r.code == 2 &&
            contains(r.out.data, r.out.len, "--serve needs an address"),
        "--serve without an address: usage error");
  free(r.out.data);

  // --watch: copies what a server sends, quits when it closes
  port = free_port();
  int srv = listen_on(port);
  pthread_create(&th, NULL, serve_once_thread, &srv);
  char watch[48];
  snprintf(watch, sizeof watch, "127.0.0.1:%d", port);
  r = run(A("--watch", watch, NULL), 3.0, -1, "q");
  pthread_join(th, NULL);
  close(srv);
  check(contains(r.out.data, r.out.len, "HELLO-WATCH-FRAME"),
        "--watch: shows what the server sends");
  check(r.code == 1 && contains(r.out.data, r.out.len, "closed the connection"),
        "--watch: says so when the server goes away");
  free(r.out.data);

  // --watch --retry: server restarts, viewer comes back
  restart_t rs = {0, free_port()};
  rs.first_lfd = listen_on(rs.port); // Listening before viewer starts
  pthread_create(&th, NULL, restarting_thread, &rs);
  snprintf(watch, sizeof watch, "127.0.0.1:%d", rs.port);
  r = run(A("--watch", watch, "--retry", NULL), 7.0, -1, "q");
  pthread_join(th, NULL);
  int saw_before = contains(r.out.data, r.out.len, "BEFORE-RESTART");
  int saw_recon = contains(r.out.data, r.out.len, "reconnecting");
  int saw_after = contains(r.out.data, r.out.len, "AFTER-RESTART");
  check(saw_before && saw_recon && saw_after,
        "--watch --retry: reconnects after the server restarts (before %d, "
        "reconnecting %d, after %d)",
        saw_before, saw_recon, saw_after);
  check(r.code == 0, "--watch --retry: q still quits (%d)", r.code);
  long wrap_off = rfind_mem(r.out.data, r.out.len, "\x1b[?7l");
  long wrap_on = rfind_mem(r.out.data, r.out.len, "\x1b[?7h");
  check(wrap_off >= 0 && wrap_on > wrap_off,
        "--watch: lines are cut at the edge of a small terminal, wrap is put "
        "back on leaving");
  free(r.out.data);

  rm_rf(idir);
}

static void real_binary(const char *real, int only_serve) {
  char *abs = realpath(real, NULL);
  if (!abs || access(abs, X_OK) != 0) {
    return; // No real binary, nothing to check
  }

  const char *saved = BIN;
  BIN = abs;
  if (only_serve) {
    real_serve_watch();
    BIN = saved;
    free(abs);

    return;
  }

  real_demo_and_plugins(abs);
  real_net_and_vcam();
  real_play();

  // A missing camera explains how to get a picture
  run_t r = run(A("-d", "/dev/definitely-not-a-camera", NULL), 0.3, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "--demo"),
        "missing camera: error mentions --demo");
  free(r.out.data);
  real_image();
  real_serve_watch();
  BIN = saved;
  free(abs);
}

// Files, environment and little JSON recorder writes
static char REAL_HOME[1024]; // HOME of test run itself
static char START_DIR[4096];

static void set_home(const char *h) { setenv("HOME", h, 1); }

static int is_dir(const char *p) {
  struct stat st;

  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// rm -rf for scratch directories tests make
static void rm_rf(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return;
  }

  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
        char sub[4300];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        rm_rf(sub);
      }
    }

    if (d) {
      closedir(d);
    }

    rmdir(path);
  } else {
    unlink(path);
  }
}

static int cmp_names(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

typedef struct {
  char name[16][256];
  int n;
} names_t;

// Names in dir, sorted (at most 16)
static names_t list_dir(const char *dir) {
  names_t l;
  memset(&l, 0, sizeof l);
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d)) && l.n < 16) {
    if (e->d_name[0] != '.') {
      snprintf(l.name[l.n++], sizeof l.name[0], "%s", e->d_name);
    }
  }

  if (d) {
    closedir(d);
  }

  qsort(l.name, (size_t)l.n, sizeof l.name[0], cmp_names);

  return l;
}

static int ends_with(const char *s, const char *suffix) {
  size_t a = strlen(s);
  size_t b = strlen(suffix);

  return a >= b && !strcmp(s + a - b, suffix);
}

static char *read_text(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = (char *)malloc((size_t)len + 1);
  if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) {
    fclose(f);
    free(b);

    return NULL;
  }

  fclose(f);
  b[len] = 0;
  if (n) {
    *n = (size_t)len;
  }

  return b;
}

// Number after "key": in a JSON object text
static int json_int(const char *text, const char *key, long *v) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(text, pat);
  if (!p) {
    return 0;
  }

  p = strchr(p + strlen(pat), ':');
  if (!p) {
    return 0;
  }

  char *end;
  *v = strtol(p + 1, &end, 10);

  return end != p + 1;
}

// Decode JSON string starting at *pp (on opening quote) into bytes (UTF-8)
// Returns a malloc'd NUL-terminated string and moves *pp past closing quote, or
// NULL
static char *json_string(const char **pp) {
  const char *p = *pp;
  if (*p != '"') {
    return NULL;
  }

  p++;
  size_t cap = strlen(p) + 1, n = 0;
  char *o = (char *)malloc(cap);
  while (*p && *p != '"') {
    if (*p != '\\') {
      o[n++] = *p++;

      continue;
    }

    p++;
    switch (*p) {
    case 'n': {
      o[n++] = '\n';
      p++;
      break;
    }
    case 'r': {
      o[n++] = '\r';
      p++;
      break;
    }
    case 't': {
      o[n++] = '\t';
      p++;
      break;
    }
    case 'b': {
      o[n++] = '\b';
      p++;
      break;
    }
    case 'f': {
      o[n++] = '\f';
      p++;
      break;
    }
    case 'u': {
      unsigned cp = 0;
      for (int i = 1; i <= 4; i++) {
        char c = p[i];
        cp = cp * 16 + (unsigned)(c >= 'a'   ? c - 'a' + 10
                                  : c >= 'A' ? c - 'A' + 10
                                             : c - '0');
      }

      p += 5;
      if (cp < 0x80) {
        o[n++] = (char)cp;
      } else if (cp < 0x800) {
        o[n++] = (char)(0xc0 | (cp >> 6));
        o[n++] = (char)(0x80 | (cp & 0x3f));
      } else {
        o[n++] = (char)(0xe0 | (cp >> 12));
        o[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[n++] = (char)(0x80 | (cp & 0x3f));
      }

      break;
    }

    default: { // \" \\ \/
      o[n++] = *p++;
    }
    }
  }

  if (*p != '"') {
    free(o);

    return NULL;
  }

  o[n] = 0;
  *pp = p + 1;

  return o;
}

// One asciicast event line: [time, "kind", "text"]
static int parse_event(const char *line, double *t, char *kind, char **data) {
  const char *p = line;
  if (*p != '[') {
    return 0;
  }

  char *end;
  *t = strtod(p + 1, &end);
  if (end == p + 1) {
    return 0;
  }

  p = end;
  while (*p == ',' || *p == ' ') {
    p++;
  }

  char *k = json_string(&p);
  if (!k) {
    return 0;
  }

  snprintf(kind, 8, "%s", k);
  free(k);
  while (*p == ',' || *p == ' ') {
    p++;
  }

  *data = json_string(&p);

  return *data != NULL;
}

/* Recorder, screenshots, unwritable folders */

static void capture_tests(void) {
  const args_t ARGS = A("-d", "fake", "-W", "40", "-H", "20", "-r", "20", NULL);

  // Options cut out of argv (--net) must not hide environment
  char root[] = "/tmp/asciicam_net_XXXXXX";
  if (!mkdtemp(root)) {
    check(0, "capture: cannot make a scratch directory");

    return;
  }

  set_home(root);
  step_t one[] = {S("s", 0.8)};
  sess_t se = session(cat(ARGS, A("--net", "100", NULL)), one, 1, 0.6);
  set_home(REAL_HOME);
  free(se.out.data);
  char net_dir[300];
  snprintf(net_dir, sizeof net_dir, "%s/Pictures/asciicam", root);
  names_t nl = list_dir(net_dir);
  int any_png = 0;
  for (int i = 0; i < nl.n; i++) {
    any_png |= ends_with(nl.name[i], ".png");
  }

  check(is_dir(net_dir) && any_png,
        "--net keeps HOME: screenshot saved under ~/Pictures/asciicam");
  rm_rf(root);

  static const char *labels[] = {"colour", "mono"};
  static const char *extras[] = {"-C", NULL};
  for (int v = 0; v < 2; v++) {
    const char *label = labels[v];
    int colour = v == 0;
    // Captures go to ~/Pictures/asciicam: HOME points at a scratch directory so
    // a test run never writes into real one
    char cap_root[] = "/tmp/asciicam_cap_XXXXXX";
    if (!mkdtemp(cap_root)) {
      check(0, "recorder %s: cannot make a scratch directory", label);

      continue;
    }

    set_home(cap_root);
    step_t rec[] = {S("t", 0.9),  // start recording
                    S("s", 0.5),  // screenshot while recording
                    S("t", 0.5)}; // stop
    sess_t s = session(plus(ARGS, extras[v]), rec, 3, 0.6);
    set_home(REAL_HOME);
    const unsigned char *out = s.out.data;
    size_t on = s.out.len;

    char cap_dir[300];
    snprintf(cap_dir, sizeof cap_dir, "%s/Pictures/asciicam", cap_root);
    names_t files = list_dir(cap_dir);
    int ncast = 0, npng = 0, nansi = 0;
    char cast_name[256] = "", png_name[256] = "", ansi_name[256] = "";
    char listing[600] = "";
    for (int i = 0; i < files.n; i++) {
      if (ends_with(files.name[i], ".cast") && !ncast++) {
        snprintf(cast_name, sizeof cast_name, "%s", files.name[i]);
      }

      if (ends_with(files.name[i], ".png") && !npng++) {
        snprintf(png_name, sizeof png_name, "%s", files.name[i]);
      }

      if (ends_with(files.name[i], ".ansi") && !nansi++) {
        snprintf(ansi_name, sizeof ansi_name, "%s", files.name[i]);
      }

      size_t used = strlen(listing);
      snprintf(listing + used, sizeof listing - used, "%s%s", i ? ", " : "",
               files.name[i]);
    }

    check(ncast == 1 && npng == 1 && nansi == 1,
          "recorder %s: one .cast, .png, .ansi saved (%s)", label, listing);
    check(contains(out, on, "REC started"), "recorder %s: toast on start",
          label);
    check(contains(out, on, "REC stopped") && contains(out, on, "saved"),
          "recorder %s: toasts on stop and screenshot", label);
    check(contains(out, on, "asciicam-"),
          "recorder %s: toast names the saved file", label);
    check(contains(out, on, " REC 0:0"),
          "recorder %s: REC clock drawn in panel", label);
    if (colour) {
      check(contains(out, on, "\x1b[1;38;2;255;40;40m\xe2\x97\x8f"),
            "recorder colour: red dot");
    } else {
      check(contains(out, on, "(o) REC"), "recorder mono: (o) dot");
    }

    if (ncast) {
      char path[600];
      snprintf(path, sizeof path, "%s/%s", cap_dir, cast_name);
      char *text = read_text(path, NULL);
      long ver = 0, wd = 0, ht = 0;
      int nev = 0, all_o = 1, sorted = 1, any_esc = 0, any_rec = 0;
      double prev = -1, last = -1;
      if (text) {
        json_int(text, "version", &ver);
        json_int(text, "width", &wd);
        json_int(text, "height", &ht);
        char *line = strchr(text, '\n'); // End of header line
        while (line) {
          char *start = line + 1; // Events start on second line
          char *nl2 = strchr(start, '\n');
          if (nl2) {
            *nl2 = 0;
          }

          double t;
          char kind[8];
          char *data;
          if (*start && parse_event(start, &t, kind, &data)) {
            nev++;
            all_o &= !strcmp(kind, "o");
            sorted &= t >= prev;
            prev = last = t;
            any_esc |= strstr(data, "\x1b[") != NULL;
            any_rec |= strstr(data, "REC") != NULL;
            free(data);
          }

          line = nl2;
        }
      }

      check(ver == 2 && wd == 120 && ht == 50,
            "recorder %s: cast header version=%ld width=%ld height=%ld", label,
            ver, wd, ht);
      check(nev >= 8 && all_o, "recorder %s: %d output events", label, nev);
      check(sorted && last > 0.4 && last < 2.0,
            "recorder %s: event times rise, last %.6f", label, last);
      check(any_esc, "recorder %s: frames hold the escapes", label);
      check(!any_rec, "recorder %s: panel/REC clock not part of recording",
            label);
      free(text);
    }

    if (npng) {
      char path[600];
      snprintf(path, sizeof path, "%s/%s", cap_dir, png_name);
      mz_png_info info;
      int ok = mz_png_read(path, &info);
      char desc[48];
      if (ok) {
        snprintf(desc, sizeof desc, "(%u, %u, %d)", info.w, info.h,
                 info.channels);
      } else {
        snprintf(desc, sizeof desc, "None");
      }

      // Camera size, not -W/-H
      check(ok && info.w == 640 && info.h == 480 &&
                info.channels == (colour ? 3 : 1),
            "recorder %s: png decodes, %s", label, desc);
    }

    if (nansi) {
      char path[600];
      size_t n = 0;
      snprintf(path, sizeof path, "%s/%s", cap_dir, ansi_name);
      char *txt = read_text(path, &n);
      check(txt && starts_with_str(txt, n, "\x1b[2J\x1b[H") && n > 200,
            "recorder %s: .ansi holds the frame", label);
      free(txt);
    }

    free(s.out.data);
    rm_rf(cap_root);
  }

  // Unwritable captures folder (and an unwritable working directory, which is
  // last-resort fallback): a message, no crash
  set_home("/proc/nope");
  if (chdir("/proc") != 0) {
    fprintf(stderr, "cannot chdir to /proc\n");
  }

  step_t bad[] = {S("t", 0.5), S("s", 0.5)};
  se = session(ARGS, bad, 2, 0.6);
  if (chdir(START_DIR) != 0) {
    fprintf(stderr, "cannot go back to %s\n", START_DIR);
  }

  set_home(REAL_HOME);
  check(contains(se.out.data, se.out.len, "cannot record") &&
            contains(se.out.data, se.out.len, "cannot save"),
        "recorder: unwritable folder gives messages, program keeps running");
  check(pty_buf_count(&se.out, "\x1b[?2026h") > 10,
        "recorder: still drawing after failures");
  free(se.out.data);
}

// Sections named after second argument; none named = all
static int want(int argc, char **argv, const char *name) {
  if (argc <= 3) {
    return 1;
  }

  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], name)) {
      return 1;
    }
  }

  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <asciicam-fake-binary> [asciicam-binary]\n",
            argv[0]);

    return 2;
  }

  BIN = realpath(argv[1], NULL);
  if (!BIN || access(BIN, X_OK) != 0) {
    fprintf(stderr, "e2e: %s is not an executable (build it with `./nb e2e`)\n",
            argv[1]);

    return 2;
  }

  if (!getcwd(START_DIR, sizeof START_DIR)) {
    START_DIR[0] = 0;
  }

  snprintf(REAL_HOME, sizeof REAL_HOME, "%s",
           getenv("HOME") ? getenv("HOME") : "");

  if (want(argc, argv, "screen")) {
    screen_tests();
  }

  if (want(argc, argv, "smoke")) {
    smoke();
  }

  if (want(argc, argv, "stalled")) {
    stalled_camera();
  }

  if (want(argc, argv, "mouse")) {
    mouse_tests();
  }

  if (want(argc, argv, "capture")) {
    capture_tests();
  }

  if (argc > 2 && want(argc, argv, "real")) {
    real_binary(argv[2], 0);
  } else if (argc > 2 && want(argc, argv, "serve")) {
    real_binary(argv[2], 1); // Only --serve / --watch part of "real"
  }

  if (failures) {
    printf("e2e (C): %d FAILED\n", failures);

    return 1;
  }

  printf("e2e (C): all passed\n");

  return 0;
}
