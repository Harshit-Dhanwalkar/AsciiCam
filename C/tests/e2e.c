/*
 * tests/e2e.c - end-to-end checks of main loop
 *
 *   cc -O1 -o build/tests/e2e tests/e2e.c
 *   build/tests/e2e build/tests/asciicam_fake [build/asciicam [section ...]]
 *
 * Runs program in a pseudo-terminal against software camera
 * (tests/fake_capture.c) and checks what it draws
 *
 * macOS has no software camera build: give real binary twice and name sections
 * that need no fake camera, `.nb e2e` does there:
 *   build/tests/e2e build/asciicam build/asciicam voice model real
 * (WARN: Windows has no pseudo-terminal, so no e2e)
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

#if defined(__linux__)
#include "../include/plugin_build.h" // PB_SCRIPT: How app builds a plugin
#endif

// Plugin file extension of this platform (build.c SO_EXT)
#if defined(__APPLE__)
#define E2E_SO_EXT "dylib"
#else
#define E2E_SO_EXT "so"
#endif

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

  // NOTE: Whatever program wrote last may still be waiting in terminal when
  // exit is noticed (read, then it writes and exits, then waitpid): take it
  while (pty_read(&p, &r.out, 10) > 0) {
    // Continue
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

  pty_finish(&p, 0.0); // Kills it if it is still there

  return s;
}

static char START_DIR[4096]; // Location from run began
static int write_file(const char *path, const void *d, size_t n);

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

// Mean (R - B), or with green_excess mean (G - max(R, B)), over foreground
// colour escapes ESC[38;2;R;G;Bm
// Returns 0 when there are none
static int colour_mean(const pty_buf *b, double *out, int green_excess) {
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
      int hi = v[0] > v[2] ? v[0] : v[2];
      sum += green_excess ? v[1] - hi : v[0] - v[2];
      count++;
    }
  }

  if (!count) {
    return 0;
  }

  *out = sum / (double)count;

  return 1;
}

static int warmth(const pty_buf *b, double *out) {
  return colour_mean(b, out, 0);
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
  // (plugin lookups are relative to binary: build/<name>.so, .dylib on macOS)
  static const char *plugins[] = {
      "chromatic", "crt",        "dither_bayer", "dither_fs",   "fisheye",
      "framediff", "glitch",     "kaleidoscope", "matrix_rain", "motion",
      "neon_edge", "pixel_sort", "ripple",       "sepia",       "thermal",
      "trails",    "tv_static",  "vhs",          "vignette",    "halftone",
      "sharpen",   "solarize",   "posterize",    "pixelate",    "emboss",
      "equalize",  "bloom",      "swirl",        "nightvision"};
  char dir[4096];
  snprintf(dir, sizeof dir, "%s", real_abs);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
  }

  for (size_t i = 0; i < sizeof plugins / sizeof plugins[0]; i++) {
    char so[4200];
    snprintf(so, sizeof so, "%s/%s." E2E_SO_EXT, dir, plugins[i]);
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
    snprintf(so, sizeof so, "%s/%s." E2E_SO_EXT, dir, hooks[i]);
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

  // Nightvision paints picture phosphor green: green clearly above red and blue
  {
    char so[4200];
    snprintf(so, sizeof so, "%s/nightvision." E2E_SO_EXT, dir);
    r = run(A("--demo", "-r", "20", "-C", "-p", so, NULL), 1.2, -1, "q");
    double g = 0;
    int have = colour_mean(&r.out, &g, 1);
    check(r.frames >= 8 && r.code == 0 && have && g > 15,
          "plugin nightvision paints colour pictures green (%s, %ld frames)",
          num_str(have, g), r.frames);
    free(r.out.data);
  }

  // --sandbox: plugins run in a locked-down child, picture and colour hooks
  // still work and several plugins can be stacked
  static const char *sb[] = {"glitch", "vhs", "ripple"};
  for (int i = 0; i < 3; i++) {
    char so[4200];
    snprintf(so, sizeof so, "%s/%s." E2E_SO_EXT, dir, sb[i]);
    r = run(A("--demo", "-r", "20", "--sandbox", "-p", so, NULL), 1.2, -1, "q");
    check(r.frames >= 8 && r.code == 0 &&
              !contains(r.out.data, r.out.len, "switched off"),
          "--sandbox plugin %s: loads and draws (%ld frames, exit %d)", sb[i],
          r.frames, r.code);
    free(r.out.data);
  }
  for (int i = 0; i < 2; i++) {
    char so[4200];
    snprintf(so, sizeof so, "%s/%s." E2E_SO_EXT, dir, hooks[i]);
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
    snprintf(so1, sizeof so1, "%s/glitch." E2E_SO_EXT, dir);
    snprintf(so2, sizeof so2, "%s/vignette." E2E_SO_EXT, dir);
    r = run(A("--demo", "-r", "20", "--sandbox", "-p", so1, "-p", so2, NULL),
            1.2, -1, "q");
    check(r.frames >= 8 && r.code == 0,
          "--sandbox with two plugins stacked (%ld frames, exit %d)", r.frames,
          r.code);
    free(r.out.data);
  }

#if defined(__linux__)
  // Every plugin under --sandbox, in colour mode so luma and colour hooks both
  // run in locked-down child: a plugin that needs a call sandbox forbids is
  // switched off with a "sandbox: <name> ..." message and picture stops
  // changing
  for (size_t i = 0; i < sizeof plugins / sizeof plugins[0]; i++) {
    char so[4200];
    snprintf(so, sizeof so, "%s/%s." E2E_SO_EXT, dir, plugins[i]);
    r = run(A("--sandbox", "--demo", "-r", "20", "-C", "-p", so, NULL), 1.2, -1,
            "q");
    check(r.frames >= 8 && r.code == 0 &&
              !contains(r.out.data, r.out.len, "sandbox: "),
          "plugin %s under --sandbox: keeps drawing (%ld frames, exit %d)",
          plugins[i], r.frames, r.code);
    free(r.out.data);
  }

  // A plugin that makes a call sandbox forbids is caught and switched off and
  // program carries on. Built here way app builds one on demand
  {
    char sdir[] = "/tmp/asciicam_bad_XXXXXX";
    if (mkdtemp(sdir)) {
      char src[300], bad[300], cmd[5200];
      snprintf(src, sizeof src, "%s/bad.c", sdir);
      snprintf(bad, sizeof bad, "%s/bad.so", sdir);
      // socket(2): in no allow list; written as raw system call because a
      // plugin has no errno of its own to link with
      static const char code[] =
          "#include \"nolibc.h\"\n#include \"fx.h\"\n#include "
          "\"plugins.h\"\n"
          "void bad_process(uint8_t *g, int w, int h, void *ctx) {\n"
          "  (void)g; (void)w; (void)h; (void)ctx;\n"
          "#if defined(__x86_64__)\n"
          "  long r;\n"
          "  __asm__ volatile(\"syscall\" : \"=a\"(r) : \"0\"(41L), \"D\"(2L), "
          "\"S\"(1L), \"d\"(0L) : \"rcx\", \"r11\", \"memory\");\n"
          "#elif defined(__aarch64__)\n"
          "  register long x0 __asm__(\"x0\") = 2;\n"
          "  register long x1 __asm__(\"x1\") = 1;\n"
          "  register long x2 __asm__(\"x2\") = 0;\n"
          "  register long x8 __asm__(\"x8\") = 198;\n"
          "  __asm__ volatile(\"svc 0\" : \"+r\"(x0) : \"r\"(x1), \"r\"(x2), "
          "\"r\"(x8) : \"memory\");\n"
          "#endif\n}\n"
          "static filter_plugin_t self = {.process = bad_process, .name = "
          "\"bad\"};\n"
          "filter_plugin_t *plugin_get(void) { return &self; }\n"
          "PLUGIN_ABI_EXPORT;\n";
      write_file(src, code, sizeof code - 1);
      snprintf(cmd, sizeof cmd, "sh -c '%s' sh '%s' '%s' '%s' 2>&1", PB_SCRIPT,
               src, bad, START_DIR);
      int built = system(cmd) == 0;
      check(built,
            "sandbox: test plugin builds with app's plugin build script");
      if (built) {
        r = run(A("--sandbox", "--demo", "-r", "20", "-C", "-p", bad, NULL),
                1.5, -1, "q");
        check(contains(r.out.data, r.out.len,
                       "sandbox: bad tried a system call that is not allowed"),
              "sandbox: forbidden system call is caught and named");
        check(r.frames >= 8 && r.code == 0,
              "sandbox: program carries on after plugin was stopped (%ld "
              "frames, exit %d)",
              r.frames, r.code);
        free(r.out.data);
      }

      rm_rf(sdir);
    }
  }

#endif

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
  int idx = 0;
  int code = -1;
  int done = 0;
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

  if (done) { // Last output can still be waiting in the terminal
    for (;;) {
      pty_buf rest = {0};
      long got = pty_read(&p, &rest, 10);
      if (rest.len) {
        chunks_add(ch, pty_now() - t0, rest.data, rest.len);
      }

      free(rest.data);
      if (got <= 0) {
        break;
      }
    }
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

static void real_image(void);
static void real_serve_and_watch(void);
static void real_captions_and_calls(void);

static void real_binary(const char *real, int only_serve) {
  char *abs = realpath(real, NULL);
  if (!abs || access(abs, X_OK) != 0) {
    return; // No real binary, nothing to check
  }

  const char *saved = BIN;
  BIN = abs;
  if (only_serve) {
    real_serve_and_watch();
    BIN = saved;
    free(abs);

    return;
  }

  real_demo_and_plugins(abs);
  real_net_and_vcam();
  real_play();
  real_image();
  real_serve_and_watch();
  real_captions_and_calls();

  // A missing camera explains how to get a picture
  run_t r = run(A("-d", "/dev/definitely-not-a-camera", NULL), 0.3, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "--demo"),
        "missing camera: error mentions --demo");
  free(r.out.data);
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
    step_t rec[] = {S("t", 0.9),  // Start recording
                    S("s", 0.5),  // Screenshot while recording
                    S("t", 0.5)}; // Stop
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
      long ver = 0;
      long wd = 0;
      long ht = 0;
      int nev = 0;
      int all_o = 1;
      int sorted = 1;
      int any_esc = 0;
      int any_rec = 0;
      double prev = -1;
      double last = -1;
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

/* Real binary: --image and slideshows */

typedef void (*pix_fn)(int x, int y, void *ctx, unsigned char rgb[3]);

static void pix_solid(int x, int y, void *ctx, unsigned char o[3]) {
  (void)x;
  (void)y;
  memcpy(o, ctx, 3);
}

static void pix_half(int x, int y, void *ctx, unsigned char o[3]) {
  (void)y;
  (void)ctx;
  o[0] = o[1] = o[2] = x >= 32 ? 255 : 0;
}

static void pix_red_blue(int x, int y, void *ctx, unsigned char o[3]) {
  (void)y;
  (void)ctx;
  static const unsigned char red[3] = {220, 30, 30};
  static const unsigned char blue[3] = {30, 30, 220};
  memcpy(o, x < 10 ? red : blue, 3);
}

static void put_be32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

static void png_chunk(FILE *f, const char *type, const unsigned char *d,
                      size_t n) {
  static const unsigned char none[1] = {0};
  if (!d) {
    d = none;
  }

  unsigned char hdr[8];
  put_be32(hdr, (uint32_t)n);
  memcpy(hdr + 4, type, 4);
  fwrite(hdr, 1, 8, f);
  fwrite(d, 1, n, f);
  uint32_t crc = mz_crc32((const unsigned char *)type, 4, 0);
  crc = mz_crc32(d, n, crc);
  unsigned char c[4];
  put_be32(c, crc);
  fwrite(c, 1, 4, f);
}

// 8-bit RGB PNG (stored deflate blocks, so no compressor is needed)
static int write_png(const char *path, int w, int h, pix_fn pix, void *ctx,
                     int interlace) {
  static const int A7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8},
                               {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2},
                               {0, 1, 1, 2}};
  size_t cap = (size_t)h * (1 + (size_t)w * 3) + 16, n = 0;
  unsigned char *raw = (unsigned char *)malloc(cap);
  if (!raw) {
    return 0;
  }

  for (int p = 0; p < (interlace ? 7 : 1); p++) {
    int x0 = interlace ? A7[p][0] : 0, y0 = interlace ? A7[p][1] : 0;
    int dx = interlace ? A7[p][2] : 1, dy = interlace ? A7[p][3] : 1;
    for (int y = y0; y < h; y += dy) {
      if (x0 >= w) {
        continue;
      }

      raw[n++] = 0; // Filter: none
      for (int x = x0; x < w; x += dx) {
        pix(x, y, ctx, raw + n);
        n += 3;
      }
    }
  }

  size_t zcap = n + n / 65535 * 5 + 64, zn = 0;
  unsigned char *z = (unsigned char *)malloc(zcap);
  if (!z) {
    free(raw);

    return 0;
  }

  z[zn++] = 0x78;
  z[zn++] = 0x01;
  for (size_t off = 0; off < n || off == 0;) {
    size_t len = n - off > 65535 ? 65535 : n - off;
    int last = off + len >= n;
    z[zn++] = (unsigned char)last;
    z[zn++] = (unsigned char)len;
    z[zn++] = (unsigned char)(len >> 8);
    z[zn++] = (unsigned char)~len;
    z[zn++] = (unsigned char)(~len >> 8);
    memcpy(z + zn, raw + off, len);
    zn += len;
    off += len;
    if (last) {
      break;
    }
  }

  put_be32(z + zn, mz_adler32(raw, n));
  zn += 4;
  FILE *f = fopen(path, "wb");
  if (f) {
    unsigned char ihdr[13];
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = (unsigned char)interlace;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
  }

  free(raw);
  free(z);

  return f != NULL;
}

static int write_file(const char *path, const void *d, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    return 0;
  }

  fwrite(d, 1, n, f);

  return fclose(f) == 0;
}

// Text of last whole frame with escape sequences removed, split in rows
typedef struct {
  char *text;
  const char *row[256];
  size_t len[256];
  int n;
} rows_t;

static rows_t frame_rows(const pty_buf *out) {
  static const char mark[] = "\x1b[?2026h";
  size_t ml = sizeof mark - 1;
  long last = rfind_mem(out->data, out->len, mark);
  long prev = -1;
  if (last >= 0) {
    prev = rfind_mem(out->data, (size_t)last, mark);
  }

  size_t from = 0, to = out->len;
  if (last >= 0 && prev >= 0) { // Two or more starts: whole frame between
    from = (size_t)prev + ml;
    to = (size_t)last;
  }

  rows_t r;
  memset(&r, 0, sizeof r);
  r.text = (char *)malloc(to - from + 1);
  size_t n = 0;
  for (size_t i = from; i < to;) {
    if (out->data[i] == 0x1b && i + 1 < to && out->data[i + 1] == '[') {
      size_t j = i + 2;
      while (j < to && (strchr("0123456789;?<", out->data[j]) != NULL) &&
             out->data[j]) {
        j++;
      }

      if (j < to && ((out->data[j] >= 'A' && out->data[j] <= 'Z') ||
                     (out->data[j] >= 'a' && out->data[j] <= 'z'))) {
        i = j + 1;

        continue;
      }
    }

    r.text[n++] = (char)out->data[i++];
  }

  r.text[n] = 0;
  size_t s = 0;
  for (size_t i = 0; i <= n && r.n < 256; i++) {
    if (i == n || r.text[i] == '\n') {
      size_t a = s, b = i;
      while (a < b && r.text[a] == '\r') {
        a++;
      }

      while (b > a && r.text[b - 1] == '\r') {
        b--;
      }

      if (b > a) {
        r.row[r.n] = r.text + a;
        r.len[r.n++] = b - a;
      }

      s = i + 1;
    }
  }

  return r;
}

static int row_has(const rows_t *r, int i, char c) {
  return memchr(r->row[i], c, r->len[i]) != NULL;
}

static int row_count(const rows_t *r, int i, char c) {
  int k = 0;
  for (size_t j = 0; j < r->len[i]; j++) {
    k += r->row[i][j] == c;
  }

  return k;
}

static int row_index(const rows_t *r, int i, char c) {
  const char *p = (const char *)memchr(r->row[i], c, r->len[i]);

  return p ? (int)(p - r->row[i]) : -1;
}

// Foreground colours ESC[38;2;R;G;Bm in seg: calls back for each
typedef struct {
  int r;
  int g;
  int b;
} rgb_t;

static int fg_colours(const unsigned char *d, size_t n, rgb_t *out, int cap) {
  static const char pre[] = "\x1b[38;2;";
  size_t pl = sizeof pre - 1;
  int k = 0;
  for (size_t i = 0; i + pl < n && k < cap; i++) {
    if (memcmp(d + i, pre, pl)) {
      continue;
    }

    size_t j = i + pl;
    int v[3] = {0, 0, 0}, ok = 1;
    for (int c = 0; c < 3 && ok; c++) {
      int nd = 0;
      while (j < n && d[j] >= '0' && d[j] <= '9') {
        v[c] = v[c] * 10 + (d[j++] - '0');
        nd++;
      }

      ok = nd > 0 && j < n && d[j] == (c < 2 ? ';' : 'm');
      j++;
    }

    if (ok) {
      out[k].r = v[0];
      out[k].g = v[1];
      out[k].b = v[2];
      k++;
    }
  }

  return k;
}

// 'r', 'g' or 'b': strongest channel of colours in seg, 0 for none
static char dominant(const unsigned char *d, size_t n) {
  static rgb_t cs[20000];
  int k = fg_colours(d, n, cs, 20000);
  if (!k) {
    return 0;
  }

  long t[3] = {0, 0, 0};
  for (int i = 0; i < k; i++) {
    t[0] += cs[i].r;
    t[1] += cs[i].g;
    t[2] += cs[i].b;
  }

  return t[0] >= t[1] && t[0] >= t[2] ? 'r' : t[1] >= t[2] ? 'g' : 'b';
}

static void real_image(void) {
  char idir[] = "/tmp/asciicam_img_XXXXXX";
  if (!mkdtemp(idir)) {
    check(0, "--image: cannot make a scratch directory");

    return;
  }

  static const char *IARGS[] = {"-r", "20", "-m", "ascii", "-s", " .:-=+*#%@",
                                "-W", "60", "-H", "40",    NULL};
  args_t ia = A(IARGS[0], IARGS[1], IARGS[2], IARGS[3], IARGS[4], IARGS[5],
                IARGS[6], IARGS[7], IARGS[8], IARGS[9], NULL);
  args_t ic = A(IARGS[0], IARGS[1], IARGS[2], IARGS[3], IARGS[4], IARGS[5],
                NULL); // Without -W -H

  char white[300];
  char pgm[300];
  char half[300];
  char wide[300];
  char jpg[300];
  char inter[300];
  snprintf(white, sizeof white, "%s/white.png", idir);
  snprintf(pgm, sizeof pgm, "%s/black.pgm", idir);
  snprintf(half, sizeof half, "%s/half.png", idir);
  snprintf(wide, sizeof wide, "%s/wide.png", idir);
  snprintf(jpg, sizeof jpg, "%s/x.jpg", idir);
  snprintf(inter, sizeof inter, "%s/adam7.png", idir);
  unsigned char w3[3] = {255, 255, 255};
  write_png(white, 64, 64, pix_solid, w3, 0);

  run_t r = run(cat(A("--image", white, NULL), ia), 1.0, -1, "q");
  check(r.frames >= 10 && r.code == 0 && contains(r.out.data, r.out.len, "@"),
        "--image PNG: drawn (%ld frames, exit %d)", r.frames, r.code);
  free(r.out.data);

  char spec[320];
  snprintf(spec, sizeof spec, "image:%s", white);
  r = run(cat(A("-d", spec, NULL), ia), 1.0, -1, "q");
  check(r.code == 0 && contains(r.out.data, r.out.len, "@"),
        "-d image:<file> is the same thing");
  free(r.out.data);

  unsigned char p5[11 + 64];
  memcpy(p5, "P5\n8 8\n255\n", 11);
  memset(p5 + 11, 0, 64);
  write_file(pgm, p5, sizeof p5);
  r = run(cat(A("--image", pgm, NULL), ia), 1.0, -1, "q");
  check(r.code == 0 && !contains(r.out.data, r.out.len, "@"),
        "--image PGM, all black: no bright cells");
  free(r.out.data);

  write_png(half, 64, 64, pix_half, NULL, 0);
  r = run(cat(A("--image", half, NULL), ia), 1.0, -1, "q");
  rows_t fr = frame_rows(&r.out);
  int bright = 0, min_first = 1 << 30, all_wide = 1;
  for (int i = 0; i < fr.n; i++) {
    if (row_has(&fr, i, '@')) {
      bright++;
      int f = row_index(&fr, i, '@');
      min_first = f < min_first ? f : min_first;
      all_wide &= row_count(&fr, i, '@') > 3;
    }
  }

  check(bright && min_first > 5 && all_wide,
        "left half black, right half white: bright cells only on the right");
  free(fr.text);
  free(r.out.data);

  write_png(wide, 200, 20, pix_solid, w3, 0);
  r = run(cat(A("--image", wide, NULL), ia), 1.0, -1, "q");
  fr = frame_rows(&r.out);
  bright = 0;
  for (int i = 0; i < fr.n; i++) {
    bright += row_has(&fr, i, '@');
  }

  check(r.code == 0 && bright && bright < fr.n - 4,
        "wide picture keeps its proportions: bars above and below (%d of %d "
        "rows)",
        bright, fr.n);
  free(fr.text);
  free(r.out.data);

  // Colour map works on a picture
  r = run(cat(A("--image", half, "-C", NULL), ic), 1.0, -1, "q");
  check(r.code == 0 && contains(r.out.data, r.out.len, "\x1b[38;2;"),
        "--image with -C: colour");
  free(r.out.data);

  // Errors say what is wrong
  char nope[300];
  char text[300];
  snprintf(nope, sizeof nope, "%s/nope.png", idir);
  snprintf(text, sizeof text, "%s/notes.txt", idir);
  write_file(text, "this is not a picture\n", 22);
  r = run(cat(A("--image", nope, NULL), ia), 0.5, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "cannot open"),
        "--image missing file: cannot open");
  free(r.out.data);
  r = run(cat(A("--image", text, NULL), ia), 0.5, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "not a PNG"),
        "--image not a picture: not a PNG");
  free(r.out.data);
  unsigned char bad_jpg[104] = {0xff, 0xd8, 0xff, 0xe0};
  write_file(jpg, bad_jpg, sizeof bad_jpg);
  r = run(cat(A("--image", jpg, NULL), ia), 0.5, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "JPEG"),
        "--image damaged JPEG: says so");
  free(r.out.data);

  write_png(inter, 20, 20, pix_red_blue, NULL, 1);
  r = run(cat(A("--image", inter, "-C", NULL), ic), 1.0, -1, "q");
  static rgb_t cols[20000];
  int nc = fg_colours(r.out.data, r.out.len, cols, 20000);
  int has_red = 0, has_blue = 0;
  for (int i = 0; i < nc; i++) {
    has_red |= cols[i].r > 150 && cols[i].b < 100;
    has_blue |= cols[i].b > 150 && cols[i].r < 100;
  }

  check(r.code == 0 && has_red && has_blue,
        "--image Adam7 PNG: both halves arrive in the right colours");
  free(r.out.data);

  // A folder is a slideshow: . and , step, a broken picture is skipped
  char sdir[300];
  snprintf(sdir, sizeof sdir, "%s/show", idir);
  mkdir(sdir, 0755);
  static const char *names[] = {"a.png", "b.png", "c.png"};
  unsigned char shades[3][3] = {{230, 30, 30}, {30, 230, 30}, {30, 30, 230}};
  for (int i = 0; i < 3; i++) {
    char p[400];
    snprintf(p, sizeof p, "%s/%s", sdir, names[i]);
    write_png(p, 16, 16, pix_solid, shades[i], 0);
  }

  char p[400];
  snprintf(p, sizeof p, "%s/d.png", sdir);
  write_file(p, "not a picture", 13);
  snprintf(p, sizeof p, "%s/notes.txt", sdir);
  write_file(p, "x", 1);

  step_t steps[4] = {S(".", 0.7), S(".", 0.7), S(".", 0.7), S(",", 0.7)};
  sess_t s = session(cat(A("--image", sdir, "--slide", "0", "-C", NULL), ic),
                     steps, 4, 0.6);
  char seq[8] = {0};
  for (int i = 0; i < 5 && i < s.nmarks; i++) {
    size_t a = i == 0 ? 0 : s.marks[i - 1];
    size_t b = s.marks[i];
    seq[i] = dominant(s.out.data + a, b - a);
  }

  check(!strcmp(seq, "rgbrb"),
        "slideshow: start, . . . (broken one skipped), , -> "
        "['%c', '%c', '%c', '%c', '%c']",
        seq[0] ? seq[0] : '?', seq[1] ? seq[1] : '?', seq[2] ? seq[2] : '?',
        seq[3] ? seq[3] : '?', seq[4] ? seq[4] : '?');
  check(s.nmarks > 1 &&
            contains(s.out.data + s.marks[0], s.marks[1] - s.marks[0], "2/4") &&
            contains(s.out.data + s.marks[0], s.marks[1] - s.marks[0], "b.png"),
        "slideshow: a toast names the picture");
  check(!contains(s.out.data, s.out.len, "d.png"),
        "slideshow: the broken picture is never announced");
  free(s.out.data);

  r = run(cat(A("--image", sdir, "--slide", "1", "-C", NULL), ic), 3.2, -1,
          "q");
  int seen[3] = {0, 0, 0};
  {
    static const char mark[] = "\x1b[?2026h";
    size_t ml = sizeof mark - 1;
    size_t starts[4096];
    int ns = 0;
    for (size_t i = 0; i + ml <= r.out.len && ns < 4096; i++) {
      if (!memcmp(r.out.data + i, mark, ml)) {
        starts[ns++] = i + ml;
      }
    }

    for (int i = 0; i < ns; i++) {
      size_t e = i + 1 < ns ? starts[i + 1] - ml : r.out.len;
      char d = dominant(r.out.data + starts[i], e - starts[i]);
      if (d) {
        seen[d == 'r' ? 0 : d == 'g' ? 1 : 2] = 1;
      }
    }
  }

  check(r.code == 0 && seen[0] + seen[1] + seen[2] >= 3,
        "slideshow: moves on by itself, --slide 1 (%s%s%s)", seen[0] ? "r" : "",
        seen[1] ? "g" : "", seen[2] ? "b" : "");
  free(r.out.data);

  char empty[300];
  snprintf(empty, sizeof empty, "%s/empty", idir);
  mkdir(empty, 0755);
  r = run(cat(A("--image", empty, NULL), ia), 0.5, -1, "q");
  check(r.code != 0 && contains(r.out.data, r.out.len, "no pictures"),
        "slideshow: empty folder says so");
  free(r.out.data);
  r = run(cat(A("--image", sdir, "--slide", "x", NULL), ia), 0.5, -1, "q");
  check(r.code == 2 && contains(r.out.data, r.out.len, "--slide needs"),
        "--slide with a bad number: usage error");
  free(r.out.data);
  rm_rf(idir);
}

/* Real binary: --serve and --watch over TCP
 * Python script used threads for other end of connection; here a forked child
 * plays that part and hands what it got back through a file */

static int free_port(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t l = sizeof a;
  int port = 0;
  if (s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 &&
      getsockname(s, (struct sockaddr *)&a, &l) == 0) {
    port = ntohs(a.sin_port);
  }

  if (s >= 0) {
    close(s);
  }

  return port;
}

static int listen_on(int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 1) != 0) {
    close(s);

    return -1;
  }

  return s;
}

static int connect_to(int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
    return s;
  }

  if (s >= 0) {
    close(s);
  }

  return -1;
}

// Child: connects after `delay` seconds (retrying while nobody listens yet),
// reads for `secs` seconds or until server closes, writes it to outpath
static pid_t spawn_viewer(int port, double delay, double secs,
                          const char *outpath) {
  pid_t pid = fork();
  if (pid != 0) {
    return pid;
  }

  usleep((useconds_t)(delay * 1e6));
  int fd = -1;
  for (int i = 0; i < 40 && fd < 0; i++) {
    fd = connect_to(port);
    if (fd < 0) {
      usleep(100000);
    }
  }

  FILE *f = fopen(outpath, "wb");
  if (fd >= 0 && f) {
    double end = pty_now() + secs;
    while (pty_now() < end) {
      struct pollfd pf = {fd, POLLIN, 0};
      if (poll(&pf, 1, 300) <= 0) {
        continue;
      }

      unsigned char tmp[65536];
      ssize_t n = read(fd, tmp, sizeof tmp);
      if (n <= 0) {
        break;
      }

      fwrite(tmp, 1, (size_t)n, f);
    }
  }

  if (fd >= 0) {
    close(fd);
  }

  if (f) {
    fclose(f);
  }

  _exit(0);
}

// Child: accepts one connection on lfd, sends msg, holds it open for `hold`
// seconds
static pid_t spawn_server_once(int lfd, const char *msg, double hold) {
  pid_t pid = fork();
  if (pid != 0) {
    return pid;
  }

  int c = accept(lfd, NULL, NULL);
  if (c >= 0) {
    ssize_t w = write(c, msg, strlen(msg));
    (void)w; // Peer gone is fine

    usleep((useconds_t)(hold * 1e6));
    close(c);
  }

  _exit(0);
}

// Child: serves BEFORE-RESTART on lfd, closes, listens on nobody for 2.5 s
// (viewer has to retry), then listens again and serves AFTER-RESTART
static pid_t spawn_restarting_server(int lfd, int port) {
  pid_t pid = fork();
  if (pid != 0) {
    return pid;
  }

  static const char *words[2] = {"\x1b[2JBEFORE-RESTART",
                                 "\x1b[2JAFTER-RESTART"};
  for (int n = 0; n < 2; n++) {
    int l = n == 0 ? lfd : listen_on(port);
    struct pollfd pf = {l, POLLIN, 0};
    if (l < 0 || poll(&pf, 1, 8000) <= 0) {
      if (l >= 0) {
        close(l);
      }

      _exit(0);
    }

    int c = accept(l, NULL, NULL);
    if (c >= 0) {
      ssize_t w = write(c, words[n], strlen(words[n]));
      (void)w;

      usleep(500000);
      close(c);
    }

    close(l);
    usleep(2500000);
  }

  _exit(0);
}

static pty_buf slurp(const char *path) {
  pty_buf b = {0};
  size_t n = 0;
  char *t = read_text(path, &n);
  if (t) {
    pty_buf_add(&b, t, n);
    free(t);
  }

  return b;
}

static long find_from(const pty_buf *b, size_t from, const char *needle) {
  if (from > b->len) {
    return -1;
  }

  const void *p = memmem(b->data + from, b->len - from, needle, strlen(needle));

  return p ? (long)((const unsigned char *)p - b->data) : -1;
}

static void real_serve_and_watch(void) {
  char dir[] = "/tmp/asciicam_serve_XXXXXX";
  if (!mkdtemp(dir)) {
    check(0, "--serve: cannot make a scratch directory");

    return;
  }

  char pic[300];
  char f1[300];
  char f2[300];
  snprintf(pic, sizeof pic, "%s/a.png", dir);
  snprintf(f1, sizeof f1, "%s/first.out", dir);
  snprintf(f2, sizeof f2, "%s/late.out", dir);
  unsigned char red[3] = {230, 30, 30};
  write_png(pic, 16, 16, pix_solid, red, 0);
  args_t ic = A("-r", "20", "-m", "ascii", "-s", " .:-=+*#%@", NULL);
  args_t ia = cat(ic, A("-W", "60", "-H", "40", NULL));
  char portstr[16];

  // --serve: a viewer on TCP gets a clear-screen, then whole frames
  int port = free_port();
  snprintf(portstr, sizeof portstr, "%d", port);
  pid_t v = spawn_viewer(port, 0.0, 1.5, f1);
  run_t r = run(cat(A("--image", pic, "-C", "--serve", portstr, NULL), ic), 3.0,
                -1, "q");
  waitpid(v, NULL, 0);
  pty_buf got = slurp(f1);
  size_t nh = pty_buf_count(&got, "\x1b[?2026h");
  size_t nl = pty_buf_count(&got, "\x1b[?2026l");
  check(r.code == 0 && got.len >= 12 &&
            !memcmp(got.data, "\x1b[0m\x1b[?25l\x1b[2J", 12),
        "--serve: viewer gets a clear-screen first");
  check(nh >= 5 && nh == nl,
        "--serve: frames arrive whole, one sync pair each");
  check(contains(got.data, got.len, "\x1b[38;2;"),
        "--serve: the colours go to the viewer too");
  check(contains(r.out.data, r.out.len, "viewer connected"),
        "--serve: the host sees a toast when a viewer joins");
  free(got.data);
  free(r.out.data);

  // A viewer who comes late is brought up to date, and later frames are only
  // changes: a still picture costs one whole picture, not one per frame
  port = free_port();
  snprintf(portstr, sizeof portstr, "%d", port);
  pid_t v1 = spawn_viewer(port, 0.0, 2.2, f1);
  pid_t v2 = spawn_viewer(port, 1.0, 1.4, f2);
  r = run(cat(A("--image", pic, "-C", "--serve", portstr, NULL), ic), 3.5, -1,
          "q");
  waitpid(v1, NULL, 0);
  waitpid(v2, NULL, 0);
  free(r.out.data);
  pty_buf one = slurp(f1), two = slurp(f2);
  long end1 = find_from(&one, 100, "\x1b[?2026l");
  check(one.len > 1000 && (long)one.len < 6 * end1 + 4000,
        "--serve: a still picture is sent once, then only changes (%zu bytes)",
        one.len);
  check(contains(two.data, two.len, "\x1b[38;2;") &&
            pty_buf_count(&two, "\x1b[?2026h") >= 2,
        "--serve: a viewer that comes late gets the whole picture");
  free(one.data);
  free(two.data);

  r = run(cat(A("--serve", "x", NULL), ia), 0.5, -1, "q");
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
  int lfd = listen_on(port);
  snprintf(portstr, sizeof portstr, "127.0.0.1:%d", port);
  pid_t s1 = spawn_server_once(lfd, "\x1b[2JHELLO-WATCH-FRAME", 0.4);
  r = run(A("--watch", portstr, NULL), 3.0, -1, "q");
  waitpid(s1, NULL, 0);
  close(lfd);
  check(contains(r.out.data, r.out.len, "HELLO-WATCH-FRAME"),
        "--watch: shows what the server sends");
  check(r.code == 1 && contains(r.out.data, r.out.len, "closed the connection"),
        "--watch: says so when the server goes away");
  free(r.out.data);

  // --watch --retry: server restarts, viewer comes back
  // Listening before viewer starts: binding in child raced with viewer's first
  // connect, which is refused (exit 1) on a busy machine
  port = free_port();
  lfd = listen_on(port);
  snprintf(portstr, sizeof portstr, "127.0.0.1:%d", port);
  pid_t s2 = spawn_restarting_server(lfd, port);
  close(lfd);
  r = run(A("--watch", portstr, "--retry", NULL), 7.0, -1, "q");
  waitpid(s2, NULL, 0);
  check(contains(r.out.data, r.out.len, "BEFORE-RESTART") &&
            contains(r.out.data, r.out.len, "reconnecting") &&
            contains(r.out.data, r.out.len, "AFTER-RESTART"),
        "--watch --retry: reconnects after the server restarts");
  check(r.code == 0, "--watch --retry: q still quits (%d)", r.code);
  long off = rfind_mem(r.out.data, r.out.len, "\x1b[?7l");
  long on = rfind_mem(r.out.data, r.out.len, "\x1b[?7h");
  check(off >= 0 && on > off,
        "--watch: lines are cut at the edge of a small terminal, wrap is put "
        "back on leaving");
  free(r.out.data);
  rm_rf(dir);
}

/* Real binary: captions, --room and --call */

// drive(): run a program and perform timed actions, then quit it with q
enum { ACT_KEY, ACT_FD, ACT_PATH };

typedef struct {
  double at; // Seconds after start
  int kind;  // ACT_KEY: to program's terminal, ACT_FD: to fd, ACT_PATH: to a
             // fifo opened non-blocking (skipped when nobody reads it)
  int fd;
  const char *path;
  const char *data;
} act_t;

static int drive(args_t a, const act_t *acts, int nacts, double total,
                 const char *env_name, const char *env_val, pty_buf *out) {
  char *argv[50];
  argv[0] = (char *)BIN;
  for (int i = 0; i < a.n; i++) {
    argv[i + 1] = (char *)a.v[i];
  }

  argv[a.n + 1] = NULL;
  char *saved = NULL;
  if (env_name) {
    const char *old = getenv(env_name);
    saved = old ? strdup(old) : NULL;
    setenv(env_name, env_val, 1);
  }

  pty_proc p;
  int ok = pty_spawn(&p, BIN, argv, 50, 120);
  if (env_name) {
    if (saved) {
      setenv(env_name, saved, 1);
    } else {
      unsetenv(env_name);
    }

    free(saved);
  }

  if (ok != 0) {
    fprintf(stderr, "cannot start %s in a pty\n", BIN);
    exit(1);
  }

  double t0 = pty_now();
  int idx = 0;
  while (pty_now() - t0 < total) {
    while (idx < nacts && pty_now() - t0 >= acts[idx].at) {
      const act_t *x = &acts[idx++];
      if (x->kind == ACT_KEY) {
        pty_write(&p, x->data, strlen(x->data));
      } else if (x->kind == ACT_FD) {
        ssize_t w = write(x->fd, x->data, strlen(x->data));
        (void)w;
      } else {
        int fd = open(x->path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
          ssize_t w = write(fd, x->data, strlen(x->data));
          (void)w;
          close(fd);
        }
      }
    }

    if (pty_read(&p, out, 50) < 0) {
      break;
    }
  }

  pty_write(&p, "q", 1);
  double end = pty_now() + 3;
  while (pty_now() < end) {
    if (pty_read(&p, out, 100) < 0) {
      break;
    }
  }

  return pty_finish(&p, 5.0);
}

// Read from several programs at once for `secs`
static void pump_all(pty_proc **ps, pty_buf **bs, int n, double secs) {
  double end = pty_now() + secs;
  while (pty_now() < end) {
    struct pollfd pf[4];
    for (int i = 0; i < n; i++) {
      pf[i].fd = ps[i]->eof ? -1 : ps[i]->fd;
      pf[i].events = POLLIN;
      pf[i].revents = 0;
    }

    if (poll(pf, (nfds_t)n, 100) <= 0) {
      continue;
    }

    for (int i = 0; i < n; i++) {
      if (pf[i].revents) {
        pty_read(ps[i], bs[i], 0);
      }
    }
  }
}

static int spawn_call(pty_proc *p, args_t a) {
  char *argv[50];
  argv[0] = (char *)BIN;
  for (int i = 0; i < a.n; i++) {
    argv[i + 1] = (char *)a.v[i];
  }

  argv[a.n + 1] = NULL;

  return pty_spawn(p, BIN, argv, 50, 120);
}

typedef struct {
  pid_t pid;
  int err; // Read end of its stderr
} room_t;

static room_t spawn_room(int port) {
  room_t r = {-1, -1};
  char portstr[16];
  snprintf(portstr, sizeof portstr, "%d", port);
  int fds[2];
  if (pipe(fds) != 0) {
    return r;
  }

  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], 2);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, 1);
    }

    execl(BIN, BIN, "--room", portstr, (char *)NULL);
    _exit(127);
  }

  close(fds[1]);
  r.pid = pid;
  r.err = fds[0];

  return r;
}

// SIGTERM, then everything it said on stderr
static pty_buf stop_room(room_t r) {
  pty_buf b = {0};
  if (r.pid > 0) {
    kill(r.pid, SIGTERM);
  }

  unsigned char tmp[4096];
  double end = pty_now() + 5;
  while (pty_now() < end) {
    struct pollfd pf = {r.err, POLLIN, 0};
    if (poll(&pf, 1, 200) <= 0) {
      continue;
    }

    ssize_t n = read(r.err, tmp, sizeof tmp);
    if (n <= 0) {
      break;
    }

    pty_buf_add(&b, tmp, (size_t)n);
  }

  close(r.err);
  if (r.pid > 0) {
    waitpid(r.pid, NULL, 0);
  }

  return b;
}

static int has_range(const pty_buf *b, size_t from, size_t to,
                     const char *needle) {
  return to <= b->len && from <= to &&
         contains(b->data + from, to - from, needle);
}

static void real_captions_and_calls(void) {
  char dir[] = "/tmp/asciicam_cap_XXXXXX";
  if (!mkdtemp(dir)) {
    check(0, "captions: cannot make a scratch directory");

    return;
  }

  char pic[300];
  char ffifo[300];
  char sfifo[300];
  char rdir[300];
  char capfile[400];
  char f1[300];
  char capfifo[300];
  snprintf(pic, sizeof pic, "%s/a.png", dir);
  snprintf(ffifo, sizeof ffifo, "%s/cap", dir);
  snprintf(sfifo, sizeof sfifo, "%s/scap", dir);
  snprintf(rdir, sizeof rdir, "%s/run", dir);
  snprintf(capfile, sizeof capfile, "%s/asciicam-captions", rdir);
  snprintf(f1, sizeof f1, "%s/viewer.out", dir);
  snprintf(capfifo, sizeof capfifo, "%s/captions.fifo", dir);
  mkdir(rdir, 0700);
  unsigned char red[3] = {230, 30, 30};
  write_png(pic, 16, 16, pix_solid, red, 0);

  // Captions on local picture (no call): fifo given, y toggles, y makes one
  mkfifo(ffifo, 0600);
  int wf = open(ffifo, O_RDWR);
  act_t acts1[] = {{1.0, ACT_FD, wf, NULL, "~good mor\n"},
                   {1.5, ACT_FD, wf, NULL, "good morning\n"},
                   {2.5, ACT_KEY, -1, NULL, "y"},
                   {3.2, ACT_FD, wf, NULL, "hidden words\n"},
                   {4.0, ACT_KEY, -1, NULL, "y"},
                   {4.4, ACT_FD, wf, NULL, "shown again\n"}};
  pty_buf out = {0};
  int code = drive(A("--demo", "-C", "--captions", ffifo, NULL), acts1, 6, 5.5,
                   NULL, NULL, &out);
  check(code == 0, "--captions: runs and quits (%d)", code);
  check(contains(out.data, out.len, "good morning") &&
            contains(out.data, out.len, "\x1b[48;2;20;20;20m") &&
            contains(out.data, out.len, "\x1b[1m good morning"),
        "--captions: a final line is drawn as a bold bar over the picture");
  check(contains(out.data, out.len, "\x1b[2m good mor"),
        "--captions: the guess came first, dim");
  check(contains(out.data, out.len, "captions off") &&
            !contains(out.data, out.len, "hidden words"),
        "--captions: y hides them (lines still read, not shown)");
  check(contains(out.data, out.len, "captions on") &&
            contains(out.data, out.len, "shown again"),
        "--captions: y shows them again");
  free(out.data);
  close(wf);

  // Picture sent to --watch viewers carries caption bars
  int sport = free_port();
  char sp[16];
  snprintf(sp, sizeof sp, "%d", sport);
  mkfifo(sfifo, 0600);
  int swf = open(sfifo, O_RDWR);
  pid_t viewer = spawn_viewer(sport, 0.0, 4.5, f1);
  act_t acts2[] = {{1.5, ACT_FD, swf, NULL, "hello viewers\n"}};
  memset(&out, 0, sizeof out);
  drive(A("--demo", "-C", "--serve", sp, "--captions", sfifo, NULL), acts2, 1,
        4.0, NULL, NULL, &out);
  free(out.data);
  waitpid(viewer, NULL, 0);
  close(swf);
  pty_buf vgot = slurp(f1);
  check(contains(vgot.data, vgot.len, "hello viewers"),
        "--serve: the viewer's picture has the caption bars");
  free(vgot.data);

  // Key y with no source makes a fifo in runtime directory
  act_t acts3[] = {{1.0, ACT_KEY, -1, NULL, "y"},
                   {1.8, ACT_PATH, -1, capfile, "made on demand\n"}};
  memset(&out, 0, sizeof out);
  drive(A("--demo", NULL), acts3, 2, 3.5, "XDG_RUNTIME_DIR", rdir, &out);
  struct stat st;
  check(stat(capfile, &st) == 0 && S_ISFIFO(st.st_mode),
        "key y with no source: makes a fifo in the runtime directory");
  check(contains(out.data, out.len, "captions on: write lines to ") &&
            contains(out.data, out.len, "made on demand"),
        "key y with no source: toast names the fifo, lines written to it show "
        "up");
  free(out.data);

  run_t r = run(A("--demo", "--captions", "/no/such/dir/captions", NULL), 1.0,
                -1, "q");
  check(r.code == 1 &&
            contains(r.out.data, r.out.len, "--captions: cannot open"),
        "--captions: missing path: clear message");
  free(r.out.data);

  char addr[32];
  snprintf(addr, sizeof addr, "127.0.0.1:%d", free_port());
  r = run(A("--watch", addr, NULL), 1.0, -1, "q");
  check(r.code == 1 && contains(r.out.data, r.out.len, "cannot connect"),
        "--watch: nobody there: clear message");
  free(r.out.data);
  r = run(A("--watch", "nonsense", NULL), 1.0, -1, "q");
  check(r.code == 1 && contains(r.out.data, r.out.len, "cannot connect"),
        "--watch: bad address: clear message");
  free(r.out.data);

  // --room / --call: two people in a room see each other and chat
  int cport = free_port();
  char cp[16];
  snprintf(cp, sizeof cp, "%d", cport);
  mkfifo(capfifo, 0600);
  room_t room = spawn_room(cport);
  usleep(400000);
  pty_proc pa;
  pty_proc pb;
  spawn_call(&pa, A("--call", cp, "--name", "Alice", "--demo", "-C",
                    "--captions", capfifo, NULL));
  spawn_call(&pb, A("--call", cp, "--name", "Bob", "--demo", NULL));
  pty_proc *ps[2] = {&pa, &pb};
  pty_buf ba = {0};
  pty_buf bb = {0};
  pty_buf *bs[2] = {&ba, &bb};
  pump_all(ps, bs, 2, 2.0);
  size_t m0 = bb.len;
  size_t a0 = ba.len;
  (void)a0;
  pty_write(&pa, "hello bob\r", 10);
  pump_all(ps, bs, 2, 1.5);
  size_t m1 = bb.len, a1 = ba.len;
  check(has_range(&bb, m0, m1, "Alice: hello bob"),
        "--call: chat reaches the other person");
  check(has_range(&ba, a0, a1, "you: hello bob"),
        "--call: and shows as yours at home");
  check(has_range(&bb, 0, m1, "Alice") && has_range(&bb, 0, m1, "\x1b[38;2;"),
        "--call: peer name and its colour picture are on screen");

  pty_write(&pa, "\x16", 1); // Ctrl-V: Alice turns her camera off
  pump_all(ps, bs, 2, 2.0);
  size_t a2 = ba.len;
  check(has_range(&ba, a1, a2, "^V cam on"),
        "--call: Ctrl-V is shown in the title");
  check(has_range(&ba, a1, a2, "kB/s"), "--call: the title shows the traffic");

  // Bob's screen shows camera-off card in Alice's tile
  // Checked before Alice turns her camera back on, so card is still
  // on Bob's screen
  {
    screen_t scr;
    screen_init(&scr, 50, 120);
    screen_feed(&scr, bb.data, bb.len);
    char *t = screen_text(&scr);
    check(strstr(t, "camera off") != NULL,
          "--call: Bob sees the camera-off card in Alice's tile");
    free(t);
    screen_free(&scr);
  }

  pty_write(&pa, "\x16", 1);
  pump_all(ps, bs, 2, 1.0);
  size_t a3 = ba.len;
  check(has_range(&ba, a2, a3, "^V cam off"),
        "--call: Ctrl-V again turns the camera back on");

  // Captions: a recogniser writes lines into fifo, others read them
  size_t b3 = bb.len;
  int wfd = open(capfifo, O_RDWR);
  const char *lines = "~good morn\ngood morning bob\n";
  ssize_t w = write(wfd, lines, strlen(lines));
  (void)w;
  pump_all(ps, bs, 2, 1.5);
  size_t a4 = ba.len, b4 = bb.len;
  check(has_range(&ba, a3, a4, "you: good morning bob"),
        "--captions: own caption shows at home");
  check(has_range(&bb, b3, b4, "Alice: good morning bob"),
        "--captions: Bob reads Alice's caption");

  // On Bob's final screen dim partial guess is gone, replaced by final
  {
    screen_t scr;
    screen_init(&scr, 50, 120);
    screen_feed(&scr, bb.data, bb.len);
    char *t = screen_text(&scr);
    int ok = 1;
    for (char *row = strtok(t, "\n"); row; row = strtok(NULL, "\n")) {
      if (strstr(row, "good morn") && !strstr(row, "morning")) {
        ok = 0;
      }
    }

    check(ok, "--captions: the partial line was replaced by the final one");
    free(t);
    screen_free(&scr);
  }

  pty_write(&pb, "\x14", 1); // Ctrl-T: Bob hides captions
  pump_all(ps, bs, 2, 1.0);
  close(wfd);

  // Nobody reads screens now: leaving must not hang on a full tty
  pty_write(&pa, "\x03", 1);
  pty_write(&pb, "\x03", 1);
  int ca = pty_finish(&pa, 5.0);
  int cb = pty_finish(&pb, 5.0);
  check(ca == 0 && cb == 0, "--call: Ctrl-C leaves cleanly");
  free(ba.data);
  free(bb.data);
  pty_buf rerr = stop_room(room);
  check(contains(rerr.data, rerr.len, "2 in the room") &&
            contains(rerr.data, rerr.len, "0 in the room"),
        "--room: reports who is in");
  free(rerr.data);

  snprintf(cp, sizeof cp, "%d", free_port());
  r = run(A("--call", cp, "--demo", NULL), 1.0, -1, "q");
  check(r.code == 1 && contains(r.out.data, r.out.len, "cannot connect"),
        "--call: no room there: clear message");
  free(r.out.data);
  snprintf(cp, sizeof cp, "%d", cport);
  r = run(A("--call", cp, "--captions", "/no/such/fifo", "--demo", NULL), 1.0,
          -1, "q");
  check(r.code == 1 &&
            contains(r.out.data, r.out.len, "--captions: cannot open"),
        "--captions: missing file: clear message");
  free(r.out.data);
  r = run(A("--room", "x", NULL), 0.5, -1, "q");
  check(r.code == 2 && contains(r.out.data, r.out.len, "--room needs a port"),
        "--room with a bad address: usage error");
  free(r.out.data);
  r = run(A("--room", "7100", "--call", "7100", NULL), 0.5, -1, "q");
  check(r.code == 2 && contains(r.out.data, r.out.len, "two programs"),
        "--room with --call: usage error");
  free(r.out.data);

  // A still picture costs next to nothing: only changed cells are redrawn
  cport = free_port();
  snprintf(cp, sizeof cp, "%d", cport);
  room = spawn_room(cport);
  usleep(400000);
  args_t still = A("--image", pic, "-C", "-W", "30", "-H", "12", NULL);
  spawn_call(&pa, cat(A("--call", cp, "--name", "Alice", NULL), still));
  spawn_call(&pb, cat(A("--call", cp, "--name", "Bob", NULL), still));
  pty_buf wa = {0};
  pty_buf wb = {0};
  pty_buf *ws[2] = {&wa, &wb};
  pump_all(ps, ws, 2, 2.5);
  size_t wl = wa.len + wb.len;
  pump_all(ps, ws, 2, 2.0);
  double rate = (double)(wa.len + wb.len - wl) / 2.0;
  check(rate < 120000,
        "--call: a still picture costs %d bytes/s on two screens", (int)rate);
  pty_write(&pa, "\x03", 1);
  pty_write(&pb, "\x03", 1);
  pty_finish(&pa, 5.0);
  pty_finish(&pb, 5.0);
  free(wa.data);
  free(wb.data);
  rerr = stop_room(room);
  free(rerr.data);

  r = run(A("--image", NULL), 0.5, -1, "q");
  check(r.code == 2 && contains(r.out.data, r.out.len, "--image needs"),
        "--image without a file: usage error");
  free(r.out.data);
  rm_rf(dir);
}

/* Tone curve: --curve, Curve tab of side panel and its tab strip */

// Characters of last picture, escapes removed, as mean position in ramp (-1
// when there are none)
static double mean_level(const unsigned char *buf, size_t n) {
  static const char ramp[] = " .:-=+*#%@";
  static const char mark[] = "\x1b[?2026h";
  long i = rfind_mem(buf, n, mark);
  size_t from = i >= 0 ? (size_t)i : (n ? n - 1 : 0);
  size_t to = n;
  const void *e =
      i >= 0 ? memmem(buf + from, n - from, "\x1b[?2026l", 8) : NULL;
  if (e) {
    to = (size_t)((const unsigned char *)e - buf);
  }

  double sum = 0;
  long count = 0;
  for (size_t k = from; k < to;) {
    if (buf[k] == 0x1b && k + 1 < to && buf[k + 1] == '[') {
      size_t j = k + 2;
      while (j < to && strchr("0123456789;?", buf[j]) && buf[j]) {
        j++;
      }

      if (j < to && ((buf[j] >= 'A' && buf[j] <= 'Z') ||
                     (buf[j] >= 'a' && buf[j] <= 'z'))) {
        k = j + 1;

        continue;
      }
    }

    const char *p = buf[k] ? strchr(ramp, buf[k]) : NULL;
    if (p) {
      sum += (double)(p - ramp);
      count++;
    }

    k++;
  }

  return count ? sum / (double)count : -1;
}

static int range_has(const sess_t *s, int from, int to, const char *needle) {
  size_t a = s->marks[from];
  size_t b = to < 0 ? s->out.len : s->marks[to];

  return contains(s->out.data + a, b - a, needle);
}

static void curve_tests(void) {
  args_t cargs = A("-d", "fake", "-W", "40", "-H", "20", "-r", "20", "-m",
                   "ascii", "-s", " .:-=+*#%@", NULL);
  run_t plain = run(cargs, 1.2, -1, "q");
  run_t white =
      run(plus(plus(cargs, "--curve"), "0,255 255,255"), 1.2, -1, "q");
  run_t black = run(plus(plus(cargs, "--curve"), "0,0 255,0"), 1.2, -1, "q");
  run_t same = run(plus(plus(cargs, "--curve"), "0,0 255,255"), 1.2, -1, "q");
  check(plain.code == 0 && white.code == 0 && black.code == 0 && same.code == 0,
        "--curve: runs and exits 0");
  double a = mean_level(plain.out.data, plain.out.len);
  double w = mean_level(white.out.data, white.out.len);
  double k = mean_level(black.out.data, black.out.len);
  double c = mean_level(same.out.data, same.out.len);
  check(a > 1 && a < 8,
        "the fake picture uses the whole ramp (mean level %.2f)", a);
  check(w > 7.5,
        "--curve 0,255 255,255 makes everything the brightest character (%.2f)",
        w);
  check(0 <= k && k < 1.0,
        "--curve 0,0 255,0 makes everything the darkest (%.2f)", k);
  check((a > c ? a - c : c - a) < 1.0,
        "--curve 0,0 255,255 changes nothing (%.2f vs %.2f)", a, c);
  free(plain.out.data);
  free(white.out.data);
  free(black.out.data);
  free(same.out.data);

  static const char *bad[] = {"0,0 255",     "0,0 100,50",
                              "5,0 255,255", "0,0 99999,5 255,255",
                              "nonsense",    "@/nonexistent/c"};
  for (int i = 0; i < 6; i++) {
    run_t r = run(plus(plus(cargs, "--curve"), bad[i]), 0.5, -1, "q");
    check(r.code == 2 && contains(r.out.data, r.out.len, "curve"),
          "--curve '%s' is refused", bad[i]);
    free(r.out.data);
  }

  run_t r = run(plus(cargs, "--curve"), 0.5, -1, "q");
  check(r.code == 2, "--curve without a value is refused");
  free(r.out.data);

  char cdir[] = "/tmp/asciicam_curve_XXXXXX";
  if (mkdtemp(cdir)) {
    char cfile[300];
    char arg[310];
    snprintf(cfile, sizeof cfile, "%s/c.txt", cdir);
    snprintf(arg, sizeof arg, "@%s", cfile);
    write_file(cfile, "0,255 255,255\n", 14);
    r = run(plus(plus(cargs, "--curve"), arg), 1.2, -1, "q");
    check(r.code == 0 && mean_level(r.out.data, r.out.len) > 7.5,
          "--curve @file reads the points from a file");
    free(r.out.data);
    rm_rf(cdir);
  }

  // Panel: k opens Curve tab, [ ] change preset, tab switches tabs
  step_t panel[] = {S("k", 0.6),  S("]", 0.4),  S("]", 0.4),
                    S("\t", 0.5), S("\t", 0.5), S("k", 0.5)};
  sess_t s = session(cargs, panel, 6, 0.6);
  check(range_has(&s, 0, 1, "Tone curve"), "k opens the curve editor");
  check(range_has(&s, 1, 2, "preset: s-curve") &&
            range_has(&s, 2, 3, "preset: asinh"),
        "[ and ] switch presets and the panel shows the name");
  check(range_has(&s, 3, 4, "Captures"), "tab goes to the Files tab");
  check(range_has(&s, 4, 5, "Tone curve"), "tab again goes back to the curve");
  check(!range_has(&s, 6, -1, "Tone curve"), "k closes the panel");
  check(contains(s.out.data, s.out.len, "F") &&
            contains(s.out.data, s.out.len, "C"),
        "tab strip drawn");
  free(s.out.data);

  // Curve acts on live picture: lift black point to 255 in editor
  step_t lift[34];
  lift[0] = S("k", 0.5);
  for (int i = 1; i <= 32; i++) {
    lift[i] = S("+", 0.03);
  }

  lift[33] = S("k", 0.8);
  s = session(cargs, lift, 34, 0.6);
  double open_lvl =
      mean_level(s.out.data + s.marks[0], s.marks[1] - s.marks[0]);
  int m3 = s.nmarks - 3;
  double lifted = mean_level(s.out.data + s.marks[m3], s.out.len - s.marks[m3]);
  check(open_lvl < 8, "editor opens on a normal picture");
  check(lifted > 7.5,
        "raising the black point to 255 makes the picture white (%.2f)",
        lifted);
  free(s.out.data);

  // Tab strip with mouse: open Curve tab, then retract it by clicking it again
  step_t strip[] = {M(0, 119, 9, 0, 0.2), M(0, 119, 9, 1, 0.5),
                    M(0, 85, 9, 0, 0.2), M(0, 85, 9, 1, 0.6)};
  s = session(cargs, strip, 4, 0.6);
  check(range_has(&s, 0, 2, "Tone curve"),
        "clicking the Curve tab of the strip opens the panel");
  check(!range_has(&s, 3, 5, "Tone curve"),
        "clicking the open tab retracts it");
  free(s.out.data);

  // Clicking in graph adds a point and dragging moves it; editor reports it
  step_t graph[] = {S("k", 0.6), M(0, 95, 8, 0, 0.2), M(32, 104, 3, 0, 0.2),
                    M(0, 104, 3, 1, 0.4)};
  s = session(cargs, graph, 4, 0.6);
  check(range_has(&s, 1, -1, "pt 2/3"),
        "click in the graph adds a point and selects it");
  check(contains(s.out.data, s.out.len, "frame: 40x20") ||
            !last_frame(s.out.data, s.out.len, &(int){0}, &(int){0}) ||
            frame_is(s.out.data, s.out.len, 40, 20),
        "the frame was not dragged by clicks on the panel");
  free(s.out.data);
}

/* Speech recognition: --voice-enroll, --voice-list, --listen,
 * --voice-forget, and --listen --model
 * Speech is synthesised (a buzz shaped by two moving formants), so this needs
 * no audio files and no sound card
 * Program is run for these checks with pipes, not a terminal */

// sin() without libm: reduce to [-pi, pi], then Taylor series
static double sp_sin(double x) {
  const double pi = 3.14159265358979323846;
  while (x > pi) {
    x -= 2 * pi;
  }

  while (x < -pi) {
    x += 2 * pi;
  }

  double term = x, sum = x;
  for (int k = 1; k < 14; k++) {
    term *= -x * x / ((2.0 * k) * (2.0 * k + 1.0));
    sum += term;
  }

  return sum;
}

static double sp_sin_table[8192];

typedef struct {
  short *s;
  size_t n;
  size_t cap;
} pcm_t;

static void pcm_add(pcm_t *p, short v) {
  if (p->n == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 65536;
    p->s = (short *)realloc(p->s, p->cap * sizeof(short));
    if (!p->s) {
      abort();
    }
  }

  p->s[p->n++] = v;
}

static void pcm_quiet(pcm_t *p, int ms) {
  for (int i = 0; i < ms * 16; i++) {
    pcm_add(p, 0);
  }
}

typedef struct {
  int ms;
  double f1;
  double f2;
} seg_t;

// A buzz at pitch f0 whose harmonics are weighted by two formants that move
// from segment to segment
static void pcm_synth(pcm_t *p, const seg_t *segs, int nseg, double tempo,
                      double f0) {
  if (!sp_sin_table[1]) {
    for (int i = 0; i < 8192; i++) {
      sp_sin_table[i] = sp_sin(2 * 3.14159265358979323846 * i / 8192);
    }
  }

  long n = 0;
  for (int s = 0; s < nseg; s++) {
    double f1 = segs[s].f1;
    double f2 = segs[s].f2;
    long ln = (long)(segs[s].ms * tempo * 16);
    for (long i = 0; i < ln; i++) {
      double t = (double)(n + i) / 16000.0;
      double v = 0;
      for (int h = 1; h * f0 < 4500; h++) {
        double f = h * f0;
        double w1 = 0.12 * f1 + 60;
        double w2 = 0.12 * f2 + 60;
        double d1 = (f - f1) / w1;
        double d2 = (f - f2) / w2;
        double a = 1 / (1 + d1 * d1) + 0.7 / (1 + d2 * d2);
        double ph = f * t;
        v += a * sp_sin_table[(int)((ph - (long)ph) * 8192) & 8191];
      }

      double env = 1.0;
      if (i / 80.0 < env) {
        env = i / 80.0;
      }

      if ((double)(ln - i) / 80.0 < env) {
        env = (double)(ln - i) / 80.0;
      }

      pcm_add(p, (short)(int)(v * env * 1500));
    }

    n += ln;
  }
}

static void pcm_append(pcm_t *p, const pcm_t *q) {
  for (size_t i = 0; i < q->n; i++) {
    pcm_add(p, q->s[i]);
  }
}

static pcm_t pcm_of_quiet(int ms) {
  pcm_t p = {0};
  pcm_quiet(&p, ms);

  return p;
}

static pcm_t pcm_of_synth(const seg_t *segs, int nseg, double tempo,
                          double f0) {
  pcm_t p = {0};
  pcm_synth(&p, segs, nseg, tempo, f0);

  return p;
}

static void le32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}

// 16 kHz mono 16 bit WAV (returned buffer is 44 bytes of header + samples)
static unsigned char *wav_bytes(const pcm_t *p, size_t *size) {
  size_t dn = p->n * 2;
  unsigned char *w = (unsigned char *)malloc(44 + dn);
  memcpy(w, "RIFF", 4);
  le32(w + 4, (uint32_t)(36 + dn));
  memcpy(w + 8, "WAVEfmt ", 8);
  le32(w + 16, 16);
  w[20] = 1; // PCM
  w[21] = 0;
  w[22] = 1; // Mono
  w[23] = 0;
  le32(w + 24, 16000);
  le32(w + 28, 32000);
  w[32] = 2; // Block align
  w[33] = 0;
  w[34] = 16; // Bits
  w[35] = 0;
  memcpy(w + 36, "data", 4);
  le32(w + 40, (uint32_t)dn);
  for (size_t i = 0; i < p->n; i++) {
    w[44 + 2 * i] = (unsigned char)(p->s[i] & 255);
    w[45 + 2 * i] = (unsigned char)((p->s[i] >> 8) & 255);
  }

  *size = 44 + dn;

  return w;
}

static int write_wav(const char *path, const pcm_t *p) {
  size_t n;
  unsigned char *w = wav_bytes(p, &n);
  int ok = write_file(path, w, n);
  free(w);

  return ok;
}

static int write_pcm(const char *path, const pcm_t *p) {
  unsigned char *raw = (unsigned char *)malloc(p->n * 2 + 1);
  for (size_t i = 0; i < p->n; i++) {
    raw[2 * i] = (unsigned char)(p->s[i] & 255);
    raw[2 * i + 1] = (unsigned char)((p->s[i] >> 8) & 255);
  }

  int ok = write_file(path, raw, p->n * 2);
  free(raw);

  return ok;
}

/* Programs run with pipes */

// Make buffer a C string without counting terminator
static void buf_nul(pty_buf *b) {
  pty_buf_add(b, "", 1);
  b->len--;
}

typedef struct {
  pty_buf out;
  pty_buf err;
  int code;
} proc_t;

// Starts argv with stdin from in_fd (or /dev/null), stdout and stderr to pipes
// Returns pid; *out_fd and *err_fd are read ends (*err_fd -1: stderr goes to
// /dev/null); *in_fd is write end when want_in
static pid_t spawn_piped(args_t a, const char *env_name, const char *env_val,
                         int want_in, int *in_fd, int *out_fd, int *err_fd) {
  int pin[2] = {-1, -1};
  int pout[2];
  int perr[2] = {-1, -1};
  if (want_in && pipe(pin) != 0) {
    return -1;
  }

  if (pipe(pout) != 0 || (err_fd && pipe(perr) != 0)) {
    return -1;
  }

  char *argv[50];
  argv[0] = (char *)BIN;
  for (int i = 0; i < a.n; i++) {
    argv[i + 1] = (char *)a.v[i];
  }

  argv[a.n + 1] = NULL;
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_RDWR);
    dup2(want_in ? pin[0] : dn, 0);
    dup2(pout[1], 1);
    dup2(err_fd ? perr[1] : dn, 2);
    for (int fd = 3; fd < 64; fd++) {
      close(fd);
    }

    if (env_name) {
      setenv(env_name, env_val, 1);
    }

    execv(BIN, argv);
    _exit(127);
  }

  if (want_in) {
    close(pin[0]);
    *in_fd = pin[1];
  }

  close(pout[1]);
  *out_fd = pout[0];
  if (err_fd) {
    close(perr[1]);
    *err_fd = perr[0];
  }

  return pid;
}

static int wait_exit(pid_t pid, double seconds) {
  double end = pty_now() + seconds;
  int st = 0;
  for (;;) {
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) {
      return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }

    if (r < 0 && errno != EINTR) {
      return -1;
    }

    if (pty_now() > end) {
      kill(pid, SIGKILL);
      waitpid(pid, &st, 0);

      return -1;
    }

    usleep(5000);
  }
}

// Run to end, feeding `in` on stdin (when given); timeout in seconds
static proc_t proc_run(args_t a, const char *env_name, const char *env_val,
                       const void *in, size_t inn, double timeout) {
  proc_t r;
  memset(&r, 0, sizeof r);
  signal(SIGPIPE, SIG_IGN);
  int in_fd = -1, out_fd = -1, err_fd = -1;
  pid_t pid =
      spawn_piped(a, env_name, env_val, in != NULL, &in_fd, &out_fd, &err_fd);
  if (pid < 0) {
    r.code = -1;

    return r;
  }

  size_t woff = 0;
  if (in && inn == 0) {
    close(in_fd);
    in_fd = -1;
  }

  double end = pty_now() + timeout;
  int out_open = 1, err_open = 1;
  while ((out_open || err_open) && pty_now() < end) {
    struct pollfd pf[3];
    int n = 0, io = -1, ie = -1, ii = -1;
    if (out_open) {
      io = n;
      pf[n++] = (struct pollfd){out_fd, POLLIN, 0};
    }

    if (err_open) {
      ie = n;
      pf[n++] = (struct pollfd){err_fd, POLLIN, 0};
    }

    if (in && in_fd >= 0) {
      ii = n;
      pf[n++] = (struct pollfd){in_fd, POLLOUT, 0};
    }

    if (poll(pf, (nfds_t)n, 100) <= 0) {
      continue;
    }

    unsigned char tmp[65536];
    if (io >= 0 && pf[io].revents) {
      ssize_t k = read(out_fd, tmp, sizeof tmp);
      if (k <= 0) {
        out_open = 0;
      } else {
        pty_buf_add(&r.out, tmp, (size_t)k);
      }
    }

    if (ie >= 0 && pf[ie].revents) {
      ssize_t k = read(err_fd, tmp, sizeof tmp);
      if (k <= 0) {
        err_open = 0;
      } else {
        pty_buf_add(&r.err, tmp, (size_t)k);
      }
    }

    if (ii >= 0 && pf[ii].revents) {
      size_t chunk = inn - woff > 32768 ? 32768 : inn - woff;
      ssize_t k = write(in_fd, (const unsigned char *)in + woff, chunk);
      if (k > 0) {
        woff += (size_t)k;
      }

      if (k < 0 || woff >= inn) {
        close(in_fd);
        in_fd = -1;
      }
    }
  }

  if (in_fd >= 0) {
    close(in_fd);
  }

  close(out_fd);
  close(err_fd);
  r.code = wait_exit(pid, 5.0);
  buf_nul(&r.out);
  buf_nul(&r.err);

  return r;
}

// Whitespace-separated words, in place (like Python's str.split())
static int split_ws(char *s, char **tok, int max) {
  int n = 0;
  while (*s && n < max) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
      s++;
    }

    if (!*s) {
      break;
    }

    tok[n++] = s;
    while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
      s++;
    }

    if (*s) {
      *s++ = 0;
    }
  }

  return n;
}

// Lines, in place (like str.splitlines())
static int split_lines(char *s, char **tok, int max) {
  int n = 0;
  while (*s && n < max) {
    tok[n++] = s;
    while (*s && *s != '\n') {
      s++;
    }

    if (*s) {
      if (s > tok[n - 1] && s[-1] == '\r') {
        s[-1] = 0;
      }

      *s++ = 0;
    }
  }

  return n;
}

static int count_tok(char **t, int n, const char *w) {
  int k = 0;
  for (int i = 0; i < n; i++) {
    k += !strcmp(t[i], w);
  }

  return k;
}

// 3 taught words, in order, once "~" hypotheses are dropped
static int finals_are(char **t, int n, const char *w1, const char *w2,
                      const char *w3) {
  const char *want[3] = {w1, w2, w3};
  int k = 0;
  for (int i = 0; i < n; i++) {
    if (t[i][0] == '~') {
      continue;
    }

    if (k >= 3 || strcmp(t[i], want[k])) {
      return 0;
    }

    k++;
  }

  return k == 3;
}

static int any_hyp(char **t, int n) {
  for (int i = 0; i < n; i++) {
    if (t[i][0] == '~') {
      return 1;
    }
  }

  return 0;
}

// First 100 characters of a buffer on one line, for messages
static const char *one_line(const pty_buf *b) {
  static char ring[4][112];
  static int k;
  char *s = ring[k++ & 3];
  size_t n = b->len < 100 ? b->len : 100;
  for (size_t i = 0; i < n; i++) {
    s[i] = b->data[i] == '\n' || b->data[i] == '\r' ? ' ' : (char)b->data[i];
  }

  s[n] = 0;

  return s;
}

static args_t vargs(const char *vfile, args_t rest) {
  return cat(A("--voice", vfile, NULL), rest);
}

static void drain_fd(int fd, pty_buf *b, double secs, int *eof) {
  double end = pty_now() + secs;
  while (pty_now() < end && !*eof) {
    struct pollfd pf = {fd, POLLIN, 0};
    if (poll(&pf, 1, 20) <= 0) {
      continue;
    }

    unsigned char tmp[4096];
    ssize_t k = read(fd, tmp, sizeof tmp);
    if (k <= 0) {
      *eof = 1;
    } else {
      pty_buf_add(b, tmp, (size_t)k);
    }
  }
}

static int running(pid_t pid) {
  int st;

  return waitpid(pid, &st, WNOHANG) == 0;
}

static void speech_tests(void) {
  static const seg_t ALPHA[3] = {
      {120, 300, 2200}, {150, 700, 1200}, {120, 400, 800}};
  static const seg_t BRAVO[3] = {
      {150, 600, 1000}, {120, 300, 2500}, {150, 500, 1500}};
  static const seg_t VX[3] = {
      {160, 900, 1700}, {160, 250, 3200}, {140, 750, 2400}}; // Never taught
  char vdir[] = "/tmp/asciicam-voice-XXXXXX";
  if (!mkdtemp(vdir)) {
    check(0, "speech: cannot make a scratch directory");

    return;
  }

  char vfile[300];
  char wp[320];
  char pcm_path[320];
  char wav_path[320];
  char fifo[320];
  snprintf(vfile, sizeof vfile, "%s/sub/voice.tpl", vdir);
  snprintf(wp, sizeof wp, "%s/alpha.wav", vdir);
  snprintf(pcm_path, sizeof pcm_path, "%s/said.pcm", vdir);
  snprintf(wav_path, sizeof wav_path, "%s/said.wav", vdir);
  snprintf(fifo, sizeof fifo, "%s/mic.fifo", vdir);

  proc_t r =
      proc_run(vargs(vfile, A("--voice-list", NULL)), NULL, NULL, NULL, 0, 60);
  check(r.code == 0 && contains(r.err.data, r.err.len, "0 words"),
        "an empty voice file lists nothing");
  free(r.out.data);
  free(r.err.data);

  static const struct {
    const char *name;
    const seg_t *segs;
  } words[2] = {{"alpha", ALPHA}, {"bravo", BRAVO}};
  static const struct {
    double tempo;
    double f0;
    const char *label;
  } says[3] = {{1.0, 120, "1.0"}, {0.85, 105, "0.85"}, {1.2, 135, "1.2"}};
  for (int w = 0; w < 2; w++) {
    for (int k = 0; k < 3; k++) {
      pcm_t p = pcm_of_quiet(300);
      pcm_t sy = pcm_of_synth(words[w].segs, 3, says[k].tempo, says[k].f0);
      pcm_t q = pcm_of_quiet(500);
      pcm_append(&p, &sy);
      pcm_append(&p, &q);
      write_wav(wp, &p);
      r = proc_run(vargs(vfile, A("--voice-enroll", words[w].name, wp, NULL)),
                   NULL, NULL, NULL, 0, 60);
      check(r.code == 0 && contains(r.err.data, r.err.len, "1 new"),
            "enrol %s %s: %s", words[w].name, says[k].label, one_line(&r.err));
      free(r.out.data);
      free(r.err.data);
      free(p.s);
      free(sy.s);
      free(q.s);
    }
  }

  struct stat st;
  check(stat(vfile, &st) == 0,
        "the voice file was created, with its directories");
  r = proc_run(vargs(vfile, A("--voice-list", NULL)), NULL, NULL, NULL, 0, 60);
  check(contains(r.err.data, r.err.len, "2 words, 6 templates") &&
            contains(r.err.data, r.err.len, "alpha") &&
            contains(r.err.data, r.err.len, "bravo"),
        "list shows both words: %s", one_line(&r.err));
  free(r.out.data);
  free(r.err.data);

  // Enrolling from stdin ("-"), a raw PCM stream
  {
    pcm_t p = pcm_of_quiet(300);
    pcm_t sy = pcm_of_synth(ALPHA, 3, 0.95, 125);
    pcm_t q = pcm_of_quiet(500);
    pcm_append(&p, &sy);
    pcm_append(&p, &q);
    unsigned char *raw = (unsigned char *)malloc(p.n * 2);
    for (size_t i = 0; i < p.n; i++) {
      raw[2 * i] = (unsigned char)(p.s[i] & 255);
      raw[2 * i + 1] = (unsigned char)((p.s[i] >> 8) & 255);
    }

    r = proc_run(vargs(vfile, A("--voice-enroll", "alpha", "-", NULL)), NULL,
                 NULL, raw, p.n * 2, 60);
    check(r.code == 0 && contains(r.err.data, r.err.len, "4 of 5"),
          "enrol from stdin: %s", one_line(&r.err));
    free(r.out.data);
    free(r.err.data);
    free(raw);
    free(p.s);
    free(sy.s);
    free(q.s);
  }

  // A sentence: known, unknown, known, known. Said at other speeds and pitch
  pcm_t said = pcm_of_quiet(600);
  {
    pcm_t part;
    pcm_t gaps[3] = {pcm_of_quiet(700), pcm_of_quiet(700), pcm_of_quiet(700)};
    part = pcm_of_synth(BRAVO, 3, 0.92, 112);
    pcm_append(&said, &part);
    free(part.s);
    pcm_append(&said, &gaps[0]);
    part = pcm_of_synth(VX, 3, 1.0, 120);
    pcm_append(&said, &part);
    free(part.s);
    pcm_append(&said, &gaps[1]);
    part = pcm_of_synth(ALPHA, 3, 1.1, 128);
    pcm_append(&said, &part);
    free(part.s);
    pcm_append(&said, &gaps[2]);
    part = pcm_of_synth(BRAVO, 3, 1.0, 118);
    pcm_append(&said, &part);
    free(part.s);
    part = pcm_of_quiet(600);
    pcm_append(&said, &part);
    free(part.s);
    for (int i = 0; i < 3; i++) {
      free(gaps[i].s);
    }
  }

  write_pcm(pcm_path, &said);
  write_wav(wav_path, &said);
  static const char *labels[2] = {"raw pcm", "wav"};
  const char *paths[2] = {pcm_path, wav_path};
  for (int k = 0; k < 2; k++) {
    r = proc_run(vargs(vfile, A("--listen", paths[k], NULL)), NULL, NULL, NULL,
                 0, 60);
    char *tok[64];
    char *copy = strdup((char *)r.out.data ? (char *)r.out.data : "");
    int n = split_ws(copy, tok, 64);
    check(r.code == 0, "--listen %s ends at the end of the file", labels[k]);
    check(finals_are(tok, n, "bravo", "alpha", "bravo"),
          "%s: the three taught words in order, the unknown one skipped "
          "(%d words)",
          labels[k], n);
    check(any_hyp(tok, n), "%s: hypotheses come before finals", labels[k]);
    free(copy);
    free(r.out.data);
    free(r.err.data);
  }

  // Same from a pipe on stdin
  {
    unsigned char *raw = (unsigned char *)malloc(said.n * 2);
    for (size_t i = 0; i < said.n; i++) {
      raw[2 * i] = (unsigned char)(said.s[i] & 255);
      raw[2 * i + 1] = (unsigned char)((said.s[i] >> 8) & 255);
    }

    r = proc_run(vargs(vfile, A("--listen", "-", NULL)), NULL, NULL, raw,
                 said.n * 2, 60);
    char *tok[64];
    char *copy = strdup((char *)r.out.data ? (char *)r.out.data : "");
    int n = split_ws(copy, tok, 64);
    check(count_tok(tok, n, "bravo") >= 2, "--listen - reads stdin");
    free(copy);
    free(r.out.data);
    free(r.err.data);
    free(raw);
  }

  // A live fifo, written at about real time, is followed and gives lines as it
  // goes
  mkfifo(fifo, 0600);
  int ofd = -1;
  pid_t pid = spawn_piped(vargs(vfile, A("--listen", fifo, NULL)), NULL, NULL,
                          0, NULL, &ofd, NULL);
  pty_buf live = {0};
  int live_eof = 0;
  drain_fd(ofd, &live, 0.3, &live_eof);
  int wfd = -1;
  for (int i = 0; i < 100 && wfd < 0; i++) { // Until program opens it
    wfd = open(fifo, O_WRONLY | O_NONBLOCK);
    if (wfd < 0) {
      usleep(50000);
    }
  }

  if (wfd >= 0) {
    fcntl(wfd, F_SETFL, fcntl(wfd, F_GETFL) & ~O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);
    unsigned char *raw = (unsigned char *)malloc(said.n * 2);
    for (size_t i = 0; i < said.n; i++) {
      raw[2 * i] = (unsigned char)(said.s[i] & 255);
      raw[2 * i + 1] = (unsigned char)((said.s[i] >> 8) & 255);
    }

    size_t total = said.n * 2;
    for (size_t off = 0; off < total; off += 3200) {
      size_t len = total - off > 3200 ? 3200 : total - off;
      ssize_t w = write(wfd, raw + off, len);
      (void)w;
      drain_fd(ofd, &live, 0.05, &live_eof); // Twice real time: it does not
                                             // care
    }

    free(raw);
    drain_fd(ofd, &live, 1.0, &live_eof);
    check(running(pid),
          "--listen on a fifo keeps running when the writer pauses");
    close(wfd);
    drain_fd(ofd, &live, 0.4, &live_eof);
    check(running(pid), "and when the writer closes it");
  } else {
    check(0, "--listen on a fifo keeps running when the writer pauses");
    check(0, "and when the writer closes it");
  }

  kill(pid, SIGTERM);
  drain_fd(ofd, &live, 5.0, &live_eof);
  wait_exit(pid, 5.0);
  close(ofd);
  buf_nul(&live);
  {
    char *tok[64];
    char *copy = strdup((char *)live.data ? (char *)live.data : "");
    int n = split_ws(copy, tok, 64);
    check(finals_are(tok, n, "bravo", "alpha", "bravo"),
          "live fifo gives the words (%d words)", n);
    free(copy);
  }

  free(live.data);

  // Not usable: nothing enrolled, a bad file, a bad format
  char none[320];
  char absent[320];
  char bad8[320];
  char silent[320];
  snprintf(none, sizeof none, "%s/none.tpl", vdir);
  snprintf(absent, sizeof absent, "%s/absent.pcm", vdir);
  snprintf(bad8, sizeof bad8, "%s/bad8.wav", vdir);
  snprintf(silent, sizeof silent, "%s/silent.wav", vdir);
  r = proc_run(vargs(none, A("--listen", pcm_path, NULL)), NULL, NULL, NULL, 0,
               30);
  check(r.code == 1 && contains(r.err.data, r.err.len, "nothing enrolled"),
        "nothing enrolled is said: %s", one_line(&r.err));
  free(r.out.data);
  free(r.err.data);
  r = proc_run(vargs(vfile, A("--listen", absent, NULL)), NULL, NULL, NULL, 0,
               60);
  check(r.code == 1 && contains(r.err.data, r.err.len, "cannot open"),
        "missing audio file is an error");
  free(r.out.data);
  free(r.err.data);
  {
    pcm_t q = pcm_of_quiet(200);
    size_t n;
    unsigned char *w = wav_bytes(&q, &n);
    w[32] = 1; // Block align 1, 8 bit
    w[34] = 8;
    write_file(bad8, w, n);
    free(w);
    free(q.s);
    q = pcm_of_quiet(2000);
    write_wav(silent, &q);
    free(q.s);
  }

  r = proc_run(vargs(vfile, A("--listen", bad8, NULL)), NULL, NULL, NULL, 0,
               60);
  check(r.code == 1 && contains(r.err.data, r.err.len, "16 bit"),
        "8 bit audio is refused: %s", one_line(&r.err));
  free(r.out.data);
  free(r.err.data);
  r = proc_run(vargs(vfile, A("--voice-enroll", "x", bad8, NULL)), NULL, NULL,
               NULL, 0, 60);
  check(r.code == 1, "and so is enrolling it");
  free(r.out.data);
  free(r.err.data);
  r = proc_run(vargs(vfile, A("--voice-enroll", "x", silent, NULL)), NULL, NULL,
               NULL, 0, 60);
  check(r.code == 1 && contains(r.err.data, r.err.len, "no speech"),
        "enrolling silence says so");
  free(r.out.data);
  free(r.err.data);
  r = proc_run(A("--voice-enroll", "x", NULL), NULL, NULL, NULL, 0, 10);
  check(r.code == 2 && contains(r.err.data, r.err.len, "needs"),
        "--voice-enroll needs two values");
  free(r.out.data);
  free(r.err.data);
  r = proc_run(A("--listen", NULL), NULL, NULL, NULL, 0, 10);
  check(r.code == 2, "--listen needs a value");
  free(r.out.data);
  free(r.err.data);

  // Forget
  r = proc_run(vargs(vfile, A("--voice-forget", "alpha", NULL)), NULL, NULL,
               NULL, 0, 60);
  check(r.code == 0 && contains(r.err.data, r.err.len, "forgot"),
        "forget alpha");
  free(r.out.data);
  free(r.err.data);
  r = proc_run(vargs(vfile, A("--voice-forget", "alpha", NULL)), NULL, NULL,
               NULL, 0, 60);
  check(r.code == 1 && contains(r.err.data, r.err.len, "not enrolled"),
        "forgetting twice is reported");
  free(r.out.data);
  free(r.err.data);
  r = proc_run(vargs(vfile, A("--listen", pcm_path, NULL)), NULL, NULL, NULL, 0,
               60);
  {
    char *tok[64];
    char *copy = strdup((char *)r.out.data ? (char *)r.out.data : "");
    int n = split_ws(copy, tok, 64);
    check(count_tok(tok, n, "alpha") == 0 &&
              contains(r.out.data, r.out.len, "bravo"),
          "a forgotten word is no longer heard");
    free(copy);
  }

  free(r.out.data);
  free(r.err.data);

  // Default location comes from XDG_CONFIG_HOME
  {
    char xdg[320];
    char tpl[400];
    snprintf(xdg, sizeof xdg, "%s/xdg", vdir);
    snprintf(tpl, sizeof tpl, "%s/xdg/asciicam/voice.tpl", vdir);
    pcm_t p = pcm_of_quiet(300);
    pcm_t sy = pcm_of_synth(ALPHA, 3, 1.0, 120.0);
    pcm_t q = pcm_of_quiet(500);
    pcm_append(&p, &sy);
    pcm_append(&p, &q);
    write_wav(wp, &p);
    r = proc_run(A("--voice-enroll", "alpha", wp, NULL), "XDG_CONFIG_HOME", xdg,
                 NULL, 0, 30);
    check(stat(tpl, &st) == 0, "default path under XDG_CONFIG_HOME: %s",
          one_line(&r.err));
    free(r.out.data);
    free(r.err.data);
    free(p.s);
    free(sy.s);
    free(q.s);
  }

  free(said.s);
  rm_rf(vdir);
}

/* Sentences from a neural model (--listen --model)
 * Weights are tiny random model of unit tests (tests/asrnet_fixture.h, which
 * needs nolibc, so its recipe is repeated here with libc), so words are noise:
 * what is checked is that real binary loads weights + tokenizer.json +
 * config.json, cuts audio into utterances and prints one line per sentence */

typedef struct {
  char name[96];
  int nd;
  size_t shape[3];
  double scale;
  char kind; // w weight, g gamma, o around 1, l log_k
} mdef_t;

static mdef_t mdefs[200];
static int nmdefs;
static unsigned mdl_s;

static double mdl_prng(void) {
  unsigned s = mdl_s;
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  mdl_s = s;

  return (double)s / 4294967296.0;
}

static void mdef(const char *name, double scale, char kind, int nd, size_t a,
                 size_t b, size_t c) {
  mdef_t *t = &mdefs[nmdefs++];
  snprintf(t->name, sizeof t->name, "%s", name);
  t->nd = nd;
  t->shape[0] = a;
  t->shape[1] = b;
  t->shape[2] = c;
  t->scale = scale;
  t->kind = kind;
}

static void make_model_defs(void) {
  nmdefs = 0;
  size_t h = 32;
  size_t f = 64;
  size_t d = 32;
  size_t df = 48;
  size_t V = 50;
  size_t P = 64;
  char n[96];
  mdef("model.encoder.embedder.comp.log_k", 0, 'l', 1, 1, 0, 0);
  mdef("model.encoder.embedder.linear.weight", 0.25, 'w', 2, h, 80, 0);
  mdef("model.encoder.embedder.conv1.weight", 0.12, 'w', 3, 2 * h, h, 5);
  mdef("model.encoder.embedder.conv1.bias", 0.1, 'w', 1, 2 * h, 0, 0);
  mdef("model.encoder.embedder.conv2.weight", 0.08, 'w', 3, h, 2 * h, 5);
  mdef("model.encoder.embedder.conv2.bias", 0.1, 'w', 1, h, 0, 0);
  for (int l = 0; l < 3; l++) {
    for (int q = 0; q < 4; q++) {
      snprintf(n, sizeof n, "model.encoder.layers.%d.self_attn.%c_proj.weight",
               l, "qkvo"[q]);
      mdef(n, 0.3, 'w', 2, h, h, 0);
    }

    snprintf(n, sizeof n, "model.encoder.layers.%d.input_layernorm.gamma", l);
    mdef(n, 0.3, 'g', 1, h, 0, 0);
    snprintf(n, sizeof n,
             "model.encoder.layers.%d.post_attention_layernorm.gamma", l);
    mdef(n, 0.3, 'g', 1, h, 0, 0);
    snprintf(n, sizeof n, "model.encoder.layers.%d.mlp.fc1.weight", l);
    mdef(n, 0.3, 'w', 2, f, h, 0);
    snprintf(n, sizeof n, "model.encoder.layers.%d.mlp.fc1.bias", l);
    mdef(n, 0.1, 'w', 1, f, 0, 0);
    snprintf(n, sizeof n, "model.encoder.layers.%d.mlp.fc2.weight", l);
    mdef(n, 0.25, 'w', 2, h, f, 0);
    snprintf(n, sizeof n, "model.encoder.layers.%d.mlp.fc2.bias", l);
    mdef(n, 0.1, 'w', 1, h, 0, 0);
  }

  mdef("model.encoder.final_norm.gamma", 0.3, 'g', 1, h, 0, 0);
  mdef("model.decoder.embed_tokens.weight", 0.6, 'w', 2, V, d, 0);
  mdef("model.decoder.pos_emb.weight", 0.3, 'w', 2, P, h, 0);
  for (int l = 0; l < 2; l++) {
    for (int b = 0; b < 2; b++) {
      for (int q = 0; q < 4; q++) {
        snprintf(n, sizeof n, "model.decoder.layers.%d.%s.%c_proj.weight", l,
                 b ? "encoder_attn" : "self_attn", "qkvo"[q]);
        mdef(n, 0.3, 'w', 2, d, d, 0);
      }
    }

    const char *ln3[3] = {"input_layernorm", "post_attention_layernorm",
                          "final_layernorm"};
    for (int i = 0; i < 3; i++) {
      snprintf(n, sizeof n, "model.decoder.layers.%d.%s.weight", l, ln3[i]);
      mdef(n, 0.3, 'o', 1, d, 0, 0);
    }

    snprintf(n, sizeof n, "model.decoder.layers.%d.mlp.fc1.weight", l);
    mdef(n, 0.3, 'w', 2, 2 * df, d, 0);
    snprintf(n, sizeof n, "model.decoder.layers.%d.mlp.fc1.bias", l);
    mdef(n, 0.1, 'w', 1, 2 * df, 0, 0);
    snprintf(n, sizeof n, "model.decoder.layers.%d.mlp.fc2.weight", l);
    mdef(n, 0.25, 'w', 2, d, df, 0);
    snprintf(n, sizeof n, "model.decoder.layers.%d.mlp.fc2.bias", l);
    mdef(n, 0.1, 'w', 1, d, 0, 0);
  }

  mdef("model.decoder.norm.weight", 0.3, 'o', 1, d, 0, 0);
  mdef("proj_out.weight", 0.5, 'w', 2, V, d, 0);
}

static size_t mdef_numel(const mdef_t *t) {
  size_t n = 1;
  for (int i = 0; i < t->nd; i++) {
    n *= t->shape[i];
  }

  return n;
}

// Safetensors file, all F32
static unsigned char *build_model_file(size_t *size) {
  make_model_defs();
  size_t total = 0;
  for (int i = 0; i < nmdefs; i++) {
    total += mdef_numel(&mdefs[i]) * 4;
  }

  char *hdr = (char *)malloc((size_t)nmdefs * 300 + 64);
  size_t hn = 0, off = 0;
  hdr[hn++] = '{';
  for (int i = 0; i < nmdefs; i++) {
    size_t bytes = mdef_numel(&mdefs[i]) * 4;
    char shape[64];
    size_t sn = 0;
    for (int k = 0; k < mdefs[i].nd; k++) {
      sn += (size_t)snprintf(shape + sn, sizeof shape - sn, "%s%d",
                             k ? "," : "", (int)mdefs[i].shape[k]);
    }

    shape[sn] = 0;
    hn += (size_t)snprintf(hdr + hn, 300,
                           "%s\"%s\":{\"dtype\":\"F32\",\"shape\":[%s],"
                           "\"data_offsets\":[%d,%d]}",
                           i ? "," : "", mdefs[i].name, shape, (int)off,
                           (int)(off + bytes));
    off += bytes;
  }

  hdr[hn++] = '}';
  while (hn % 8) {
    hdr[hn++] = ' ';
  }

  unsigned char *file = (unsigned char *)malloc(8 + hn + total);
  for (int i = 0; i < 8; i++) {
    file[i] = (unsigned char)((hn >> (8 * i)) & 255);
  }

  memcpy(file + 8, hdr, hn);
  free(hdr);
  unsigned char *p = file + 8 + hn;
  mdl_s = 0x1234ABCDu;
  for (int i = 0; i < nmdefs; i++) {
    size_t n = mdef_numel(&mdefs[i]);
    for (size_t k = 0; k < n; k++) {
      float v;
      if (mdefs[i].kind == 'l') {
        v = (float)-0.2876820724517809; // log(0.75)
      } else {
        double x = (mdl_prng() - 0.5) * mdefs[i].scale;
        if (mdefs[i].kind == 'o') {
          x += 1.0;
        }

        v = (float)x;
      }

      memcpy(p, &v, 4);
      p += 4;
    }
  }

  *size = (size_t)(p - file);

  return file;
}

static unsigned rnd_s = 5;

static int rnd_range(int lo, int hi) { // Like random.randint
  rnd_s ^= rnd_s << 13;
  rnd_s ^= rnd_s >> 17;
  rnd_s ^= rnd_s << 5;

  return lo + (int)(rnd_s % (unsigned)(hi - lo + 1));
}

// Noisy tone: loud, steady, nothing like voice test's words
static void pcm_talk(pcm_t *p, int ms) {
  for (int i = 0; i < ms * 16; i++) {
    double t = i / 16000.0;
    pcm_add(p,
            (short)(int)(6000 * sp_sin(2 * 3.14159265358979323846 * 180 * t) +
                         3000 * sp_sin(2 * 3.14159265358979323846 * 540 * t) +
                         rnd_range(-1000, 1000)));
  }
}

static void model_tests(void) {
  char mdir[] = "/tmp/asciicam-model-XXXXXX";
  if (!mkdtemp(mdir)) {
    check(0, "--model: cannot make a scratch directory");

    return;
  }

  char model[300];
  char tokp[300];
  char cfgp[300];
  char wavp[300];
  char none[300];
  snprintf(model, sizeof model, "%s/model.safetensors", mdir);
  snprintf(tokp, sizeof tokp, "%s/tokenizer.json", mdir);
  snprintf(cfgp, sizeof cfgp, "%s/config.json", mdir);
  snprintf(wavp, sizeof wavp, "%s/s.wav", mdir);
  snprintf(none, sizeof none, "%s/none.safetensors", mdir);
  size_t fsz;
  unsigned char *wf = build_model_file(&fsz);
  write_file(model, wf, fsz);
  free(wf);

  static char tok_json[4096];
  size_t tn = (size_t)snprintf(
      tok_json, sizeof tok_json,
      "{\"model\":{\"vocab\":{\"<pad>\":0,\"<s>\":1,\"</s>\":2");
  for (int i = 3; i < 50; i++) {
    tn += (size_t)snprintf(tok_json + tn, sizeof tok_json - tn,
                           ",\"\xe2\x96\x81w%d\":%d", i, i);
  }

  tn += (size_t)snprintf(tok_json + tn, sizeof tok_json - tn, "}}}");
  write_file(tokp, tok_json, tn);
  static const char cfg_json[] =
      "{\"encoder_config\": {\"hidden_size\": 32, \"intermediate_size\": 64, "
      "\"num_hidden_layers\": 3, \"num_attention_heads\": 2, "
      "\"sliding_windows\": [[5, 4], [5, 0], [4, 3]]}, \"vocab_size\": 50, "
      "\"hidden_size\": 32, \"intermediate_size\": 48, "
      "\"num_hidden_layers\": 2, \"num_attention_heads\": 2, "
      "\"bos_token_id\": 1, \"eos_token_id\": 2}";
  write_file(cfgp, cfg_json, sizeof cfg_json - 1);

  rnd_s = 5;
  pcm_t sentences = pcm_of_quiet(800);
  pcm_t part = {0};
  pcm_talk(&part, 2500);
  pcm_append(&sentences, &part);
  free(part.s);
  part = pcm_of_quiet(1000);
  pcm_append(&sentences, &part);
  free(part.s);
  part = (pcm_t){0};
  pcm_talk(&part, 1500);
  pcm_append(&sentences, &part);
  free(part.s);
  part = pcm_of_quiet(900);
  pcm_append(&sentences, &part);
  free(part.s);
  write_wav(wavp, &sentences);

  proc_t r = proc_run(A("--listen", wavp, "--model", model, NULL), NULL, NULL,
                      NULL, 0, 60);
  char *lines[32];
  char *copy = strdup((char *)r.out.data ? (char *)r.out.data : "");
  int nl = split_lines(copy, lines, 32);
  int nfinal = 0, all_w = 1;
  for (int i = 0; i < nl; i++) {
    if (lines[i][0] != '~') {
      nfinal++;
      all_w &= lines[i][0] == 'w';
    }
  }

  check(r.code == 0 && nfinal == 2 && all_w,
        "--model: two sentences from two utterances: %d lines, %s", nl,
        one_line(&r.err));
  pty_buf first_out = r.out;
  free(r.err.data);

  size_t wn;
  unsigned char *wb = wav_bytes(&sentences, &wn);
  r = proc_run(A("--listen", "-", "--model", model, NULL), NULL, NULL, wb, wn,
               60);
  check(r.out.len == first_out.len &&
            (!r.out.len || !memcmp(r.out.data, first_out.data, r.out.len)),
        "--model: stdin gives the same lines");
  free(wb);
  free(r.out.data);
  free(r.err.data);
  free(first_out.data);
  free(copy);

  // A wrong config is refused with reason
  write_file(cfgp, "{\"hidden_size\": 33}", 19);
  r = proc_run(A("--listen", wavp, "--model", model, NULL), NULL, NULL, NULL, 0,
               30);
  check(r.code == 1 && contains(r.err.data, r.err.len, "--model") &&
            r.out.len == 0,
        "bad config refused: %s", one_line(&r.err));
  free(r.out.data);
  free(r.err.data);

  // No config: tiny shape, which these weights are not
  unlink(cfgp);
  r = proc_run(A("--listen", wavp, "--model", model, NULL), NULL, NULL, NULL, 0,
               30);
  check(r.code == 1 && contains(r.err.data, r.err.len, "wrong shape"),
        "shape mismatch named: %s", one_line(&r.err));
  free(r.out.data);
  free(r.err.data);
  r = proc_run(A("--listen", wavp, "--model", none, NULL), NULL, NULL, NULL, 0,
               30);
  check(r.code == 1 && contains(r.err.data, r.err.len, "cannot open"),
        "missing weights");
  free(r.out.data);
  free(r.err.data);
  free(sentences.s);
  rm_rf(mdir);
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

  if (want(argc, argv, "curve")) {
    curve_tests();
  }

  if (want(argc, argv, "voice")) {
    speech_tests();
  }

  if (want(argc, argv, "model")) {
    model_tests();
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
