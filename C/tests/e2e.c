/*
 * tests/e2e.c - end-to-end checks of main loop
 *
 *   cc -O1 -o build/tests/e2e tests/e2e.c
 *   build/tests/e2e build/tests/asciicam_fake [build/asciicam]
 *
 * Runs program in a pseudo-terminal against software camera
 * (tests/fake_capture.c) and checks what it draws
 */

#include "pty_harness.h"

#include <stdarg.h>
#include <stdio.h>

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

/* Output helpers */

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
// ESC[?2026h ESC[ y;x H (or just H for 1,1). Last one wins
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

  smoke();
  stalled_camera();
  mouse_tests();

  if (failures) {
    printf("e2e (C): %d FAILED\n", failures);

    return 1;
  }

  printf("e2e (C): all passed\n");

  return 0;
}
