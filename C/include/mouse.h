#ifndef MOUSE_H
#define MOUSE_H

#include "capture.h"

// Mouse tracking state
extern int mouse_tracking_enabled;
extern int dragging_corner;
extern int drag_start_x;
extern int drag_start_y;
extern int drag_start_w;
extern int drag_start_h;

#define CORNER_DRAG_MARGIN 2

// Enable/disable X10 button-event tracking
void enable_mouse_tracking(void);
void disable_mouse_tracking(void);

void draw_corner_indicator(int ascii_w, int ascii_h, int color);

// Return codes from handle_mouse_event:
//   0  nothing changed
//   1  drag started (corner press recorded)
//   2  drag released with size change -> caller must call reinit_capture ONCE
//   3  drag in progress -> cap_w/cap_h updated for live preview display,
//      but the driver has NOT been touched; caller must NOT call reinit here
int handle_mouse_event(int button, int x, int y, int *cap_w, int *cap_h,
                       webcam_t *cam, int ascii_w, int ascii_h);

#endif
