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

void capview_init(capview_t *v) { nl_memset(v, 0, sizeof(*v)); }

void capview_push(capview_t *v, int final, const char *text, long now_ms) {
  int slot;
  if (v->n > 0 && !v->final[v->n - 1]) {
    slot = v->n - 1; // guess before is replaced
  } else if (v->n < CAPVIEW_ROWS) {
    slot = v->n++;
  } else {
    for (int i = 1; i < CAPVIEW_ROWS; i++) {
      nl_memcpy(v->text[i - 1], v->text[i], sizeof(v->text[0]));
      v->final[i - 1] = v->final[i];
      v->at[i - 1] = v->at[i];
    }

    slot = CAPVIEW_ROWS - 1;
  }

  nl_strncpy_safe(v->text[slot], text, sizeof(v->text[0]));
  v->final[slot] = final != 0;
  v->at[slot] = now_ms;
}

int capview_tick(capview_t *v, long now_ms) {
  int changed = 0;
  int keep = 0;
  for (int i = 0; i < v->n; i++) {
    long limit = v->final[i] ? CAPVIEW_FINAL_MS : CAPVIEW_PARTIAL_MS;
    if (now_ms - v->at[i] >= limit) {
      changed = 1;
      continue;
    }

    if (keep != i) {
      nl_memcpy(v->text[keep], v->text[i], sizeof(v->text[0]));
      v->final[keep] = v->final[i];
      v->at[keep] = v->at[i];
    }

    keep++;
  }

  v->n = keep;

  return changed;
}

int capview_count(const capview_t *v) { return v->n; }

int capview_render(const capview_t *v, int bottom_row, int col, int width,
                   int color, char *buf, size_t size) {
  if (v->n == 0 || width < 8 || bottom_row < v->n + 1 || col < 1 || size < 64) {
    return 0;
  }

  size_t pos = 0;
  for (int i = 0; i < v->n; i++) {
    int row = bottom_row - v->n + 1 + i;
    const char *t = v->text[i];
    int len = (int)nl_strlen(t);
    int shown = len + 2 > width ? width - 2 : len; // end is news
    const char *from = t + (len - shown);
    char line[CAPSRC_LINE + 8];
    int k = nl_snprintf(line, sizeof(line), " %.*s", shown, from);
    // pad to width so dark bar is even
    while (k < width && k < (int)sizeof(line) - 1) {
      line[k++] = ' ';
    }

    line[k] = 0;
    char head[96];
    int hk = nl_snprintf(head, sizeof(head), "\033[%d;%dH%s%s", row, col,
                         color ? "\033[38;2;255;255;255m\033[48;2;20;20;20m"
                               : "\033[7m",
                         v->final[i] ? "\033[1m" : "\033[2m");
    if (hk <= 0 || pos + (size_t)hk + (size_t)k + 8 >= size) {
      break;
    }

    nl_memcpy(buf + pos, head, (size_t)hk);
    pos += (size_t)hk;
    nl_memcpy(buf + pos, line, (size_t)k);
    pos += (size_t)k;
    nl_memcpy(buf + pos, "\033[0m", 4);
    pos += 4;
  }

  buf[pos] = 0;

  return (int)pos;
}

int capsrc_default_path(char *out, size_t size, const char *xdg_runtime,
                        const char *home) {
  int n;
  if (xdg_runtime && xdg_runtime[0] == '/') {
    n = nl_snprintf(out, size, "%s/asciicam-captions", xdg_runtime);
  } else if (home && home[0] == '/') {
    n = nl_snprintf(out, size, "%s/.asciicam-captions", home);
  } else {
    return -1;
  }

  // nl_snprintf may report what it wrote, not what it wanted: a full buffer
  // counts as cut
  return n > 0 && (size_t)n + 1 < size ? 0 : -1;
}

#if defined(__LINUX_NOLIBC__)
#define CAPSRC_SYS_MKNOD 133
#define CAPSRC_S_IFIFO 0010000

int capsrc_make_fifo(const char *path) {
  long r = __sc3(CAPSRC_SYS_MKNOD, (long)path, CAPSRC_S_IFIFO | 0600, 0);
  if (r == 0 || r == -17) { // -EEXIST
    return 0;
  }

  return -1;
}
#else
int capsrc_make_fifo(const char *path) {
  (void)path;

  return -1;
}
#endif
