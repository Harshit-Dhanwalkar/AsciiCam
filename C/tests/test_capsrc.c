/*
 * tests/test_capsrc.c - caption lines from a recogniser (src/capsrc.c)
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_capsrc.c src/capsrc.c lib/nl_*.c \
 *     -Iinclude -Ilib -o /tmp/tcap
 */

#include "../lib/nolibc.h"
#include "capsrc.h"
#include "platform.h"

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

static int nl_strstr_pos(const char *hay, const char *needle) {
  size_t nn = nl_strlen(needle);
  size_t hn = nl_strlen(hay);
  for (size_t i = 0; i + nn <= hn; i++) {
    size_t k = 0;
    while (k < nn && hay[i + k] == needle[k]) {
      k++;
    }

    if (k == nn) {
      return (int)i;
    }
  }

  return -1;
}

typedef struct {
  int n;
  int final[16];
  char text[16][CAPSRC_LINE + 16];
} got_t;

static void take(void *user, int final, const char *text) {
  got_t *g = user;
  if (g->n < 16) {
    g->final[g->n] = final;
    nl_strncpy_safe(g->text[g->n], text, CAPSRC_LINE + 16);
  }

  g->n++;
}

static void feed(capsrc_t *c, got_t *g, const char *s) {
  capsrc_feed(c, s, nl_strlen(s), take, g);
}

int main(void) {
  capsrc_t c;
  got_t g = {0};
  capsrc_init(&c);
  CHECK(c.fd == -1, "starts closed");

  feed(&c, &g, "hello world\n");
  CHECK(g.n == 1 && g.final[0] == 1 && nl_strcmp(g.text[0], "hello world") == 0,
        "a line is a final caption");

  feed(&c, &g, "~hello wor\n");
  CHECK(g.n == 2 && g.final[1] == 0 && nl_strcmp(g.text[1], "hello wor") == 0,
        "~ marks a partial one and is not part of the text");

  feed(&c, &g, "one\ntwo\n~three\n");
  CHECK(g.n == 5 && nl_strcmp(g.text[2], "one") == 0 &&
            nl_strcmp(g.text[3], "two") == 0 && g.final[4] == 0,
        "several lines in one read");

  int before = g.n;
  feed(&c, &g, "spl");
  CHECK(g.n == before, "no newline: nothing yet");
  feed(&c, &g, "it li");
  feed(&c, &g, "ne\n");
  CHECK(g.n == before + 1 && nl_strcmp(g.text[before], "split line") == 0,
        "a line split over reads is put together");

  before = g.n;
  feed(&c, &g, "\n   \n\r\n~\n~   \n");
  CHECK(g.n == before, "blank lines and an empty hypothesis are ignored");

  feed(&c, &g, "  padded text \r\n");
  CHECK(g.n == before + 1 && nl_strcmp(g.text[before], "padded text") == 0,
        "blanks at the ends and CR are dropped");

  before = g.n;
  char longline[CAPSRC_LINE * 3];
  nl_memset(longline, 'x', sizeof(longline));
  longline[sizeof(longline) - 2] = '\n';
  longline[sizeof(longline) - 1] = 0;
  feed(&c, &g, longline);
  CHECK(g.n == before + 1 && nl_strlen(g.text[before]) == CAPSRC_LINE,
        "a long line is cut to %d (%zu)", CAPSRC_LINE,
        nl_strlen(g.text[before]));
  feed(&c, &g, "after\n");
  CHECK(g.n == before + 2 && nl_strcmp(g.text[before + 1], "after") == 0,
        "the line after a long one is not harmed");

  // File and fifo
  const char *path = "/tmp/capsrc_test.txt";
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(fd >= 0, "temp file");
  const char body[] = "first\n~second partial\nthird\n";
  CHECK(write(fd, body, sizeof(body) - 1) == (ssize_t)sizeof(body) - 1,
        "temp file written");
  close(fd);
  capsrc_t f;
  got_t gf = {0};
  CHECK(capsrc_open(&f, path) == 0 && f.fd >= 0, "file opens");
  CHECK(capsrc_poll(&f, take, &gf) == (long)sizeof(body) - 1 && gf.n == 3 &&
            gf.final[1] == 0 && nl_strcmp(gf.text[2], "third") == 0,
        "poll reads the lines (%d)", gf.n);
  CHECK(capsrc_poll(&f, take, &gf) == 0 && gf.n == 3,
        "poll at end of file: nothing more, does not block");
  capsrc_close(&f);
  CHECK(f.fd == -1 && capsrc_poll(&f, take, &gf) == 0,
        "closed source is quiet");
  CHECK(capsrc_open(&f, "/tmp/no/such/capsrc") == -1, "missing path refused");
  unlink(path);

  // On-screen view
  capview_t v;
  capview_init(&v);
  char out[2048];
  CHECK(capview_count(&v) == 0 &&
            capview_render(&v, 30, 1, 80, 1, out, sizeof(out)) == 0,
        "nothing to show, nothing drawn");
  capview_push(&v, 0, "hel", 1000);
  capview_push(&v, 0, "hello wor", 1100);
  CHECK(capview_count(&v) == 1 && nl_strcmp(v.text[0], "hello wor") == 0,
        "a partial line replaces the partial line before it");
  capview_push(&v, 1, "hello world", 1200);
  CHECK(capview_count(&v) == 1 && v.final[0] == 1 &&
            nl_strcmp(v.text[0], "hello world") == 0,
        "the final line replaces its partial one");
  capview_push(&v, 0, "second", 1300);
  CHECK(capview_count(&v) == 2 && nl_strcmp(v.text[0], "hello world") == 0 &&
            nl_strcmp(v.text[1], "second") == 0,
        "a new line goes below a final one");
  capview_push(&v, 1, "second line", 1400);
  capview_push(&v, 1, "third", 1500);
  CHECK(capview_count(&v) == 2 && nl_strcmp(v.text[0], "second line") == 0 &&
            nl_strcmp(v.text[1], "third") == 0,
        "only the last two lines are kept, oldest first");

  int n = capview_render(&v, 30, 5, 40, 1, out, sizeof(out));
  CHECK(n > 0 && nl_strstr_pos(out, "\033[29;5H") >= 0 &&
            nl_strstr_pos(out, "\033[30;5H") >= 0,
        "two bars on the rows above and at the bottom row given");
  CHECK(nl_strstr_pos(out, " second line") >= 0 &&
            nl_strstr_pos(out, " third") >= 0 &&
            nl_strstr_pos(out, "\033[1m") >= 0,
        "text drawn, final lines bold");
  capview_push(&v, 0, "guess", 1600);
  n = capview_render(&v, 30, 5, 40, 0, out, sizeof(out));
  CHECK(n > 0 && nl_strstr_pos(out, "\033[2m guess") >= 0 &&
            nl_strstr_pos(out, "\033[7m") >= 0,
        "a guess is dim; no colour: reverse video");
  capview_init(&v);
  capview_push(&v, 1, "a very long caption line that does not fit in ten", 0);
  n = capview_render(&v, 30, 1, 20, 1, out, sizeof(out));
  CHECK(n > 0 && nl_strstr_pos(out, "fit in ten") >= 0 &&
            nl_strstr_pos(out, "a very long") < 0,
        "a long line shows its end");
  int bars = 0;
  for (int i = 0; i < n; i++) {
    bars += out[i] == '\033' && out[i + 1] == '[' && out[i + 2] == '3' &&
            out[i + 3] == '0' && out[i + 4] == ';';
  }

  CHECK(bars == 1, "one line is one bar (%d)", bars);
  CHECK(capview_render(&v, 1, 1, 20, 1, out, sizeof(out)) == 0 &&
            capview_render(&v, 30, 1, 4, 1, out, sizeof(out)) == 0 &&
            capview_render(&v, 30, 0, 20, 1, out, sizeof(out)) == 0 &&
            capview_render(&v, 30, 1, 20, 1, out, 10) == 0,
        "no room, too narrow, bad column, small buffer: nothing");

  capview_init(&v);
  capview_push(&v, 1, "old final", 1000);
  capview_push(&v, 0, "guess", 5000);
  CHECK(capview_tick(&v, 6900) == 0 && capview_count(&v) == 2,
        "nothing gone yet (final 5.9 s, partial 1.9 s)");
  CHECK(capview_tick(&v, 7100) == 1 && capview_count(&v) == 1 &&
            nl_strcmp(v.text[0], "guess") == 0,
        "a final goes after 6 s, the younger line stays and moves up");
  CHECK(capview_tick(&v, 8100) == 1 && capview_count(&v) == 0,
        "a guess goes after 3 s");
  CHECK(capview_tick(&v, 9000) == 0, "ticking an empty view changes nothing");

  // Default fifo
  char dp[128];
  CHECK(capsrc_default_path(dp, sizeof(dp), "/run/user/1000", "/home/x") == 0 &&
            nl_strcmp(dp, "/run/user/1000/asciicam-captions") == 0,
        "runtime dir first");
  CHECK(capsrc_default_path(dp, sizeof(dp), NULL, "/home/x") == 0 &&
            nl_strcmp(dp, "/home/x/.asciicam-captions") == 0,
        "then home");
  CHECK(capsrc_default_path(dp, sizeof(dp), "relative", "relative") == -1 &&
            capsrc_default_path(dp, sizeof(dp), NULL, NULL) == -1 &&
            capsrc_default_path(dp, 8, "/run/user/1000", NULL) == -1,
        "no absolute place, or no room: refused");

  const char *fifo = "/tmp/capsrc_test.fifo";
  unlink(fifo);
  CHECK(capsrc_make_fifo(fifo) == 0 && capsrc_make_fifo(fifo) == 0,
        "fifo made, and again is fine");
  capsrc_t ff;
  got_t gg = {0};
  CHECK(capsrc_open(&ff, fifo) == 0, "fifo opens");
  CHECK(capsrc_poll(&ff, take, &gg) == 0 && gg.n == 0,
        "empty fifo: no block, no end");
  int wfd = open(fifo, O_WRONLY | 04000);
  CHECK(wfd >= 0 && write(wfd, "~hi\nhi there\n", 13) == 13, "writer side");
  CHECK(capsrc_poll(&ff, take, &gg) == 13 && gg.n == 2 && gg.final[0] == 0 &&
            nl_strcmp(gg.text[1], "hi there") == 0,
        "lines come through the fifo");
  close(wfd);
  CHECK(capsrc_poll(&ff, take, &gg) == 0, "writer gone: the fifo stays open");
  capsrc_close(&ff);
  unlink(fifo);
  CHECK(capsrc_make_fifo("/no/such/dir/fifo") == -1,
        "fifo in a missing directory fails");

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
