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

// Resize handle state machine
static int in_corner(const mouse_event_t *ev, int fw, int fh) {
  return ev->x >= fw - CORNER_DRAG_MARGIN && ev->x <= fw &&
         ev->y >= fh - CORNER_DRAG_MARGIN && ev->y <= fh;
}

static int clamp_int(int v, int lo, int hi) {
  if (hi < lo) {
    hi = lo;
  }

  return v < lo ? lo : (v > hi ? hi : v);
}

// new = base * (frame + delta) / frame, even, clamped
static int scale_dim(int base_cap, int base_frame, int delta, int lo, int hi) {
  if (base_frame < 1) {
    base_frame = 1;
  }

  int cells = base_frame + delta;
  if (cells < 0) {
    cells = 0;
  }

  int v = (int)((long)base_cap * (long)cells / (long)base_frame);
  v &= ~1; // capture sizes are rounded to even by camera layer anyway

  return clamp_int(v, lo, hi);
}

int mouse_drag_event(mouse_drag_t *d, const mouse_event_t *ev, int frame_w,
                     int frame_h, int cap_w, int cap_h, int min_w, int min_h,
                     int max_w, int max_h, int *out_w, int *out_h) {
  if (ev->wheel) {
    return MOUSE_NONE;
  }

  int left = (ev->button == 0);

  // Press on handle starts a drag (a second press means release was
  // lost, e.g. focus change: just start over)
  if (ev->pressed && !ev->motion && left) {
    if (in_corner(ev, frame_w, frame_h)) {
      d->dragging = 1;
      d->start_x = ev->x;
      d->start_y = ev->y;
      d->base_fw = frame_w;
      d->base_fh = frame_h;
      d->base_cw = cap_w;
      d->base_ch = cap_h;
      d->pv_w = cap_w;
      d->pv_h = cap_h;
      *out_w = cap_w;
      *out_h = cap_h;

      return MOUSE_GRAB;
    }

    d->dragging = 0;

    return MOUSE_NONE;
  }

  if (!d->dragging) {
    return MOUSE_NONE;
  }

  if (ev->pressed && ev->motion && left) {
    int w = scale_dim(d->base_cw, d->base_fw, ev->x - d->start_x, min_w, max_w);
    int h = scale_dim(d->base_ch, d->base_fh, ev->y - d->start_y, min_h, max_h);
    if (w == d->pv_w && h == d->pv_h) {
      return MOUSE_NONE;
    }

    d->pv_w = w;
    d->pv_h = h;
    *out_w = w;
    *out_h = h;

    return MOUSE_RESIZE;
  }

  if (!ev->pressed && left) {
    d->dragging = 0;
    if (d->pv_w != d->base_cw || d->pv_h != d->base_ch) {
      *out_w = d->pv_w;
      *out_h = d->pv_h;

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

void draw_corner_indicator(int ascii_w, int ascii_h, int color,
                           const mouse_drag_t *d) {
  if (!MOUSE_TTY || ascii_w < 4 || ascii_h < 2) {
    return;
  }

  int active = d && d->dragging;
  char buf[96];
  int n;

  if (color) {
    // U+25E2 lower-right triangle = \342\227\242, orange; red while dragging
    n = nl_snprintf(buf, sizeof(buf),
                    "\033[%d;%dH\033[38;2;%sm\342\227\242\033[0m", ascii_h,
                    ascii_w, active ? "255;60;60" : "255;140;0");
  } else {
    n = nl_snprintf(buf, sizeof(buf), "\033[%d;%dH\033[%sm+\033[0m", ascii_h,
                    ascii_w, active ? "7;5" : "7");
  }

  if (n > 0 && n < (int)sizeof(buf)) {
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }
}
