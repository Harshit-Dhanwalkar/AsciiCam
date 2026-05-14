#include "ascii.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x, msg) if ((x) < 0) { perror(msg); goto cleanup; }

typedef struct {
    uint8_t *raw_frame;
    uint8_t *gray;
    char *ascii;
} frame_t;

char *grayscale_to_ascii(const uint8_t *gray, int src_w, int src_h,
                         int dst_w, int dst_h) {
    // Allocate output string: each row has dst_w chars + newline, plus null terminator
    char *output = malloc(dst_w * dst_h + dst_h + 1);
    if (!output) return NULL;

    double block_w = (double)src_w / dst_w;
    double block_h = (double)src_h / dst_h;
    const char *ascii = ASCII_CHARS;
    int ascii_len = strlen(ascii);

    int out_idx = 0;
    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            long total = 0;
            int count = 0;
            int y_start = (int)(y * block_h);
            int y_end   = (int)((y + 1) * block_h);
            int x_start = (int)(x * block_w);
            int x_end   = (int)((x + 1) * block_w);

            for (int sy = y_start; sy < y_end && sy < src_h; sy++) {
                for (int sx = x_start; sx < x_end && sx < src_w; sx++) {
                    total += gray[sy * src_w + sx];
                    count++;
                }
            }
            unsigned char avg = (count > 0) ? (total / count) : 0;
            int idx = avg * (ascii_len - 1) / 255;
            output[out_idx++] = ascii[idx];
        }
        output[out_idx++] = '\n';
    }
    output[out_idx] = '\0';
    return output;
}
