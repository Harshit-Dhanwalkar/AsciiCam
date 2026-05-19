#ifndef ASCII_H
#define ASCII_H

#include <stddef.h>
#include <stdint.h>

#define ASCII_CHARS_DEFAULT " .:-=+*#%@"

typedef struct {
  int brightness;      /* additive offset applied to gray: -128..128       */
  int contrast;        /* multiplier in percent; 100 = no change           */
  int invert;          /* non-zero: flip brightness -> charset mapping      */
  int color;           /* non-zero: emit ANSI truecolor escape codes        */
  int dither;          /* non-zero: apply Floyd-Steinberg dithering         */
  const char *charset; /* custom charset string; NULL -> ASCII_CHARS_DEFAULT */
} ascii_opts_t;

// Convert YUYV raw data to grayscale
void yuyv_to_gray(const uint8_t *yuyv, uint8_t *gray, int width, int height);
void yuyv_to_rgb(const uint8_t *yuyv, uint8_t *rgb, int width, int height);

// Output buffer sizing
size_t ascii_out_size(int dst_w, int dst_h, int color);

// grayscale to ASCII output grid
int grayscale_to_ascii(const uint8_t *gray, const uint8_t *rgb, int src_w,
                       int src_h, int dst_w, int dst_h, char *out,
                       size_t out_size, const ascii_opts_t *opts);

#endif
