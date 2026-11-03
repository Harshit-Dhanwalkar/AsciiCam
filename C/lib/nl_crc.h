#ifndef NL_CRC_H
#define NL_CRC_H

/*
 * CRC-32 (IEEE 802.3, as in PNG/zlib/gzip) and Adler-32 (zlib), streaming and
 * header-only
 *
 *   uint32_t c = nl_crc32_update(0, buf, n);        // chain: feed previous c
 *   uint32_t a = nl_adler32_update(1, buf, n);      // Adler-32 starts at 1
 *
 * Check values:
 *  crc32("123456789") = 0xCBF43926
 *  adler32("Wikipedia") = 0x11E60398
 */

#include "nl_types.h"

static inline uint32_t nl_crc32_update(uint32_t crc, const void *data,
                                       size_t n) {
  static uint32_t table[256];
  static int ready;
  if (!ready) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }

      table[i] = c;
    }

    ready = 1;
  }

  const uint8_t *p = (const uint8_t *)data;
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  }

  return ~crc;
}

static inline uint32_t nl_adler32_update(uint32_t adler, const void *data,
                                         size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t a = adler & 0xFFFF;
  uint32_t b = adler >> 16;
  while (n > 0) {
    size_t chunk = n < 5552 ? n : 5552; // largest run before b can overflow
    n -= chunk;
    while (chunk--) {
      a += *p++;
      b += a;
    }

    a %= 65521;
    b %= 65521;
  }

  return (b << 16) | a;
}

#endif
