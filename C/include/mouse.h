#ifndef MOUSE_H
#define MOUSE_H

#include "nl_types.h"

/*
 * Mouse support: place and resize ASCII frame with mouse
 *
 *   - drag any of 4 corners to resize; opposite corner stays put
 *   - drag anywhere else inside frame to move it
 *
 * Nothing is applied while button is down: caller shows an outline of pending
 * rectangle and only release (MOUSE_APPLY) changes frame, so buffers are not
 * reallocated and screen not cleared on every motion event
 *
 * NOTE: Uses xterm "button-event tracking" (?1002) with SGR encoding (?1006)
 * SGR reports look like  ESC [ < b ; x ; y M  (press / drag)  and  ... m
 * (release) with decimal coordinates, so unlike legacy X10 encoding there is no
 * column/row limit (legacy bytes overflow past column 223 and are mangled by
 * UTF-8 terminals) and a release says which button was released
 */

typedef struct {
  int x, y;    /* 1-based terminal column / row */
  int button;  /* 0 left, 1 middle, 2 right, 3 none */
  int motion;  /* 1 = pointer moved with a button held */
  int wheel;   /* 1 = scroll wheel */
  int pressed; /* 1 = press or drag ('M'), 0 = release ('m') */
} mouse_event_t;

/* Incremental SGR report parser: survives a report split across reads */
typedef struct {
  int state; /* 0 idle, else parsing field 1..3 */
  int val[3];
  int digits;
} mouse_parser_t;

#define MOUSE_PARSE_MORE 0 /* need more bytes */
#define MOUSE_PARSE_DONE 1 /* *ev is filled in */
#define MOUSE_PARSE_FAIL                                                       \
  (-1) /* not a mouse report; caller handles byte as key */

void mouse_parser_begin(mouse_parser_t *p); /* call after seeing ESC [ < */
int mouse_parser_active(const mouse_parser_t *p);
int mouse_parser_feed(mouse_parser_t *p, char c, mouse_event_t *ev);

/* Frame position and size in terminal cells; x, y are 1-based column and row of
 * its top-left cell */
typedef struct {
  int x, y, w, h;
} frame_rect_t;

/* TL/TR/BL/BR resize, MOVE moves */
#define GRAB_NONE 0
#define GRAB_TL 1
#define GRAB_TR 2
#define GRAB_BL 3
#define GRAB_BR 4
#define GRAB_MOVE 5

typedef struct {
  int dragging;         /* GRAB_* while a button is held, else GRAB_NONE */
  int start_x, start_y; /* pointer position at grab */
  frame_rect_t base;    /* frame at grab */
  frame_rect_t pv;      /* pending (previewed) frame */
} mouse_drag_t;

/* term_w/term_h are terminal size in cells (0 = unknown: only top/left edge is
 * enforced); reserved_rows at bottom belong to status panel and stay out of
 * reach */
typedef struct {
  int term_w, term_h;
  int reserved_rows;
  int min_w, min_h;
  int max_w, max_h;
} mouse_limits_t;

#define CORNER_DRAG_MARGIN 2 /* hit area: corner cell plus 2 cells inwards */

#define MOUSE_NONE 0    /* nothing to do */
#define MOUSE_GRAB 1    /* grabbed; *out is current frame */
#define MOUSE_RESIZE 2  /* pending frame changed: *out holds it */
#define MOUSE_APPLY 3   /* released with a changed frame: adopt *out */
#define MOUSE_RELEASE 4 /* released, frame unchanged */

/* Feed one mouse event. cur is frame as drawn now */
int mouse_drag_event(mouse_drag_t *d, const mouse_event_t *ev,
                     const frame_rect_t *cur, const mouse_limits_t *lim,
                     frame_rect_t *out);

/* tty side effects. mouse_disable is async-signal-safe (a single write) */
void mouse_enable(void);
void mouse_disable(void);
/* Four corner handles of r; one being dragged is highlighted */
void draw_corner_indicators(const frame_rect_t *r, int color,
                            const mouse_drag_t *d);

/* Outline of r, drawn over whatever is there, or erased with spaces
 * Frame is redrawn every cycle, so erasing only matters outside it */
void draw_drag_outline(const frame_rect_t *r, int color, int erase);

#endif
