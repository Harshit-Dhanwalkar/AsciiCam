#ifndef IMAGE_H
#define IMAGE_H

/*
 * Picture as camera: `--image <file>` / `-d image:<file>`
 *
 * Decodes PNG (every colour type and bit depth, not interlaced), baseline JPEG
 * and binary PGM/PPM (P5/P6) with own inflate and IDCT
 * Anything with transparency is shown over black
 * Picture is scaled (area average) to fit capture size, aspect kept, black bars
 * where it does not fill
 */

#include "nl_types.h"

#define IMAGE_DEVICE_PREFIX "image:"
#define IMAGE_MAX_PIXELS (16u * 1024u * 1024u) // 4096 x 4096

typedef struct {
  int w, h;
  uint8_t *rgb; // w * h * 3, from nl_malloc
} image_t;

// Device string "image:<path>": path, or NULL when device names something else
const char *image_device_path(const char *device);

// Decode PNG or PNM bytes. 0 on success, -1 and a short message in *err
int image_decode(const uint8_t *data, size_t n, image_t *out, const char **err);

// Read and decode a file
int image_load(const char *path, image_t *out, const char **err);

void image_free(image_t *img);

// Fill a YUYV frame of w x h pixels (w even), rows stride bytes apart, with
// picture scaled to fit and centred, BT.601 limited range
void image_fill_yuyv(const image_t *img, uint8_t *yuyv, int w, int h,
                     int stride);

// Baseline JPEG (image_decode calls it): sequential Huffman, 8 bit, grayscale
// or YCbCr, any sampling, restart intervals. Progressive and CMYK are refused
int jpeg_decode(const uint8_t *data, size_t n, image_t *out, const char **err);

// 8 x 8 inverse DCT of dequantized coefficients (natural order) into 8 rows of
// samples stride bytes apart, level shifted and clamped. Exposed for tests
void jpeg_idct(const int *coef, uint8_t *out, int stride);

// zlib stream (RFC 1950, checksum not verified) into out of cap bytes: bytes
// written, or Return -1 on bad data or when it does not fit
// Exposed for tests
long image_inflate(const uint8_t *src, size_t n, uint8_t *out, size_t cap);

#endif
