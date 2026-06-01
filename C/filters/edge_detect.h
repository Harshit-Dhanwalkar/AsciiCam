#ifndef EDGE_DETECT_H
#define EDGE_DETECT_H

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

void do_edge_boost(uint8_t *gray, int w, int h, void *ctx);

static inline uint8_t *edge_detect(uint8_t *gray, int w, int h) {
  if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > (uint64_t)UINT32_MAX) {
    return NULL;
  }

  if (gray == NULL) {
    return NULL;
  }

  size_t size = (size_t)w * (size_t)h;
  uint8_t *out_buffer = malloc(size);
  if (!out_buffer) {
    return NULL;
  }

  for (size_t i = 0; i < size; i++) {
    out_buffer[i] = gray[i];
  }

  do_edge_boost(out_buffer, w, h, NULL);

  return out_buffer;
}

#endif
