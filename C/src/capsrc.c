/*
 * src/capsrc.c - caption lines from a file or fifo
 */

#include "platform.h"

#include "nolibc.h"

#include "capsrc.h"

#define CAPSRC_O_NONBLOCK 04000

void capsrc_init(capsrc_t *c) {
  nl_memset(c, 0, sizeof(*c));
  c->fd = -1;
}

static void finish(capsrc_t *c, capsrc_fn fn, void *user) {
  c->buf[c->n] = 0;
  const char *s = c->buf;
  int final = 1;
  if (s[0] == '~') {
    final = 0;
    s++;
  }

  while (*s == ' ') {
    s++;
  }

  // Trailing blanks
  size_t len = nl_strlen(s);
  while (len > 0 && s[len - 1] == ' ') {
    len--;
  }

  if (len > 0) {
    char line[CAPSRC_LINE + 1];
    nl_memcpy(line, s, len);
    line[len] = 0;
    fn(user, final, line);
  }

  c->n = 0;
  c->cut = 0;
}

void capsrc_feed(capsrc_t *c, const char *data, size_t n, capsrc_fn fn,
                 void *user) {
  for (size_t i = 0; i < n; i++) {
    char ch = data[i];
    if (ch == '\n') {
      finish(c, fn, user);
    } else if (ch == '\r') {
      continue;
    } else if (c->n < CAPSRC_LINE) {
      c->buf[c->n++] = ch;
    } else {
      c->cut = 1; // rest of a long line is dropped
    }
  }
}

int capsrc_open(capsrc_t *c, const char *path) {
  capsrc_init(c);
  int fd = open(path, O_RDWR | CAPSRC_O_NONBLOCK);
  if (fd < 0) {
    fd = open(path, O_RDONLY | CAPSRC_O_NONBLOCK);
  }

  if (fd < 0) {
    return -1;
  }

  c->fd = fd;

  return 0;
}

long capsrc_poll(capsrc_t *c, capsrc_fn fn, void *user) {
  if (c->fd < 0) {
    return 0;
  }

  long total = 0;
  char tmp[1024] = {0};
  for (int rounds = 0; rounds < 8; rounds++) {
    ssize_t r = read(c->fd, tmp, sizeof(tmp));
    if (r <= 0) {
      break;
    }

    capsrc_feed(c, tmp, (size_t)r, fn, user);
    total += r;
  }

  return total;
}

void capsrc_close(capsrc_t *c) {
  if (c->fd >= 0) {
    close(c->fd);
  }

  c->fd = -1;
}
