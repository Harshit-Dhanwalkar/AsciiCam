#ifndef MOUSE_H
#define MOUSE_H

#include "nl_types.h"

/*
 * Mouse support for changing capture resolution by dragging  bottom-right
 * corner of ASCII frame
 *
 * NOTE: Uses xterm "button-event tracking" (?1002) with SGR encoding (?1006).
 * SGR reports look like  ESC [ < b ; x ; y M  (press / drag)  and  ... m
 * (release) with decimal coordinates, so unlike legacy X10 encoding there is no
 * column/row limit (legacy bytes overflow past column 223 and are mangled by
 * UTF-8 terminals) and a release says which button was released.
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

/*
 * Drag state of bottom-right handle. Dragging resizes CAPTURE resolution
 * proportionally: dragging corner to 1.5x frame size asks for 1.5x capture
 * size. Frame itself does not move, so new capture size is only a preview until
 * button is released (reopening camera is expensive and must happen once, not
 * per mouse-motion event)
 */
typedef struct {
  int dragging;
  int start_x, start_y; /* pointer position at grab */
  int base_fw, base_fh; /* frame size in cells at grab */
  int base_cw, base_ch; /* capture size in pixels at grab */
  int pv_w, pv_h;       /* last preview size */
} mouse_drag_t;

#define CORNER_DRAG_MARGIN 2 /* hit area: corner cell plus 2 cells left/up */

#define MOUSE_NONE 0   /* nothing to do */
#define MOUSE_GRAB 1   /* handle grabbed; preview starts at current size */
#define MOUSE_RESIZE 2 /* preview changed: *out_w / *out_h hold it */
#define MOUSE_APPLY                                                            \
  3 /* released with a changed size: reopen camera at *out_w x *out_h */
#define MOUSE_RELEASE 4 /* released, size unchanged */

int mouse_drag_event(mouse_drag_t *d, const mouse_event_t *ev, int frame_w,
                     int frame_h, int cap_w, int cap_h, int min_w, int min_h,
                     int max_w, int max_h, int *out_w, int *out_h);

/* tty side effects. mouse_disable is async-signal-safe (a single write). */
void mouse_enable(void);
void mouse_disable(void);
void draw_corner_indicator(int ascii_w, int ascii_h, int color,
                           const mouse_drag_t *d);

#endif
