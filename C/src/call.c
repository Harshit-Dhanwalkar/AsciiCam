/*
 * src/call.c - client side of a video call, see include/call.h
 */

#include "platform.h"

#include "nolibc.h"

#include "call.h"
#include "netview.h"

static void log_line(call_t *c, const char *who, const char *text) {
  char *dst = c->log[c->log_count % CALL_LOG_LINES];
  nl_snprintf(dst, CALL_LOG_W, "%s: %s", who, text);

  c->log_count++;
  c->dirty = 1;
}

static void log_note(call_t *c, const char *name, const char *what) {
  char *dst = c->log[c->log_count % CALL_LOG_LINES];
  nl_snprintf(dst, CALL_LOG_W, "* %s %s", name, what);
  c->log_count++;
  c->dirty = 1;
}

const char *call_log_line(const call_t *c, int back) {
  if (back < 0 || back >= c->log_count || back >= CALL_LOG_LINES) {
    return NULL;
  }

  return c->log[(c->log_count - 1 - back) % CALL_LOG_LINES];
}

int call_members(const call_t *c) {
  int n = c->id >= 0 ? 1 : 0;
  for (int i = 0; i < ROOM_MAX; i++) {
    n += c->peer[i].present;
  }

  return n;
}

int call_open_fd(call_t *c, int fd, const char *name) {
  nl_memset(c, 0, sizeof(*c));
  c->fd = fd;
  c->id = -1;
  if (mb_init(&c->in) != 0 || oq_init(&c->out) != 0) {
    mb_free(&c->in);
    oq_free(&c->out);
    c->fd = -1;

    return -1;
  }

  size_t len = call_clean_text(c->name, (const uint8_t *)name, nl_strlen(name),
                               CALL_NAME_MAX);
  if (len == 0) {
    nl_strncpy_safe(c->name, "guest", sizeof(c->name));
    len = 5;
  }

  uint8_t ver = CALL_VERSION;
  if (oq_put(&c->out, CALL_HELLO, &ver, 1, c->name, len) != 0) {
    call_close(c);

    return -1;
  }

  c->need_key = 1;
  c->dirty = 1;

  return 0;
}

int call_open(call_t *c, const char *hostport, const char *name) {
  int fd = nv_connect(hostport);
  if (fd < 0) {
    return -1;
  }

  if (nv_nonblock(fd) != 0 || call_open_fd(c, fd, name) != 0) {
    close(fd);

    return -1;
  }

  return 0;
}

static void on_message(call_t *c, int type, const uint8_t *p, size_t n) {
  switch (type) {
  case CALL_WELCOME:
    if (n == 1 && p[0] < ROOM_MAX) {
      c->id = p[0];
      c->dirty = 1;
    }

    break;
  case CALL_JOIN:
    if (n >= 1 && p[0] < ROOM_MAX && (int)p[0] != c->id) {
      call_peer_t *pe = &c->peer[p[0]];
      char nm[CALL_NAME_MAX + 1];
      call_clean_text(nm, p + 1, n - 1, CALL_NAME_MAX);
      if (nm[0] == '\0') {
        nl_strncpy_safe(nm, "guest", sizeof(nm));
      }

      cc_grid_free(&pe->grid);
      pe->have_picture = 0;
      pe->present = 1;
      nl_strncpy_safe(pe->name, nm, sizeof(pe->name));
      c->need_key = 1; // newcomer has none of ours
      log_note(c, pe->name, "joined");
    }

    break;
  case CALL_LEAVE:
    if (n == 1 && p[0] < ROOM_MAX && c->peer[p[0]].present) {
      call_peer_t *pe = &c->peer[p[0]];
      log_note(c, pe->name, "left");
      pe->present = 0;
      pe->have_picture = 0;
      cc_grid_free(&pe->grid);
      c->dirty = 1;
    }

    break;
  case CALL_PFRAME:
    if (n > 1 && p[0] < ROOM_MAX && c->peer[p[0]].present) {
      call_peer_t *pe = &c->peer[p[0]];
      int rc = cc_decode(&pe->grid, p + 1, n - 1);
      if (rc == 0) {
        pe->have_picture = 1;
        c->frames_received++;
        c->dirty = 1;
      }
    }

    break;
  case CALL_PCHAT:
    if (n > 1 && p[0] < ROOM_MAX) {
      char text[CALL_CHAT_MAX + 1];
      call_clean_text(text, p + 1, n - 1, CALL_CHAT_MAX);
      const char *who = (int)p[0] == c->id      ? "you"
                        : c->peer[p[0]].present ? c->peer[p[0]].name
                                                : "?";
      if (text[0]) {
        log_line(c, who, text);
      }
    }

    break;
  case CALL_NEED_KEY:
    c->need_key = 1;

    break;
  case CALL_FULL:
    c->full = 1;
    c->closed = 1;

    break;
  default:
    break; // unknown: ignore
  }
}

int call_pump(call_t *c) {
  if (c->fd < 0 || c->closed) {
    return -1;
  }

  for (;;) {
    long got = mb_fill(&c->in, c->fd);
    if (got > 0) {
      for (;;) {
        int type;
        const uint8_t *p;
        size_t n;
        int rc = mb_next(&c->in, &type, &p, &n);
        if (rc < 0) {
          c->closed = 1;

          return -1;
        }

        if (rc == 0) {
          break;
        }

        on_message(c, type, p, n);
        mb_pop(&c->in);
      }

      continue;
    }

    if (got == -2) {
      break;
    }

    c->closed = 1;

    return -1; // closed, error, oversized message
  }

  if (oq_flush(&c->out, c->fd) != 0) {
    c->closed = 1;

    return -1;
  }

  return c->closed ? -1 : 0;
}

int call_send_picture(call_t *c, const char *ansi, size_t n) {
  if (c->fd < 0 || c->closed || c->id < 0) {
    return 1; // not welcomed yet: nothing to send to
  }

  if (cc_grid_from_ansi(&c->local, ansi, n) != 0) {
    return -1;
  }

  c->dirty = 1;
  if (oq_pending(&c->out) > CALL_OUTQ / 4) {
    c->frames_skipped++;

    return 1;
  }

  int key = c->need_key || !c->have_sent || c->since_key >= CALL_KEY_EVERY;
  static uint8_t buf[CALL_MAX_PAYLOAD];
  size_t len =
      cc_encode(&c->local, key ? NULL : &c->sent, key, buf, sizeof(buf));
  if (len == 0) {
    c->need_key = 1;

    return -1;
  }

  if (oq_put(&c->out, CALL_FRAME, NULL, 0, buf, len) != 0) {
    c->frames_skipped++;
    c->need_key = 1;

    return 1;
  }

  // Receivers now hold this picture
  if (cc_grid_size(&c->sent, c->local.cols, c->local.rows) != 0) {
    c->need_key = 1;

    return 0;
  }

  nl_memcpy(c->sent.cell, c->local.cell,
            (size_t)c->local.cols * (size_t)c->local.rows * sizeof(netcell_t));
  c->have_sent = 1;
  if (key) {
    c->need_key = 0;
    c->since_key = 0;
  } else {
    c->since_key++;
  }

  c->frames_sent++;

  return 0;
}

int call_say(call_t *c, const char *text) {
  char clean[CALL_CHAT_MAX + 1];
  size_t len = call_clean_text(clean, (const uint8_t *)text, nl_strlen(text),
                               CALL_CHAT_MAX);
  if (len == 0 || c->fd < 0 || c->closed) {
    return -1;
  }

  return oq_put(&c->out, CALL_CHAT, NULL, 0, clean, len);
}

void call_close(call_t *c) {
  if (c->fd >= 0) {
    close(c->fd);
  }

  c->fd = -1;
  c->closed = 1;
  mb_free(&c->in);
  oq_free(&c->out);
  cc_grid_free(&c->local);
  cc_grid_free(&c->sent);

  for (int i = 0; i <= ROOM_MAX; i++) {
    cc_grid_free(&c->shown[i]);
  }

  for (int i = 0; i < ROOM_MAX; i++) {
    cc_grid_free(&c->peer[i].grid);
    c->peer[i].present = 0;
  }
}

// Layout

#define TILE_GAP 2

typedef struct {
  const char *name;
  const cc_grid_t *grid; // NULL: no picture yet
  int slot;              // index in call_t.shown
  int w;                 // cells of tile width without its label
  int h;                 // cells of tile height without its label
} tile_t;

static int chat_height(int term_h) {
  int h = term_h / 3;

  return h > 8 ? 8 : h < 1 ? 1 : h;
}

static int collect(const call_t *c, tile_t *t) {
  int n = 0;
  t[n++] = (tile_t){
      .name = "you", .grid = c->local.cell ? &c->local : NULL, .slot = 0};
  for (int i = 0; i < ROOM_MAX; i++) {
    if (c->peer[i].present) {
      t[n++] =
          (tile_t){.name = c->peer[i].name,
                   .grid = c->peer[i].have_picture ? &c->peer[i].grid : NULL,
                   .slot = i + 1};
    }
  }

  for (int i = 0; i < n; i++) {
    size_t nl = nl_strlen(t[i].name);
    t[i].w = t[i].grid ? t[i].grid->cols : 24;
    t[i].w = t[i].w < (int)nl ? (int)nl : t[i].w;
    t[i].h = t[i].grid ? t[i].grid->rows : 1;
  }

  return n;
}

size_t call_max_compose(const call_t *c, int term_w, int term_h) {
  size_t total =
      4096 + (size_t)(chat_height(term_h) + 3) * ((size_t)term_w * 4u + 64u);
  tile_t t[ROOM_MAX + 1];
  int n = collect(c, t);
  for (int i = 0; i < n; i++) {
    total += cc_max_draw(t[i].w, t[i].h) + 64;
  }

  return total;
}

typedef struct {
  char *p;
  size_t cap;
  size_t n;
  int over;
} cw_t;

static void cputs(cw_t *w, const char *s) {
  while (*s) {
    if (w->n < w->cap) {
      w->p[w->n++] = *s;
    } else {
      w->over = 1;
    }

    s++;
  }
}

static void cmove(cw_t *w, int row, int col) {
  char tmp[32];
  nl_snprintf(tmp, sizeof(tmp), "\033[%d;%dH", row, col);
  cputs(w, tmp);
}

// Text clipped to width cells (ASCII only here: every byte is one cell)
static void cclip(cw_t *w, const char *s, int width) {
  for (int i = 0; s[i] && i < width; i++) {
    char one[2] = {s[i], 0};
    cputs(w, one);
  }
}

// Copy of a tile shows on screen
static void remember(cc_grid_t *shown, const cc_grid_t *g) {
  if (shown->cols != g->cols || shown->rows != g->rows || !shown->cell) {
    cc_grid_free(shown);
    if (cc_grid_size(shown, g->cols, g->rows) != 0) {
      return;
    }
  }

  nl_memcpy(shown->cell, g->cell,
            (size_t)g->cols * (size_t)g->rows * sizeof(netcell_t));
}

size_t call_compose(call_t *c, const char *typing, int term_w, int term_h,
                    char *out, size_t cap) {
  if (term_w < 20 || term_h < 6) {
    return 0;
  }

  tile_t t[ROOM_MAX + 1];
  int n = collect(c, t);
  int chat_h = chat_height(term_h);
  int video_bottom = term_h - chat_h - 1;

  // Signature of layout: when it changes screen is cleared first
  int sig = term_w * 31 + term_h + n * 7;
  for (int i = 0; i < n; i++) {
    sig = sig * 131 + t[i].w * 17 + t[i].h + (t[i].grid ? 1 : 0) +
          t[i].slot * 977;
  }

  cw_t w = {out, cap, 0, 0};
  cputs(&w, "\033[?2026h\033[0m");
  if (sig != c->sig) {
    cputs(&w, "\033[2J");
    c->sig = sig;
    for (int i = 0; i <= ROOM_MAX; i++) {
      cc_grid_free(&c->shown[i]); // the screen is blank now
    }
  }

  // Title
  char title[120];
  nl_snprintf(title, sizeof(title),
              " asciicam call  |  %d here  |  Enter: send   Ctrl-C: leave",
              call_members(c));
  cmove(&w, 1, 1);
  cputs(&w, "\033[7m");
  cclip(&w, title, term_w);
  for (int i = (int)nl_strlen(title); i < term_w; i++) {
    cputs(&w, " ");
  }

  cputs(&w, "\033[0m");

  // Pictures, left to right, wrapping
  int x = 1;
  int y = 2;
  int row_h = 0;
  for (int i = 0; i < n; i++) {
    int tw = t[i].w;
    int th = t[i].h + 1; // label row
    if (x > 1 && x + tw - 1 > term_w) {
      x = 1;
      y += row_h + 1;
      row_h = 0;
    }

    if (y + 1 <= video_bottom) {
      cmove(&w, y, x);
      cputs(&w, "\033[1m");
      cclip(&w, t[i].name, tw);
      cputs(&w, "\033[0m");
      if (t[i].grid) {
        char *buf = nl_malloc(cc_max_draw(t[i].grid->cols, t[i].grid->rows));
        if (buf) {
          size_t dn =
              cc_draw_diff(t[i].grid, &c->shown[t[i].slot], x, y + 1,
                           term_w - x + 1, video_bottom - y, buf,
                           cc_max_draw(t[i].grid->cols, t[i].grid->rows));
          for (size_t k = 0; k < dn; k++) {
            if (w.n < w.cap) {
              w.p[w.n++] = buf[k];
            } else {
              w.over = 1;
            }
          }

          nl_free(buf);
          remember(&c->shown[t[i].slot], t[i].grid);
        }
      } else {
        cmove(&w, y + 1, x);
        cputs(&w, "(waiting for picture)");
      }
    }

    row_h = th > row_h ? th : row_h;
    x += tw + TILE_GAP;
  }

  // Chat: last lines, newest at bottom of area
  int first_row = term_h - chat_h;
  for (int r = 0; r < chat_h; r++) {
    int back = chat_h - 1 - r;
    cmove(&w, first_row + r, 1);
    cputs(&w, "\033[2K");
    const char *line = call_log_line(c, back);
    if (line) {
      cclip(&w, line, term_w);
    }
  }

  // Input line
  cmove(&w, term_h, 1);
  cputs(&w, "\033[2K\033[7m");
  if (typing) {
    cputs(&w, " > ");
    int room = term_w - 5;
    int tl = (int)nl_strlen(typing);
    cclip(&w, typing + (tl > room ? tl - room : 0), room);
    cputs(&w, "_");
  } else {
    cputs(&w, " Enter: write a message ");
  }

  cputs(&w, "\033[0m\033[?2026l");
  if (w.over) {
    // written is not screen holds: start again from blank
    c->sig = 0;

    return 0;
  }

  c->dirty = 0;

  return w.n;
}
