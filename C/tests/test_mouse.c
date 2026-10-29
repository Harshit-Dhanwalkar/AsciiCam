/*
 * tests/test_mouse.c - SGR mouse report parser and capture-resize drag logic
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) \
 *     -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static \
 *     test_mouse.c ../src/mouse.c ../lib/nl_printf.c ../lib/nl_errno.c \
 *     ../lib/nl_alloc.c ../lib/nl_start.c -I../include -I../lib -o
 *     /tmp/test_mouse
 */
#include "../lib/nolibc.h"
#include "mouse.h"

static int fails, total;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (!(c)) {                                                                \
      fails++;                                                                 \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

// feed "ESC [ <" already consumed; s is rest, returns last rc
static int feed_str(mouse_parser_t *p, const char *s, mouse_event_t *ev) {
  int rc = MOUSE_PARSE_MORE;
  for (; *s; s++) {
    rc = mouse_parser_feed(p, *s, ev);
    if (rc != MOUSE_PARSE_MORE) {
      break;
    }
  }

  return rc;
}

static void test_parser(void) {
  mouse_parser_t p;
  mouse_event_t ev;

  mouse_parser_begin(&p);
  CHECK(mouse_parser_active(&p), "active after begin");
  CHECK(feed_str(&p, "0;12;5M", &ev) == MOUSE_PARSE_DONE, "left press");
  CHECK(ev.x == 12 && ev.y == 5 && ev.button == 0 && ev.pressed && !ev.motion &&
            !ev.wheel,
        "left press fields x=%d y=%d b=%d", ev.x, ev.y, ev.button);
  CHECK(!mouse_parser_active(&p), "idle after report");

  mouse_parser_begin(&p);
  feed_str(&p, "0;12;5m", &ev);
  CHECK(!ev.pressed && ev.button == 0, "release is 'm' with button kept");

  mouse_parser_begin(&p);
  feed_str(&p, "32;40;20M", &ev);
  CHECK(ev.motion && ev.pressed && ev.button == 0, "left drag");

  mouse_parser_begin(&p);
  feed_str(&p, "64;10;10M", &ev);
  CHECK(ev.wheel, "wheel up");
  mouse_parser_begin(&p);
  feed_str(&p, "65;10;10M", &ev);
  CHECK(ev.wheel, "wheel down");

  mouse_parser_begin(&p);
  feed_str(&p, "2;3;4M", &ev);
  CHECK(ev.button == 2, "right button");

  // modifier bits (shift 4, meta 8, ctrl 16) must not change button
  mouse_parser_begin(&p);
  feed_str(&p, "20;3;4M", &ev);
  CHECK(ev.button == 0, "ctrl+left is still button 0, got %d", ev.button);

  // coordinates beyond legacy 223 limit
  mouse_parser_begin(&p);
  CHECK(feed_str(&p, "0;300;99M", &ev) == MOUSE_PARSE_DONE && ev.x == 300 &&
            ev.y == 99,
        "x=300 supported");

  // a report split across reads
  mouse_parser_begin(&p);
  CHECK(feed_str(&p, "0;1", &ev) == MOUSE_PARSE_MORE, "partial 1");
  CHECK(mouse_parser_active(&p), "still active mid-report");
  CHECK(feed_str(&p, "2;5M", &ev) == MOUSE_PARSE_DONE && ev.x == 12 &&
            ev.y == 5,
        "resumes across reads");

  // garbage aborts and tells caller to treat byte as a key
  mouse_parser_begin(&p);
  CHECK(mouse_parser_feed(&p, 'q', &ev) == MOUSE_PARSE_FAIL, "'q' aborts");
  CHECK(!mouse_parser_active(&p), "idle after abort");

  mouse_parser_begin(&p);
  CHECK(feed_str(&p, "0;1", &ev) == MOUSE_PARSE_MORE, "partial");
  CHECK(mouse_parser_feed(&p, 'M', &ev) == MOUSE_PARSE_FAIL,
        "terminator before 3 fields");

  mouse_parser_begin(&p);
  CHECK(mouse_parser_feed(&p, ';', &ev) == MOUSE_PARSE_FAIL,
        "empty first field");

  mouse_parser_begin(&p);
  CHECK(feed_str(&p, "0;1;2;3M", &ev) == MOUSE_PARSE_FAIL, "four fields");

  mouse_parser_begin(&p);
  CHECK(feed_str(&p, "123456;1;1M", &ev) == MOUSE_PARSE_FAIL,
        "absurdly long number");
  CHECK(mouse_parser_feed(&p, '5', &ev) == MOUSE_PARSE_FAIL,
        "feeding an idle parser fails");
}

// helper: build an event
static mouse_event_t E(int x, int y, int button, int motion, int pressed) {
  mouse_event_t e = {x, y, button, motion, 0, pressed};

  return e;
}

// Frame at column 11, row 6, 40x20 cells: spans columns 11..50, rows 6..25
// Terminal 120x50 with 4 rows kept for status panel: usable rows 1..46
static const frame_rect_t FR = {11, 6, 40, 20};
static const mouse_limits_t LIM = {120, 50, 4, 10, 5, 500, 200};

#define DRAG(d, ev) mouse_drag_event(&(d), &(ev), &FR, &LIM, &r)

static int rect_is(frame_rect_t r, int x, int y, int w, int h) {
  return r.x == x && r.y == y && r.w == w && r.h == h;
}

// Press at (gx, gy), drag to (tx, ty); returns pending rectangle
static frame_rect_t drag_from(int gx, int gy, int tx, int ty, int *grab) {
  mouse_drag_t d = {0};
  frame_rect_t r = FR;
  mouse_event_t e = E(gx, gy, 0, 0, 1);
  mouse_drag_event(&d, &e, &FR, &LIM, &r);
  *grab = d.dragging;
  e = E(tx, ty, 0, 1, 1);
  r = d.pv;
  mouse_drag_event(&d, &e, &FR, &LIM, &r);

  return d.pv;
}

static void test_drag(void) {
  mouse_drag_t d = {0};
  frame_rect_t r = {0, 0, 0, 0};
  mouse_event_t e;
  int g;

  // A press off frame does nothing
  e = E(5, 5, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_NONE && !d.dragging, "press elsewhere");
  e = E(51, 10, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_NONE, "just right of frame");
  e = E(30, 26, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_NONE, "just below frame");

  // every corner is a handle, each grabs a different one
  struct {
    int x, y, grab;
  } corners[] = {
      {11, 6, GRAB_TL}, {50, 6, GRAB_TR}, {11, 25, GRAB_BL}, {50, 25, GRAB_BR}};
  for (int i = 0; i < 4; i++) {
    mouse_drag_t dc = {0};
    e = E(corners[i].x, corners[i].y, 0, 0, 1);
    CHECK(DRAG(dc, e) == MOUSE_GRAB && dc.dragging == corners[i].grab &&
              rect_is(r, 11, 6, 40, 20),
          "corner %d grabbed (got grab %d)", i, dc.dragging);
  }

  // Hit area: margin cells count, one beyond is the body (move)
  e = E(11 + CORNER_DRAG_MARGIN, 6 + CORNER_DRAG_MARGIN, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_GRAB && d.dragging == GRAB_TL, "inside TL margin");
  e = E(11 + CORNER_DRAG_MARGIN + 1, 6, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_GRAB && d.dragging == GRAB_MOVE,
        "top edge beyond margin moves");
  e = E(30, 15, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_GRAB && d.dragging == GRAB_MOVE,
        "centre grabs the frame");

  // Resize from each corner: opposite corner stays where it is
  frame_rect_t p = drag_from(50, 25, 60, 30, &g); // BR +10,+5
  CHECK(g == GRAB_BR && rect_is(p, 11, 6, 50, 25), "BR resize");
  p = drag_from(11, 6, 6, 3, &g); // TL -5,-3
  CHECK(g == GRAB_TL && rect_is(p, 6, 3, 45, 23) && p.x + p.w - 1 == 50 &&
            p.y + p.h - 1 == 25,
        "TL resize keeps bottom-right fixed");
  p = drag_from(50, 6, 60, 4, &g); // TR +10,-2
  CHECK(g == GRAB_TR && rect_is(p, 11, 4, 50, 22) && p.y + p.h - 1 == 25,
        "TR resize keeps bottom-left fixed");
  p = drag_from(11, 25, 7, 31, &g); // BL -4,+6
  CHECK(g == GRAB_BL && rect_is(p, 7, 6, 44, 26) && p.x + p.w - 1 == 50,
        "BL resize keeps top-right fixed");

  // Shrinking stops at the minimum, anchored to the opposite corner
  p = drag_from(11, 6, 200, 200, &g);
  CHECK(rect_is(p, 41, 21, 10, 5), "TL min clamp %d,%d %dx%d", p.x, p.y, p.w,
        p.h);
  p = drag_from(50, 25, 1, 1, &g);
  CHECK(rect_is(p, 11, 6, 10, 5), "BR min clamp");

  // Growing stops at the screen edge (and the status panel rows)
  p = drag_from(50, 25, 900, 900, &g);
  CHECK(rect_is(p, 11, 6, 110, 41),
        "BR stops at terminal edge, got %d,%d %dx%d", p.x, p.y, p.w, p.h);
  p = drag_from(11, 6, -50, -50, &g);
  CHECK(rect_is(p, 1, 1, 50, 25), "TL stops at top-left of terminal");

  // Move
  p = drag_from(30, 15, 50, 25, &g);
  CHECK(g == GRAB_MOVE && rect_is(p, 31, 16, 40, 20), "move by +20,+10");
  p = drag_from(30, 15, 900, 900, &g);
  CHECK(rect_is(p, 81, 27, 40, 20), "move stops at right/bottom edge (%d,%d)",
        p.x, p.y);
  p = drag_from(30, 15, -900, -900, &g);
  CHECK(rect_is(p, 1, 1, 40, 20), "move stops at top-left");

  // No terminal size known: only the top-left edge is enforced
  mouse_limits_t unk = {0, 0, 0, 10, 5, 500, 200};
  mouse_drag_t du = {0};
  e = E(30, 15, 0, 0, 1);
  mouse_drag_event(&du, &e, &FR, &unk, &r);
  e = E(930, 915, 0, 1, 1);
  mouse_drag_event(&du, &e, &FR, &unk, &r);
  CHECK(r.x == 911 && r.y == 906, "unknown terminal: no upper bound (%d,%d)",
        r.x, r.y);

  // Rest of the protocol
  e = E(50, 25, 0, 0, 1);
  DRAG(d, e);
  e = E(60, 30, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_RESIZE && rect_is(r, 11, 6, 50, 25), "preview");
  CHECK(DRAG(d, e) == MOUSE_NONE, "unchanged preview is not re-reported");

  e = E(50, 20, 3, 0, 1);
  e.wheel = 1;
  CHECK(DRAG(d, e) == MOUSE_NONE && d.dragging, "wheel ignored");
  e = E(50, 20, 2, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_NONE, "right-button drag ignored");
  e = E(1, 1, 2, 0, 0);
  DRAG(d, e);
  CHECK(d.dragging, "right release leaves left drag alone");

  CHECK(DRAG(d, e) == MOUSE_APPLY && !d.dragging && rect_is(r, 11, 6, 50, 25),
        "release applies last preview");
  e = E(60, 30, 0, 0, 0);
  CHECK(DRAG(d, e) == MOUSE_NONE, "second release does nothing");
  e = E(60, 30, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_NONE, "stray motion after release");

  // click without moving, or moving back to start: nothing to apply
  e = E(30, 15, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_GRAB, "click inside");
  e = E(30, 15, 0, 0, 0);
  CHECK(DRAG(d, e) == MOUSE_RELEASE, "click without movement => RELEASE");

  e = E(30, 15, 0, 0, 1);
  DRAG(d, e);

  e = E(40, 20, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_RESIZE, "move out");
  e = E(30, 15, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_RESIZE && rect_is(r, 11, 6, 40, 20), "move back");
  e = E(30, 15, 0, 0, 0);
  CHECK(DRAG(d, e) == MOUSE_RELEASE, "back at start => nothing applied");

  // Lost release: a fresh press elsewhere cancels, one on frame restarts
  e = E(50, 25, 0, 0, 1);
  DRAG(d, e);

  e = E(3, 3, 0, 0, 1);
  DRAG(d, e);
  CHECK(!d.dragging, "new press elsewhere cancels stale drag");

  e = E(50, 25, 0, 0, 1);
  DRAG(d, e);
  e = E(30, 15, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_GRAB && d.dragging == GRAB_MOVE,
        "new press restarts drag");

  // Max below min must not invert clamp
  mouse_limits_t odd = {120, 50, 4, 40, 20, 10, 10};
  mouse_drag_t dd = {0};
  e = E(50, 25, 0, 0, 1);
  mouse_drag_event(&dd, &e, &FR, &odd, &r);
  e = E(70, 35, 0, 1, 1);
  mouse_drag_event(&dd, &e, &FR, &odd, &r);
  CHECK(r.w == 40 && r.h == 20, "max<min falls back to min: %dx%d", r.w, r.h);

  // Frame size of minimum, in far corner: still sane
  frame_rect_t tiny = {111, 42, 10, 5};
  mouse_drag_t dt = {0};
  e = E(120, 46, 0, 0, 1);
  CHECK(mouse_drag_event(&dt, &e, &tiny, &LIM, &r) == MOUSE_GRAB &&
            dt.dragging == GRAB_BR,
        "tiny frame grab");
  e = E(130, 55, 0, 1, 1);
  mouse_drag_event(&dt, &e, &tiny, &LIM, &r);
  CHECK(rect_is(dt.pv, 111, 42, 10, 5), "tiny frame cannot grow past the edge");
}

int main(void) {
  test_parser();
  test_drag();

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
