#include "nolibc.h"

#include "mouse.h"

#ifdef _WIN32
/* Windows console input path does not deliver VT mouse reports through termios
 * shim, so feature is compiled out */
#define MOUSE_TTY 0
#else
#define MOUSE_TTY 1
#endif

// SGR report parser
void mouse_parser_begin(mouse_parser_t *p) {
  p->state = 1;
  p->val[0] = p->val[1] = p->val[2] = 0;
  p->digits = 0;
}

int mouse_parser_active(const mouse_parser_t *p) { return p->state != 0; }

int mouse_parser_feed(mouse_parser_t *p, char c, mouse_event_t *ev) {
  if (p->state == 0) {
    return MOUSE_PARSE_FAIL;
  }

  if (c >= '0' && c <= '9') {
    if (++p->digits > 5) { // far beyond any real terminal size
      p->state = 0;

      return MOUSE_PARSE_FAIL;
    }

    p->val[p->state - 1] = p->val[p->state - 1] * 10 + (c - '0');

    return MOUSE_PARSE_MORE;
  }

  if (c == ';' && p->state < 3 && p->digits > 0) {
    p->state++;
    p->digits = 0;

    return MOUSE_PARSE_MORE;
  }

  if ((c == 'M' || c == 'm') && p->state == 3 && p->digits > 0) {
    int b = p->val[0];
    p->state = 0;

    ev->x = p->val[1];
    ev->y = p->val[2];
    ev->pressed = (c == 'M');
    ev->wheel = (b & 64) != 0;
    ev->motion = !ev->wheel && (b & 32) != 0;
    ev->button = ev->wheel ? 3 : (b & 3); // modifier bits 4/8/16 are ignored

    return MOUSE_PARSE_DONE;
  }

  p->state = 0; // anything else: this was not a mouse report

  return MOUSE_PARSE_FAIL;
}

// Drag state machine
static int clamp_int(int v, int lo, int hi) {
  if (hi < lo) {
    hi = lo;
  }

  return v < lo ? lo : (v > hi ? hi : v);
}

static int min_int(int a, int b) { return a < b ? a : b; }

static int max_int(int a, int b) { return a > b ? a : b; }

// Part of frame is at pointer
static int hit_test(const mouse_event_t *ev, const frame_rect_t *r) {
  int x0 = r->x, x1 = r->x + r->w - 1;
  int y0 = r->y, y1 = r->y + r->h - 1;
  if (ev->x < x0 || ev->x > x1 || ev->y < y0 || ev->y > y1) {
    return GRAB_NONE;
  }

  int left = ev->x - x0 <= CORNER_DRAG_MARGIN;
  int right = x1 - ev->x <= CORNER_DRAG_MARGIN;
  int top = ev->y - y0 <= CORNER_DRAG_MARGIN;
  int bottom = y1 - ev->y <= CORNER_DRAG_MARGIN;

  if (bottom && right) {
    return GRAB_BR;
  }

  if (bottom && left) {
    return GRAB_BL;
  }

  if (top && right) {
    return GRAB_TR;
  }

  if (top && left) {
    return GRAB_TL;
  }

  return GRAB_MOVE;
}

// Right/bottom limits of area frame may occupy (huge when unknown)
static int area_right(const mouse_limits_t *l) {
  return l->term_w > 0 ? l->term_w : 1 << 20;
}

static int area_bottom(const mouse_limits_t *l) {
  return l->term_h > 0 ? l->term_h - l->reserved_rows : 1 << 20;
}

// Pending frame for pointer at (px, py)
static frame_rect_t drag_rect(const mouse_drag_t *d, int px, int py,
                              const mouse_limits_t *l) {
  frame_rect_t r = d->base;
  int dx = px - d->start_x;
  int dy = py - d->start_y;

  if (d->dragging == GRAB_MOVE) {
    r.x = clamp_int(d->base.x + dx, 1, area_right(l) - r.w + 1);
    r.y = clamp_int(d->base.y + dy, 1, area_bottom(l) - r.h + 1);

    return r;
  }

  int right_edge = d->base.x + d->base.w - 1;
  int bottom_edge = d->base.y + d->base.h - 1;

  // Corner opposite grabbed one never moves
  if (d->dragging == GRAB_TR || d->dragging == GRAB_BR) {
    r.w = clamp_int(d->base.w + dx, l->min_w,
                    min_int(l->max_w, area_right(l) - d->base.x + 1));
  } else {
    r.w = clamp_int(d->base.w - dx, l->min_w, min_int(l->max_w, right_edge));
    r.x = max_int(1, right_edge - r.w + 1);
  }

  if (d->dragging == GRAB_BL || d->dragging == GRAB_BR) {
    r.h = clamp_int(d->base.h + dy, l->min_h,
                    min_int(l->max_h, area_bottom(l) - d->base.y + 1));
  } else {
    r.h = clamp_int(d->base.h - dy, l->min_h, min_int(l->max_h, bottom_edge));
    r.y = max_int(1, bottom_edge - r.h + 1);
  }

  return r;
}

static int rect_eq(const frame_rect_t *a, const frame_rect_t *b) {
  return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

int mouse_drag_event(mouse_drag_t *d, const mouse_event_t *ev,
                     const frame_rect_t *cur, const mouse_limits_t *lim,
                     frame_rect_t *out) {
  if (ev->wheel) {
    return MOUSE_NONE;
  }

  int left = (ev->button == 0);

  // A press on frame starts a drag (a second press means release was lost, e.g.
  // focus change: start over)
  if (ev->pressed && !ev->motion && left) {
    int part = hit_test(ev, cur);
    d->dragging = part;
    if (part == GRAB_NONE) {
      return MOUSE_NONE;
    }

    d->start_x = ev->x;
    d->start_y = ev->y;
    d->base = *cur;
    d->pv = *cur;
    *out = *cur;

    return MOUSE_GRAB;
  }

  if (!d->dragging) {
    return MOUSE_NONE;
  }

  if (ev->pressed && ev->motion && left) {
    frame_rect_t r = drag_rect(d, ev->x, ev->y, lim);
    if (rect_eq(&r, &d->pv)) {
      return MOUSE_NONE;
    }

    d->pv = r;
    *out = r;

    return MOUSE_RESIZE;
  }

  if (!ev->pressed && left) {
    d->dragging = GRAB_NONE;
    if (!rect_eq(&d->pv, &d->base)) {
      *out = d->pv;

      return MOUSE_APPLY;
    }

    return MOUSE_RELEASE;
  }

  return MOUSE_NONE;
}

// tty side effects
void mouse_enable(void) {
  if (!MOUSE_TTY) {
    return;
  }

  // ?1002 = report press/release/drag, ?1006 = SGR coordinates
  static const char ON[] = "\033[?1002h\033[?1006h";
  (void)write(STDOUT_FILENO, ON, sizeof(ON) - 1);
}

void mouse_disable(void) {
  if (!MOUSE_TTY) {
    return;
  }
  static const char OFF[] = "\033[?1006l\033[?1002l";
  (void)write(STDOUT_FILENO, OFF, sizeof(OFF) - 1);
}

void draw_corner_indicators(const frame_rect_t *r, int color,
                            const mouse_drag_t *d) {
  if (!MOUSE_TTY || r->w < 4 || r->h < 2) {
    return;
  }

  // U+25E4 / U+25E5 / U+25E3 / U+25E2: triangle with its right angle in corner
  // it marks Orange; red while being dragged
  static const char *const GLYPH[4] = {"\342\227\244", "\342\227\245",
                                       "\342\227\243", "\342\227\242"};
  const int grab[4] = {GRAB_TL, GRAB_TR, GRAB_BL, GRAB_BR};
  int col[4] = {r->x, r->x + r->w - 1, r->x, r->x + r->w - 1};
  int row[4] = {r->y, r->y, r->y + r->h - 1, r->y + r->h - 1};

  char buf[512];
  size_t len = 0;
  for (int i = 0; i < 4; i++) {
    int active = d && d->dragging == grab[i];
    int n;
    if (color) {
      n = nl_snprintf(buf + len, sizeof(buf) - len,
                      "\033[%d;%dH\033[38;2;%sm%s\033[0m", row[i], col[i],
                      active ? "255;60;60" : "255;140;0", GLYPH[i]);
    } else {
      n = nl_snprintf(buf + len, sizeof(buf) - len,
                      "\033[%d;%dH\033[%sm+\033[0m", row[i], col[i],
                      active ? "7;5" : "7");
    }

    if (n <= 0 || len + (size_t)n >= sizeof(buf)) {
      break;
    }

    len += (size_t)n;
  }

  if (len > 0) {
    (void)write(STDOUT_FILENO, buf, len);
  }
}

// Buffered writer so a big outline is a few writes, not hundreds
typedef struct {
  char buf[2048];
  size_t len;
} outbuf_t;

static void ob_flush(outbuf_t *o) {
  if (o->len > 0) {
    (void)write(STDOUT_FILENO, o->buf, o->len);
    o->len = 0;
  }
}

static void ob_put(outbuf_t *o, const char *s, size_t n) {
  if (o->len + n > sizeof(o->buf)) {
    ob_flush(o);
  }

  if (n > sizeof(o->buf)) {
    return;
  }

  nl_memcpy(o->buf + o->len, s, n);
  o->len += n;
}

static void ob_str(outbuf_t *o, const char *s) { ob_put(o, s, nl_strlen(s)); }

static void ob_move(outbuf_t *o, int row, int col) {
  char t[24];
  int n = nl_snprintf(t, sizeof(t), "\033[%d;%dH", row, col);
  if (n > 0) {
    ob_put(o, t, (size_t)n);
  }
}

void draw_drag_outline(const frame_rect_t *r, int color, int erase) {
  if (!MOUSE_TTY || r->w < 2 || r->h < 2) {
    return;
  }

  const char *h = color ? "\342\224\200" : "-";  // ─
  const char *v = color ? "\342\224\202" : "|";  // │
  const char *tl = color ? "\342\224\214" : "+"; // ┌
  const char *tr = color ? "\342\224\220" : "+"; // ┐
  const char *bl = color ? "\342\224\224" : "+"; // └
  const char *br = color ? "\342\224\230" : "+"; // ┘
  if (erase) {
    h = v = tl = tr = bl = br = " ";
  }

  outbuf_t o;
  o.len = 0;
  if (!erase && color) {
    ob_str(&o, "\033[1;38;2;255;60;60m");
  } else if (!erase) {
    ob_str(&o, "\033[7m");
  }

  for (int k = 0; k < 2; k++) {
    int row = k == 0 ? r->y : r->y + r->h - 1;
    ob_move(&o, row, r->x);
    ob_str(&o, k == 0 ? tl : bl);
    for (int i = 1; i < r->w - 1; i++) {
      ob_str(&o, h);
    }

    ob_str(&o, k == 0 ? tr : br);
  }

  for (int y = r->y + 1; y < r->y + r->h - 1; y++) {
    ob_move(&o, y, r->x);
    ob_str(&o, v);
    ob_move(&o, y, r->x + r->w - 1);
    ob_str(&o, v);
  }

  ob_str(&o, "\033[0m");
  ob_flush(&o);
}
