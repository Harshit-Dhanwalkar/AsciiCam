#ifndef ASCII_H
#define ASCII_H

#include <stddef.h>
#include <stdint.h>

#define ASCII_CHARS_DEFAULT " .:-=+*#%@"

typedef struct {
  int brightness;      /* additive offset: -128..128       */
  int contrast;        /* percent, 100 = no change         */
  int invert;          /* flip brightness->charset mapping */
  int color;           /* ANSI truecolor output            */
  int edges;           /* Sobel edge detection             */
  int dither;          /* Floyd-Steinberg dithering        */
  const char *charset; /* NULL = ASCII_CHARS_DEFAULT  */
} ascii_opts_t;

// Convert YUYV raw data to grayscale
// void yuyv_to_gray(const uint8_t *yuyv, uint8_t *gray, int width, int height);
void yuyv_to_gray_simd(const uint8_t *yuyv, uint8_t *gray, int width, int height);
void yuyv_to_rgb(const uint8_t *yuyv, uint8_t *rgb, int width, int height);

// Output buffer sizing
size_t ascii_out_size(int dst_w, int dst_h, int color);

// grayscale to ASCII output grid
int grayscale_to_ascii(const uint8_t *gray, const uint8_t *rgb, int src_w,
                       int src_h, int dst_w, int dst_h, char *out,
                       size_t out_size, const ascii_opts_t *opts);

// Overlay FPS box
void overlay_fps_box(int dst_w, double fps, int color_enabled);

#endif
