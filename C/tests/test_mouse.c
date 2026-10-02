/*
 * tests/test_mouse.c - SGR mouse report parser and capture-resize drag logic
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) \
 *     -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static \
 *     test_mouse.c ../src/mouse.c ../lib/nl_printf.c ../lib/nl_errno.c \
 *     ../lib/nl_alloc.c ../lib/nl_start.c -I../include -I../lib -o test_mouse
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

// frame 80x40 cells showing a 640x480 capture; limits 160x120 .. 1280x720
#define DRAG(d, ev)                                                            \
  mouse_drag_event(&(d), &(ev), 80, 40, 640, 480, 160, 120, 1280, 720, &w, &h)

static void test_drag(void) {
  mouse_drag_t d = {0};
  int w = 0, h = 0;
  mouse_event_t e;

  // a press away from corner does nothing
  e = E(5, 5, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_NONE && !d.dragging, "press elsewhere");

  // grab at corner: preview starts at current capture size
  e = E(79, 39, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_GRAB && d.dragging && w == 640 && h == 480,
        "grab corner (w=%d h=%d)", w, h);

  // dragging +40 cells right (80 -> 120 cells = 1.5x) => 960 wide;
  // +20 cells down (40 -> 60 = 1.5x) => 720 high
  e = E(79 + 40, 39 + 20, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_RESIZE && w == 960 && h == 720,
        "1.5x drag => %dx%d", w, h);

  // same position again: no new preview
  CHECK(DRAG(d, e) == MOUSE_NONE, "unchanged preview is not re-reported");

  // a small move: -10 cells => 70/80 of 640 = 560
  e = E(79 - 10, 39, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_RESIZE && w == 560 && h == 480, "shrink => %dx%d",
        w, h);

  // clamps to min and max
  e = E(1, 1, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_RESIZE && w == 160 && h == 120, "min clamp %dx%d",
        w, h);
  e = E(900, 900, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_RESIZE && w == 1280 && h == 720, "max clamp %dx%d",
        w, h);

  // wheel and right-button events are ignored mid-drag
  e = E(50, 20, 3, 0, 1);
  e.wheel = 1;
  CHECK(DRAG(d, e) == MOUSE_NONE && d.dragging, "wheel ignored");

  e = E(50, 20, 2, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_NONE, "right-button drag ignored");

  // release applies last preview exactly once
  e = E(900, 900, 0, 0, 0);
  CHECK(DRAG(d, e) == MOUSE_APPLY && !d.dragging && w == 1280 && h == 720,
        "release => APPLY %dx%d", w, h);

  e = E(900, 900, 0, 0, 0);
  CHECK(DRAG(d, e) == MOUSE_NONE, "second release does nothing");

  e = E(60, 30, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_NONE, "stray motion after release");

  // click on handle without moving: no reinit
  e = E(80, 40, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_GRAB, "click handle");

  e = E(80, 40, 0, 0, 0);
  CHECK(DRAG(d, e) == MOUSE_RELEASE, "click without movement => RELEASE");

  // drag out and back to start: net change zero => RELEASE, no reinit
  e = E(80, 40, 0, 0, 1);
  DRAG(d, e);

  e = E(100, 50, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_RESIZE, "move out");

  e = E(80, 40, 0, 1, 1);
  CHECK(DRAG(d, e) == MOUSE_RESIZE && w == 640 && h == 480, "move back");

  e = E(80, 40, 0, 0, 0);
  CHECK(DRAG(d, e) == MOUSE_RELEASE, "back at start => no reinit");

  // a right-button release never ends a left drag
  e = E(80, 40, 0, 0, 1);
  DRAG(d, e);

  e = E(1, 1, 2, 0, 0);
  DRAG(d, e);
  CHECK(d.dragging, "right release leaves left drag alone");

  // lost release: a fresh press elsewhere cancels stale drag
  e = E(3, 3, 0, 0, 1);
  DRAG(d, e);
  CHECK(!d.dragging, "new press elsewhere cancels stale drag");

  // hit area edges: margin cells count, one beyond does not
  e = E(80 - CORNER_DRAG_MARGIN, 40 - CORNER_DRAG_MARGIN, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_GRAB, "inside hit margin");

  d.dragging = 0;
  e = E(80 - CORNER_DRAG_MARGIN - 1, 40, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_NONE, "just outside hit margin");

  e = E(81, 40, 0, 0, 1);
  CHECK(DRAG(d, e) == MOUSE_NONE, "right of frame");

  // results are always even, whatever ratio (odd base size too)
  mouse_drag_t d2 = {0};
  e = E(80, 40, 0, 0, 1);
  mouse_drag_event(&d2, &e, 80, 40, 641, 479, 160, 120, 1280, 720, &w, &h);

  e = E(87, 43, 0, 1, 1);
  mouse_drag_event(&d2, &e, 80, 40, 641, 479, 160, 120, 1280, 720, &w, &h);
  CHECK(w % 2 == 0 && h % 2 == 0, "even dims, got %dx%d", w, h);

  // degenerate frame (1 cell): must not divide by zero
  mouse_drag_t d3 = {0};
  e = E(1, 1, 0, 0, 1);
  CHECK(mouse_drag_event(&d3, &e, 1, 1, 640, 480, 160, 120, 1280, 720, &w,
                         &h) == MOUSE_GRAB,
        "1x1 frame grab");

  e = E(5, 5, 0, 1, 1);
  mouse_drag_event(&d3, &e, 1, 1, 640, 480, 160, 120, 1280, 720, &w, &h);
  CHECK(w >= 160 && w <= 1280, "1x1 frame stays in limits (%d)", w);

  // max below min must not invert clamp
  mouse_drag_t d4 = {0};
  e = E(80, 40, 0, 0, 1);
  mouse_drag_event(&d4, &e, 80, 40, 640, 480, 400, 300, 200, 100, &w, &h);
  e = E(200, 100, 0, 1, 1);
  mouse_drag_event(&d4, &e, 80, 40, 640, 480, 400, 300, 200, 100, &w, &h);
  CHECK(w == 400 && h == 300, "max<min falls back to min: %dx%d", w, h);
}

int main(void) {
  test_parser();
  test_drag();

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
