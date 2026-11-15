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

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}

