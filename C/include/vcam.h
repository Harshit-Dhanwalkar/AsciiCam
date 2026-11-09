#ifndef VCAM_H
#define VCAM_H

/*
 * Virtual camera: ASCII picture as a webcam for Meet, Teams, Zoom
 *
 * Frame main already writes to terminal (cursor moves, colours, UTF-8 glyphs)
 * is read back by a small ANSI interpreter and drawn into a pixel canvas with a
 * built-in 8x16 bitmap font. Braille, half blocks and other block elements are
 * drawn as shapes, not looked up in font
 * Canvas is converted to YUYV and written to a v4l2loopback device
 *
 *   sudo modprobe v4l2loopback devices=1 video_nr=10 \
 *        card_label="AsciiCam" exclusive_caps=1
 *   asciicam --vcam /dev/video10
 *
 * Any other path works too: when ioctl that sets video format is refused (a
 * regular file or a FIFO), raw YUYV frames are written as they are, e.g.
 * `ffplay -f rawvideo -pixel_format yuyv422 -video_size 1280x720 f`
 */

#include "nl_types.h"

#define VCAM_DEFAULT_W 1280
#define VCAM_DEFAULT_H 720
#define VCAM_CELL_W 8
#define VCAM_CELL_H 16

typedef struct {
  uint32_t cp; // 0 = empty
  uint8_t fg[3];
  uint8_t bg[3];
  uint8_t bold;
} vcam_cell_t;

typedef struct {
  int fd;       // -1 until vcam_open
  int is_video; // 1 when format was set on a real video device
  int w, h;     // canvas pixels (w even)
  int cols, rows;
  uint8_t *rgb;  // w*h*3
  uint8_t *yuyv; // w*h*2
  vcam_cell_t *cells;
  uint32_t frames;
  uint32_t unknown; // glyphs drawn as a hollow box
} vcam_t;

// Allocate a w x h canvas (w even, at least one cell). 0 or -1
int vcam_init(vcam_t *v, int w, int h);

// Open path for writing and set video format when it is a video device
// 0 on success, -1 on failure (errno set by open)
int vcam_open(vcam_t *v, const char *path);

// Draw an ANSI frame into canvas (no output)
// Frame is centred, or cropped at top left when larger than canvas
void vcam_render(vcam_t *v, const char *ansi, size_t len);

// RGB canvas written by vcam_render
const uint8_t *vcam_pixels(const vcam_t *v);

// RGB canvas to YUYV (BT.601, limited range) into v->yuyv
void vcam_to_yuyv(vcam_t *v);

// vcam_render + vcam_to_yuyv + write
// 0, or -1 when write fails
int vcam_frame(vcam_t *v, const char *ansi, size_t len);

void vcam_close(vcam_t *v);

#endif
