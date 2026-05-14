#ifndef ASCII_H
#define ASCII_H

#include <stdint.h>

#define ASCII_CHARS " .:-=+*#%@"

// Convert YUYV raw data to grayscale
void yuyv_to_gray(const uint8_t *yuyv, uint8_t *gray, int width, int height);

// grayscale to ASCII output grid
char *grayscale_to_ascii(const uint8_t *gray, int src_w, int src_h,
                         int dst_w, int dst_h);

#endif
