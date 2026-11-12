/*
 * tests/test_slides.c - src/slides.c: picture names, natural order, folder
 * scan, stepping with wrap, timer, requests, bad pictures
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_slides.c src/slides.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c lib/nl_mem.c lib/nl_gc.c \
 *     -Iinclude -Ilib -o /tmp/ts
 */

#include "../lib/nolibc.h"
#include "platform.h"
#include "slides.h"

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

#define DIR "build/tests/sl_tmp"

static int same(const char *a, const char *b) { return nl_strcmp(a, b) == 0; }

static void touch(const char *name) {
  char p[256];
  nl_snprintf(p, sizeof(p), DIR "/%s", name);
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    close(fd);
  }
}

static void rm(const char *name) {
  char p[256];
  nl_snprintf(p, sizeof(p), DIR "/%s", name);
  unlink(p);
}

static const char *const files[] = {"img10.png", "img2.PNG",  "img1.jpg",
                                    "notes.txt", "a.ppm",     ".hidden.png",
                                    "Z.jpeg",    "b.pgm",     "c.pnm",
                                    "photo",     "x.png.bak", "img02.png"};

int main(void) {
  // extensions
  CHECK(slides_is_picture("a.png") && slides_is_picture("A.PNG") &&
            slides_is_picture("x.y.Jpeg") && slides_is_picture("a.jpg") &&
            slides_is_picture("a.pgm") && slides_is_picture("a.ppm") &&
            slides_is_picture("a.pnm"),
        "picture extensions, any case");
  CHECK(!slides_is_picture("a.txt") && !slides_is_picture("png") &&
            !slides_is_picture("a.") && !slides_is_picture("") &&
            !slides_is_picture("a.png.bak") && !slides_is_picture("a.pngg") &&
            !slides_is_picture("a.gif"),
        "other names are not pictures");

  // Natural order
  CHECK(slides_natural_cmp("img2.png", "img10.png") < 0, "2 before 10");
  CHECK(slides_natural_cmp("img10.png", "img2.png") > 0, "10 after 2");
  CHECK(slides_natural_cmp("a", "B") < 0 && slides_natural_cmp("B", "a") > 0,
        "letters without case");
  CHECK(slides_natural_cmp("a", "a") == 0, "equal names");
  CHECK(slides_natural_cmp("a", "A") != 0, "case-only difference still orders");
  CHECK(slides_natural_cmp("img", "img1") < 0, "prefix first");
  CHECK(slides_natural_cmp("img2", "img02") < 0 &&
            slides_natural_cmp("img02", "img2") > 0,
        "same value: fewer zeros first");
  CHECK(slides_natural_cmp("img09", "img10") < 0, "09 before 10");
  CHECK(slides_natural_cmp("1000000000000000000000", "999999999999999999999") >
            0,
        "long digit runs do not overflow");
  CHECK(slides_natural_cmp("a1b2", "a1b10") < 0, "two digit runs");

  // Nothing open yet
  slides_close();
  char path[256];
  CHECK(!slides_active() && slides_count() == 0 && slides_tick(0) == 0 &&
            slides_path(path, sizeof(path)) != 0 && slides_name(0) == NULL,
        "closed: nothing happens");
  CHECK(slides_open("build/tests/sl_nope") == -1, "missing folder: -1");

  // Folder
  mkdir("build", 0755);
  mkdir("build/tests", 0755);
  mkdir(DIR, 0755);
  mkdir(DIR "/sub.png", 0755); // a folder named like a picture
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    touch(files[i]);
  }

  CHECK(slides_open(DIR "/img1.jpg") == -1, "a file is not a folder");
  // a.ppm b.pgm c.pnm img1.jpg img2.PNG img02.png img10.png Z.jpeg; sub.png is a folder
  int n = slides_open(DIR);
  CHECK(n == 8, "8 pictures (got %d)", n);
  CHECK(slides_active() && slides_count() == n, "active");
  int ok = 1;
  for (int i = 0; i + 1 < n; i++) {
    ok = ok && slides_natural_cmp(slides_name(i), slides_name(i + 1)) < 0;
  }

  CHECK(ok, "list is sorted");
  CHECK(same(slides_name(0), "a.ppm") && same(slides_name(n - 1), "Z.jpeg"),
        "first a.ppm, last Z.jpeg (%s, %s)", slides_name(0),
        slides_name(n - 1));
  int saw_hidden = 0, saw_txt = 0, saw_sub = 0;
  for (int i = 0; i < n; i++) {
    saw_hidden |= slides_name(i)[0] == '.';
    saw_txt |= same(slides_name(i), "notes.txt");
    saw_sub |= same(slides_name(i), "sub.png");
  }

  CHECK(!saw_hidden && !saw_txt && !saw_sub,
        "hidden files, other files and folders are left out");
  CHECK(slides_name(-1) == NULL && slides_name(n) == NULL, "name bounds");

  CHECK(slides_path(path, sizeof(path)) == 0 && same(path, DIR "/a.ppm"),
        "path of the first one: %s", path);
  char tiny[8];
  CHECK(slides_path(tiny, sizeof(tiny)) != 0, "path too long for buffer");

  // Trailing slash does not double
  slides_close();
  CHECK(slides_open(DIR "/") == n, "folder with a slash");
  CHECK(slides_path(path, sizeof(path)) == 0 && same(path, DIR "/a.ppm"),
        "no double slash: %s", path);

  // Timer: 5 s default
  slides_close();
  slides_open(DIR);
  CHECK(slides_tick(1000) == 0 && slides_index() == 0, "first tick: stays");
  CHECK(slides_tick(5999) == 0 && slides_index() == 0, "just before 5 s");
  CHECK(slides_tick(6000) == 1 && slides_index() == 1, "at 5 s: next");
  CHECK(slides_take_changed() == 1 && slides_take_changed() == 0,
        "changed reported once");
  CHECK(slides_tick(10999) == 0, "timer restarts at the change");
  CHECK(slides_tick(11000) == 1 && slides_index() == 2, "again");
  slides_set_secs(1);
  CHECK(slides_tick(11999) == 0 && slides_tick(12000) == 1, "1 s per picture");
  slides_set_secs(0);
  CHECK(slides_tick(99999999) == 0, "0 s: never on its own");
  slides_set_secs(-3);
  CHECK(slides_tick(99999999) == 0, "negative seconds count as 0");

  // Requests
  int at = slides_index();
  slides_request(1);
  CHECK(slides_tick(100) == 1 && slides_index() == at + 1, "request +1");
  slides_request(-1);
  CHECK(slides_tick(100) == 1 && slides_index() == at, "request -1");
  slides_request(1);
  slides_request(1);
  CHECK(slides_tick(100) == 1 && slides_index() == at + 2,
        "two requests add up");
  slides_request(1);
  slides_request(-1);
  CHECK(slides_tick(100) == 0 && slides_index() == at + 2,
        "opposite requests cancel");
  slides_request(n);
  CHECK(slides_tick(100) == 0 && slides_index() == at + 2,
        "a whole lap is the same picture");
  slides_request(-(2 * n + 1));
  CHECK(slides_tick(100) == 1 && slides_index() == at + 1,
        "many laps backwards");

  // Wrap
  slides_close();
  slides_open(DIR);
  slides_request(-1);
  slides_tick(0);
  CHECK(slides_index() == n - 1, "back from the first: the last");
  slides_request(1);
  slides_tick(0);
  CHECK(slides_index() == 0, "forward from the last: the first");

  // Bad pictures are skipped for good
  slides_close();
  slides_open(DIR);
  slides_set_secs(5);
  CHECK(slides_mark_bad(0) == 1 && slides_index() == 1, "bad first: on to 1");
  slides_request(-1);
  slides_tick(0);
  CHECK(slides_index() == n - 1, "stepping back skips the bad one");
  slides_request(1);
  slides_tick(0);
  CHECK(slides_index() == 1, "and forward");
  for (int i = 0; i < n - 2; i++) {
    CHECK(slides_mark_bad(0) == 1, "another one left (%d)", i);
  }

  CHECK(slides_mark_bad(0) == 0, "last one bad: nothing left");
  slides_request(1);
  CHECK(slides_tick(0) == 0, "all bad: requests change nothing");

  // Reopening keeps place (capture resize), another folder starts over
  slides_close();
  slides_open(DIR);
  slides_request(3);
  slides_tick(0);
  CHECK(slides_index() == 3, "moved to 3");
  CHECK(slides_open(DIR) == n && slides_index() == 3,
        "same folder again: place kept");
  CHECK(slides_open(DIR "/sub.png") == 0 && slides_index() == 0 &&
            !slides_active(),
        "empty folder: no pictures");
  slides_close();

  // A request that arrives while the list is closed does nothing later
  slides_request(5);
  slides_open(DIR);
  CHECK(slides_tick(0) == 1 || slides_index() == 0, "stale request is fine");
  slides_close();

  // a full folder
  mkdir(DIR "/many", 0755);
  for (int i = 0; i < SLIDES_MAX + 20; i++) {
    char nm[64];
    nl_snprintf(nm, sizeof(nm), "many/p%04d.png", i);
    touch(nm);
  }

  CHECK(slides_open(DIR "/many") == SLIDES_MAX, "list is capped at SLIDES_MAX");
  slides_close();

  // A long name is left out, not cut
  {
    char nm[200];
    nl_memset(nm, 'a', sizeof(nm));
    nm[190] = '\0';
    nl_strncpy_safe(nm + 150, ".png", 5);
    // 154 chars + ".png" from index 150: name is 154 long
    char full[256];
    nl_snprintf(full, sizeof(full), "long/%s", nm);
    mkdir(DIR "/long", 0755);
    touch(full);
    touch("long/short.png");
    CHECK(slides_open(DIR "/long") == 1 && same(slides_name(0), "short.png"),
          "name longer than 127 is left out");
    slides_close();
  }

  // Clean up
  for (int i = 0; i < SLIDES_MAX + 20; i++) {
    char nm[64];
    nl_snprintf(nm, sizeof(nm), "many/p%04d.png", i);
    rm(nm);
  }

  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    rm(files[i]);
  }

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
