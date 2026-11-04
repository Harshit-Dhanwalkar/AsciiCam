/*
 * tests/test_browser.c - src/browser.c captures panel: directory scan (order,
 * filters, limits), key handling, rendering bounds, and screen takeovers:
 * .ansi preview and .cast replay, latter fed by files written by recorder.c
 * so writer's escaping is read back by reader's parser
 *
 * Linux nolibc only: stdout/stdin are swapped with dup2 and a pipe so takeovers
 * can run without a terminal
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_browser.c src/browser.c src/recorder.c \
 *     lib/nl_printf.c lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c -Iinclude \
 *     -Ilib -o /tmp/tb
 */

#include "platform.h"

#include "../lib/nolibc.h"
#include "browser.h"
#include "nl_thread.h"
#include "recorder.h"

#ifndef SYS_pipe
#define SYS_pipe 22
#endif

static int pass_n, fail_n;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

#define DIR "build/tests/br_tmp"

static int same(const char *a, const char *b) { return nl_strcmp(a, b) == 0; }

static void touch(const char *name, const char *text) {
  char p[256];
  nl_snprintf(p, sizeof(p), DIR "/%s", name);
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    ssize_t w = write(fd, text, nl_strlen(text));
    (void)w;
    close(fd);
  }
}

static void rm(const char *name) {
  char p[256];
  nl_snprintf(p, sizeof(p), DIR "/%s", name);
  unlink(p);
}

static long long now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int has(const char *hay, size_t hn, const char *needle) {
  size_t nl = nl_strlen(needle);
  for (size_t i = 0; i + nl <= hn; i++) {
    size_t k = 0;
    while (k < nl && hay[i + k] == needle[k]) {
      k++;
    }

    if (k == nl) {
      return 1;
    }
  }

  return 0;
}

// Running a takeover with stdin/stdout replaced
static char out_file[] = DIR "/captured.out";
static int key_pipe_w = -1;
static long long key_delay_ns;
static char key_byte;
static int
    key_twice; // second key 300 ms after first (abort, then "return")

static void *key_feeder(void *arg) {
  (void)arg;
  nl_usleep((unsigned long)(key_delay_ns / 1000));
  ssize_t w = write(key_pipe_w, &key_byte, 1);
  if (key_twice) {
    nl_usleep(300000);
    w = write(key_pipe_w, &key_byte, 1);
  }

  (void)w;

  return (void *)0;
}

typedef int (*takeover_fn)(const char *, int *);

// Run fn(path) with stdout going to out_file and stdin fed one key after
// key_ms. Returns fn's result; *aborted and *elapsed_ms are set
static int run_takeover(takeover_fn fn, const char *path, long key_ms,
                        int *aborted, long *elapsed_ms) {
  int fds[2];
  __sc1(SYS_pipe, (long)fds);
  int out = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int save_in = (int)__sc1(32 /* dup */, 0);
  int save_out = (int)__sc1(32, 1);
  __sc2(SYS_dup2, fds[0], 0);
  __sc2(SYS_dup2, out, 1);

  // non-blocking stdin like terminal setup in main (VMIN=0): a read with
  // nothing pending must return at once
  long fl = __sc3(72 /* fcntl */, 0, 3 /* F_GETFL */, 0);
  __sc3(72, 0, 4 /* F_SETFL */, fl | 04000);

  key_pipe_w = fds[1];
  key_delay_ns = key_ms * 1000000LL;
  key_byte = 'x';
  nl_thread_t th;
  nl_thread_create(&th, key_feeder, (void *)0);

  long long t0 = now_ns();
  int r = fn(path, aborted);
  *elapsed_ms = (long)((now_ns() - t0) / 1000000LL);

  nl_thread_join(&th);
  __sc2(SYS_dup2, save_in, 0);
  __sc2(SYS_dup2, save_out, 1);
  close(save_in);
  close(save_out);
  close(out);
  close(fds[0]);
  close(fds[1]);

  return r;
}

static char cap[1 << 16];

static long read_out(void) {
  int fd = open(out_file, O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  long n = read(fd, cap, sizeof(cap) - 1);
  close(fd);
  if (n >= 0) {
    cap[n] = '\0';
  }

  return n;
}

// Tests
static void test_scan(void) {
  rec_make_dirs(DIR);
  touch("asciicam-20261101T000000Z.cast", "{}\n");
  touch("asciicam-20261101T120000Z.ansi", "x");
  touch("asciicam-20261102T000000Z.cast", "{}\n");
  touch("asciicam-20261103T000000Z.png", "png");
  touch(".hidden.cast", "{}\n");
  touch("notes.txt", "hi");
  touch("asciicam-20261104T000000Z.cast.tmp", "partial");

  browser_t b;
  browser_init(&b);
  CHECK(browser_scan(&b, DIR) == 0, "scan ok");
  CHECK(b.count == 4, "4 captures, others ignored: %d", b.count);
  CHECK(same(b.e[0].name, "asciicam-20261103T000000Z.png") &&
            b.e[0].type == BROWS_PNG,
        "newest first: %s", b.e[0].name);
  CHECK(same(b.e[1].name, "asciicam-20261102T000000Z.cast") &&
            b.e[1].type == BROWS_CAST,
        "then: %s", b.e[1].name);
  CHECK(same(b.e[2].name, "asciicam-20261101T120000Z.ansi") &&
            b.e[2].type == BROWS_ANSI,
        "then: %s", b.e[2].name);
  CHECK(same(b.e[3].name, "asciicam-20261101T000000Z.cast"), "oldest last");
  CHECK(same(b.e[0].path, DIR "/asciicam-20261103T000000Z.png"), "full path");
  CHECK(same(b.status, "4 files"), "status: %s", b.status);

  browser_t m;
  browser_init(&m);
  CHECK(browser_scan(&m, DIR "/no/such/dir") == -1 && m.count == 0,
        "missing dir: -1, nothing listed");
  CHECK(same(m.status, "cannot open dir"), "status says so: %s", m.status);

  // Rescanning picks up changes and resets selection
  b.selected = 3;
  touch("asciicam-20261105T000000Z.ansi", "new");
  browser_scan(&b, DIR);
  CHECK(b.count == 5 && b.selected == 0 &&
            same(b.e[0].name, "asciicam-20261105T000000Z.ansi"),
        "rescan: new file first, selection reset");

  rm("asciicam-20261101T000000Z.cast");
  rm("asciicam-20261101T120000Z.ansi");
  rm("asciicam-20261102T000000Z.cast");
  rm("asciicam-20261103T000000Z.png");
  rm("asciicam-20261105T000000Z.ansi");
  rm(".hidden.cast");
  rm("notes.txt");
  rm("asciicam-20261104T000000Z.cast.tmp");

  // Empty
  browser_scan(&m, DIR);
  CHECK(m.count == 0 && same(m.status, "no captures yet"), "empty dir: %s",
        m.status);

  // More files than panel holds
  for (int i = 0; i < BROWS_MAX_FILES + 8; i++) {
    char n[64];
    nl_snprintf(n, sizeof(n), "asciicam-20261100T%06dZ.cast", 100000 + i);
    touch(n, "{}\n");
  }

  browser_scan(&m, DIR);
  CHECK(m.count == BROWS_MAX_FILES, "capped at %d: %d", BROWS_MAX_FILES,
        m.count);
  for (int i = 0; i < BROWS_MAX_FILES + 8; i++) {
    char n[64];
    nl_snprintf(n, sizeof(n), "asciicam-20261100T%06dZ.cast", 100000 + i);
    rm(n);
  }
}

static void test_keys(void) {
  rec_make_dirs(DIR);
  touch("asciicam-20261101T000000Z.png", "p");
  touch("asciicam-20261102T000000Z.cast", "{}\n");
  touch("asciicam-20261103T000000Z.ansi", "a");

  browser_t b;
  browser_init(&b);
  browser_scan(&b, DIR);
  char path[256];

  CHECK(browser_key(&b, BR_KEY_DOWN, 0, path, sizeof(path)) == BROWS_ACT_NONE &&
            b.selected == 0,
        "inactive panel ignores keys");
  browser_toggle(&b);
  CHECK(b.active, "toggle opens");

  CHECK(browser_key(&b, BR_KEY_ENTER, 0, path, sizeof(path)) ==
                BROWS_ACT_PREVIEW_ANSI &&
            same(path, DIR "/asciicam-20261103T000000Z.ansi"),
        "enter on .ansi: preview with path");
  browser_key(&b, BR_KEY_DOWN, 0, path, sizeof(path));
  CHECK(browser_key(&b, BR_KEY_ENTER, 0, path, sizeof(path)) ==
                BROWS_ACT_REPLAY_CAST &&
            same(path, DIR "/asciicam-20261102T000000Z.cast"),
        "enter on .cast: replay with path");
  browser_key(&b, BR_KEY_CHAR, 'j', path, sizeof(path));
  CHECK(b.selected == 2, "j moves down");
  CHECK(browser_key(&b, BR_KEY_ENTER, 0, path, sizeof(path)) ==
                BROWS_ACT_INFO &&
            b.info_msg[0],
        "enter on .png: info message");
  browser_key(&b, BR_KEY_DOWN, 0, path, sizeof(path));
  browser_key(&b, BR_KEY_DOWN, 0, path, sizeof(path));
  CHECK(b.selected == 2, "down stops at the last row");
  browser_key(&b, BR_KEY_CHAR, 'g', path, sizeof(path));
  CHECK(b.selected == 0, "g: top");
  browser_key(&b, BR_KEY_UP, 0, path, sizeof(path));
  browser_key(&b, BR_KEY_CHAR, 'k', path, sizeof(path));
  CHECK(b.selected == 0, "up / k stop at the first row");
  browser_key(&b, BR_KEY_CHAR, 'G', path, sizeof(path));
  CHECK(b.selected == 2, "G: bottom");

  CHECK(browser_key(&b, BR_KEY_ESC, 0, path, sizeof(path)) == BROWS_ACT_CLOSE,
        "esc closes");
  CHECK(browser_key(&b, BR_KEY_CHAR, 'b', path, sizeof(path)) ==
            BROWS_ACT_CLOSE,
        "b closes");
  CHECK(browser_key(&b, BR_KEY_CHAR, 'q', path, sizeof(path)) ==
            BROWS_ACT_CLOSE,
        "q closes the panel (not the program)");
  CHECK(browser_key(&b, BR_KEY_CHAR, 'z', path, sizeof(path)) == BROWS_ACT_NONE,
        "other keys do nothing");

  // Empty list: enter does nothing
  browser_t e;
  browser_init(&e);
  e.active = 1;
  CHECK(browser_key(&e, BR_KEY_ENTER, 0, path, sizeof(path)) == BROWS_ACT_NONE,
        "enter with nothing listed");

  rm("asciicam-20261101T000000Z.png");
  rm("asciicam-20261102T000000Z.cast");
  rm("asciicam-20261103T000000Z.ansi");
}

static void test_render(void) {
  rec_make_dirs(DIR);
  touch("asciicam-20261101T000000Z.cast", "{}\n");
  browser_t b;
  browser_init(&b);
  browser_scan(&b, DIR);

  static char out[8192];
  CHECK(browser_render(&b, 40, 120, 1, out, sizeof(out)) == 0,
        "inactive panel draws nothing");
  browser_toggle(&b);
  int n = browser_render(&b, 40, 120, 1, out, sizeof(out));
  CHECK(n > 0 && has(out, (size_t)n, "20261101T000000Z.cast"),
        "active panel lists the file with its extension (%d bytes)", n);
  CHECK(browser_render(&b, 40, 20, 1, out, sizeof(out)) == 0,
        "terminal too narrow: nothing");
  n = browser_render(&b, 40, 120, 0, out, sizeof(out));
  CHECK(n > 0 && !has(out, (size_t)n, "38;2;"), "mono output has no truecolor");

  // Output never goes past buffer it is given
  for (size_t sz = 1; sz < 700; sz += 37) {
    static char guard[1024];
    for (size_t i = 0; i < sizeof(guard); i++) {
      guard[i] = 0x5A;
    }

    int r = browser_render(&b, 40, 120, 1, guard, sz);
    int over = 0;
    for (size_t i = sz; i < sizeof(guard); i++) {
      over |= guard[i] != 0x5A;
    }

    CHECK(!over && r >= 0 && (size_t)r <= sz, "buffer of %zu: wrote %d", sz, r);
  }

  rm("asciicam-20261101T000000Z.cast");
}

static void test_preview_and_replay(void) {
  rec_make_dirs(DIR);
  int aborted = 0;
  long ms = 0;

  // .ansi: file content reaches screen, then waits for a key
  touch("p.ansi", "\033[1mHELLO\033[0m\n");
  int r = run_takeover(browser_preview_ansi, DIR "/p.ansi", 150, &aborted, &ms);
  long n = read_out();
  CHECK(r == 0 && aborted, "preview returns 0 after the key");
  CHECK(ms >= 120 && ms < 3000, "preview waited for the key: %ld ms", ms);
  CHECK(n > 0 && has(cap, (size_t)n, "\033[1mHELLO\033[0m"),
        "preview shows the file");
  CHECK(n > 0 && has(cap, (size_t)n, "press any key"), "preview has a prompt");
  CHECK(browser_preview_ansi(DIR "/missing.ansi", &aborted) == -1,
        "missing file: -1");
  rm("p.ansi");

  // .cast written by recorder, replayed through browser's parser
  recorder_t rec;
  rec_init(&rec);
  CHECK(rec_start(&rec, DIR, 80, 24, 1000000000, 0) == 0, "record fixture");
  static const char f1[] =
      "\033[H\033[38;2;1;2;3m\"q\"\\ \xe2\xa0\x81 tab\t\r\n";
  static const char f2[] = "second \xe2\x96\x88 frame\033[0m";
  rec_frame(&rec, 0, f1, sizeof(f1) - 1);
  rec_frame(&rec, 120000000ull, f2, sizeof(f2) - 1);
  char cast_path[REC_PATH_LEN];
  nl_snprintf(cast_path, sizeof(cast_path), "%s", rec.path);
  rec_stop(&rec, 200000000ull, NULL);

  r = run_takeover(browser_replay_cast, cast_path, 500, &aborted, &ms);
  n = read_out();
  CHECK(r == 0, "replay returns 0");

  CHECK(n > 0 && has(cap, (size_t)n, f1), "frame 1 comes back byte for byte");
  CHECK(n > 0 && has(cap, (size_t)n, f2), "frame 2 comes back byte for byte");
  const char *p1 = cap;
  size_t i1 = 0, i2 = 0;
  for (size_t i = 0; i + sizeof(f1) - 1 <= (size_t)n && !i1; i++) {
    if (has(cap + i, sizeof(f1) - 1, f1)) {
      i1 = i + 1;
    }
  }

  for (size_t i = 0; i + sizeof(f2) - 1 <= (size_t)n && !i2; i++) {
    if (has(cap + i, sizeof(f2) - 1, f2)) {
      i2 = i + 1;
    }
  }

  (void)p1;
  CHECK(i1 && i2 && i1 < i2, "frames in recorded order");
  CHECK(ms >= 450, "replay waits for a key at the end: %ld ms", ms);

  // a key during playback aborts it quickly
  CHECK(rec_start(&rec, DIR, 80, 24, 1000000005, 0) == 0, "record long one");
  char long_path[REC_PATH_LEN];
  nl_snprintf(long_path, sizeof(long_path), "%s", rec.path);
  rec_frame(&rec, 0, "first", 5);
  rec_frame(&rec, 8000000000ull, "late", 4); // 8 s in
  rec_stop(&rec, 9000000000ull, NULL);
  key_twice = 1; // first key aborts playback, second leaves screen
  r = run_takeover(browser_replay_cast, long_path, 200, &aborted, &ms);
  key_twice = 0;
  n = read_out();
  CHECK(aborted && ms < 3000, "key aborts replay: aborted=%d, %ld ms", aborted,
        ms);
  CHECK(n > 0 && has(cap, (size_t)n, "first") && !has(cap, (size_t)n, "late"),
        "events after the abort are not shown");

  CHECK(browser_replay_cast(DIR "/missing.cast", &aborted) == -1,
        "missing cast: -1");

  // Same events without spaces, and with extra spaces (both are legal JSON)
  touch("sp.cast", "{\"version\":2,\"width\":80,\"height\":24}\n"
                   "[0.0,\"o\",\"compact\"]\n"
                   "[0.05 ,  \"o\" ,  \"spaced\"  ]\n"
                   "[0.06, \"i\", \"typed\"]\n"
                   "[0.07, \"o\", \"last\"]");
  r = run_takeover(browser_replay_cast, DIR "/sp.cast", 300, &aborted, &ms);
  n = read_out();
  CHECK(r == 0 && n > 0 && has(cap, (size_t)n, "compact") &&
            has(cap, (size_t)n, "spaced") && has(cap, (size_t)n, "last"),
        "spacing variants and a last line without newline all play");
  CHECK(!has(cap, (size_t)n, "typed"), "input events are not drawn");
  CHECK(has(cap, (size_t)n, "(3 events)"), "three output events counted");
  rm("sp.cast");

  // Damaged files must not crash or hang (a key is waiting at end)
  static const char *bad[] = {
      "",                                              // empty
      "{\"version\": 2}\n",                            // header only
      "{\"version\": 2}\n[0.1, \"o\", \"unterminated", // cut off, no \n
      "{\"version\": 2}\n[0.1, \"o\", \"bad \\q escape\"]\n",
      "{\"version\": 2}\n[0.1, \"o\", \"\\u12\"]\n",   // short \u
      "{\"version\": 2}\n[0.1, \"o\", \"\\uZZZZ\"]\n", // not hex
      "{\"version\": 2}\n[nonsense]\n[1e999, \"o\", \"x\"]\n",
      "{\"version\": 2}\n[-5, \"o\", \"neg\"]\n[0, \"i\", \"input\"]\n",
      "\n\n\n\n",
  };
  for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
    touch("bad.cast", bad[k]);
    r = run_takeover(browser_replay_cast, DIR "/bad.cast", 150, &aborted, &ms);
    CHECK(ms < 4000, "damaged cast #%zu handled in %ld ms (r=%d)", k, ms, r);
  }

  // One very long frame line
  {
    int fd = open(DIR "/bad.cast", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    static char chunk[4096];
    for (size_t i = 0; i < sizeof(chunk); i++) {
      chunk[i] = 'a';
    }

    ssize_t w = write(fd, "{\"version\": 2}\n[0.0, \"o\", \"", 25);
    (void)w;
    for (int i = 0; i < 300; i++) { // 1.2 MB line: longer than read buffer
      w = write(fd, chunk, sizeof(chunk));
    }

    w = write(fd, "\"]\n", 3);
    close(fd);
    r = run_takeover(browser_replay_cast, DIR "/bad.cast", 150, &aborted, &ms);
    CHECK(ms < 4000, "over-long line handled in %ld ms (r=%d)", ms, r);
  }

  rm("bad.cast");
  unlink(cast_path);
  unlink(long_path);
  rm("captured.out");
}

// Files left by an earlier run that was killed would change every count
static void clean_dir(void) {
  rec_make_dirs(DIR);
  static browser_t b;
  browser_init(&b);
  if (browser_scan(&b, DIR) == 0) {
    for (int i = 0; i < b.count; i++) {
      unlink(b.e[i].path);
    }
  }

  rm("bad.cast");
  rm("sp.cast");
  rm("p.ansi");
  rm("captured.out");
}

int main(void) {
  clean_dir();
  test_scan();
  test_keys();
  test_render();
  test_preview_and_replay();

  fprintf(stderr, "%d passed %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
