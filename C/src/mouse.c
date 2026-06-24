#include "nolibc.h"

#include "capture.h"
#include "mouse.h"

int mouse_tracking_enabled = 0;
int dragging_corner = -1;
int drag_start_x = 0;
int drag_start_y = 0;
int drag_start_w = 0;
int drag_start_h = 0;

void enable_mouse_tracking(void) {
  // \033[?1002h = button-event tracking (press, release, and drag)
  write(STDOUT_FILENO, "\033[?1002h", 8);
  mouse_tracking_enabled = 1;
}

void disable_mouse_tracking(void) {
  write(STDOUT_FILENO, "\033[?1002l", 8);
  mouse_tracking_enabled = 0;
}

void draw_corner_indicator(int ascii_w, int ascii_h, int color) {
  char buf[256];
  int n;

  if (ascii_w < 3 || ascii_h < 2)
    return;

  // During an active drag
  int is_dragging = (dragging_corner == 1);

  if (color) {
    // U+2502 (│) = \342\224\202
    const char *vbar_color =
        is_dragging ? "\033[38;2;255;60;60m" : "\033[38;2;255;140;0m";
    n = nl_snprintf(buf, sizeof(buf), "\033[%d;%dH%s\342\224\202\033[0m",
                    ascii_h - 1, ascii_w, vbar_color);
    if (n > 0 && n < (int)sizeof(buf))
      write(STDOUT_FILENO, buf, (size_t)n);

    const char *corner_color =
        is_dragging ? "\033[38;2;255;60;60m" : "\033[38;2;255;140;0m";
    const char *corner_glyph = is_dragging
                                   ? "\342\224\200\342\224\200\342\244\241"
                                   : "\342\224\200\342\224\200\342\227\242";
    n = nl_snprintf(buf, sizeof(buf), "\033[%d;%dH%s%s\033[0m", ascii_h,
                    ascii_w - 2, corner_color, corner_glyph);
    if (n > 0 && n < (int)sizeof(buf))
      write(STDOUT_FILENO, buf, (size_t)n);
  } else {
    // Mono: reverse-video
    const char *attr = is_dragging ? "\033[7;5m" : "\033[7m";

    n = nl_snprintf(buf, sizeof(buf), "\033[%d;%dH%s\342\224\202\033[0m",
                    ascii_h - 1, ascii_w, attr);
    if (n > 0 && n < (int)sizeof(buf))
      write(STDOUT_FILENO, buf, (size_t)n);

    // ASCII fallback
    const char *glyph = is_dragging ? "==>" : "--+";
    n = nl_snprintf(buf, sizeof(buf), "\033[%d;%dH%s%s\033[0m", ascii_h,
                    ascii_w - 2, attr, glyph);
    if (n > 0 && n < (int)sizeof(buf))
      write(STDOUT_FILENO, buf, (size_t)n);
  }
}

int handle_mouse_event(int button, int x, int y, int *cap_w, int *cap_h,
                       webcam_t *cam, int ascii_w, int ascii_h) {
  (void)cam;

  // Decode event type
  int is_wheel = (button & 64) != 0;
  int is_motion = !is_wheel && (button & 32) != 0;
  int is_press = !is_wheel && !is_motion && (button & 3) == 0; // left press
  int is_drag = is_motion && (button & 3) == 0;                // left drag
  int is_release = !is_wheel && (button & 3) == 3;

  // Bottom-right corner
  int corner_x = ascii_w;
  int corner_y = ascii_h;

  if (is_press && x >= corner_x - CORNER_DRAG_MARGIN && x <= corner_x &&
      y >= corner_y - CORNER_DRAG_MARGIN && y <= corner_y) {
    dragging_corner = 1;
    drag_start_x = x;
    drag_start_y = y;
    drag_start_w = *cap_w;
    drag_start_h = *cap_h;
    return 1;
  }

  if (is_drag && dragging_corner == 1) {
    int dx = x - drag_start_x;
    int dy = y - drag_start_y;

    int new_w = drag_start_w + dx * 8;
    int new_h = drag_start_h + dy * 8;

    if (new_w < 160)
      new_w = 160;
    if (new_w > 1280)
      new_w = 1280;
    if (new_h < 120)
      new_h = 120;
    if (new_h > 720)
      new_h = 720;

    *cap_w = new_w;
    *cap_h = new_h;
    return 3; // preview
  }

  // Release
  if (is_release && dragging_corner == 1) {
    dragging_corner = -1;
    if (*cap_w != drag_start_w || *cap_h != drag_start_h)
      return 2; // caller calls reinit_capture
    return 0;   // click with no movement
  }

  if (is_release)
    dragging_corner = -1;

  return 0;
}
