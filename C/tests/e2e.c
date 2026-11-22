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

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *BIN; // Program with software camera
static int failures;

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

static void real_binary(const char *real) {
  char *abs = realpath(real, NULL);
  if (!abs || access(abs, X_OK) != 0) {
    return; // No real binary, nothing to check
  }

  const char *saved = BIN;
  BIN = abs;
  real_demo_and_plugins(abs);
  real_net_and_vcam();
  real_play();

  // A missing camera explains how to get a picture
  run_t r = run(A("-d", "/dev/definitely-not-a-camera", NULL), 0.3, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "--demo"),
        "missing camera: error mentions --demo");
  free(r.out.data);
  BIN = saved;
  free(abs);
}

/* Files, environment and little JSON recorder writes */

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
  size_t a = strlen(s), b = strlen(suffix);

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
    case 'n':
      o[n++] = '\n';
      p++;
      break;
    case 'r':
      o[n++] = '\r';
      p++;
      break;
    case 't':
      o[n++] = '\t';
      p++;
      break;
    case 'b':
      o[n++] = '\b';
      p++;
      break;
    case 'f':
      o[n++] = '\f';
      p++;
      break;
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

    default: // \" \\ \/
      o[n++] = *p++;
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
    real_binary(argv[2]);
  }

  if (failures) {
    printf("e2e (C): %d FAILED\n", failures);

    return 1;
  }

  printf("e2e (C): all passed\n");

  return 0;
}
