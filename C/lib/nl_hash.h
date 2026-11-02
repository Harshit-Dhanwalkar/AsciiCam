#ifndef NL_HASH_H
#define NL_HASH_H

/*
 * SHA-256 (FIPS 180-4), streaming and header-only, no libc. Used to key plugin
 * cache on what a plugin was built FROM instead of on file times, which lie
 * after a `git checkout`, a `cp -r` or a clock change
 *
 *   nl_sha256_t h;
 *   nl_sha256_init(&h);
 *   nl_sha256_update(&h, buf, n);   // any number of times, any split
 *   nl_sha256_final(&h, digest);    // 32 bytes; h is wiped
 *   nl_sha256_hex(digest, hex);     // 64 chars + NUL
 *
 * NOT for secrets or authentication: a cache key only needs to notice change
 */

#include "nl_types.h" // NOTE: size_t/uint8_t: own definitions on Linux, system ones elsewhere

#define NL_SHA256_LEN 32
#define NL_SHA256_HEX_LEN 65

typedef struct {
  uint32_t st[8];
  uint8_t buf[64];
  uint64_t total; // bytes fed so far
  size_t fill;    // bytes waiting in buf
} nl_sha256_t;

static const uint32_t nl_sha256_k_[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t nl_sha256_rotr_(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

static inline void nl_sha256_block_(nl_sha256_t *h, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
           ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
  }

  for (int i = 16; i < 64; i++) {
    uint32_t s0 = nl_sha256_rotr_(w[i - 15], 7) ^
                  nl_sha256_rotr_(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = nl_sha256_rotr_(w[i - 2], 17) ^
                  nl_sha256_rotr_(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = h->st[0], b = h->st[1], c = h->st[2], d = h->st[3];
  uint32_t e = h->st[4], f = h->st[5], g = h->st[6], hh = h->st[7];
  for (int i = 0; i < 64; i++) {
    uint32_t s1 =
        nl_sha256_rotr_(e, 6) ^ nl_sha256_rotr_(e, 11) ^ nl_sha256_rotr_(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + s1 + ch + nl_sha256_k_[i] + w[i];
    uint32_t s0 =
        nl_sha256_rotr_(a, 2) ^ nl_sha256_rotr_(a, 13) ^ nl_sha256_rotr_(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + mj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  h->st[0] += a;
  h->st[1] += b;
  h->st[2] += c;
  h->st[3] += d;
  h->st[4] += e;
  h->st[5] += f;
  h->st[6] += g;
  h->st[7] += hh;
}

static inline void nl_sha256_init(nl_sha256_t *h) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                 0xa54ff53a, 0x510e527f, 0x9b05688c,
                                 0x1f83d9ab, 0x5be0cd19};
  for (int i = 0; i < 8; i++) {
    h->st[i] = iv[i];
  }

  h->total = 0;
  h->fill = 0;
}

static inline void nl_sha256_update(nl_sha256_t *h, const void *data,
                                    size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  h->total += n;
  while (n > 0) {
    if (h->fill == 0 && n >= 64) {
      nl_sha256_block_(h, p);
      p += 64;
      n -= 64;

      continue;
    }

    size_t take = 64 - h->fill;
    if (take > n) {
      take = n;
    }

    for (size_t i = 0; i < take; i++) {
      h->buf[h->fill + i] = p[i];
    }

    h->fill += take;
    p += take;
    n -= take;
    if (h->fill == 64) {
      nl_sha256_block_(h, h->buf);
      h->fill = 0;
    }
  }
}

static inline void nl_sha256_final(nl_sha256_t *h, uint8_t out[NL_SHA256_LEN]) {
  uint64_t bits = h->total * 8;
  uint8_t pad = 0x80;
  nl_sha256_update(h, &pad, 1);
  pad = 0;
  while (h->fill != 56) {
    nl_sha256_update(h, &pad, 1);
  }

  uint8_t len[8];
  for (int i = 0; i < 8; i++) {
    len[i] = (uint8_t)(bits >> (56 - 8 * i));
  }

  nl_sha256_update(h, len, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(h->st[i] >> 24);
    out[4 * i + 1] = (uint8_t)(h->st[i] >> 16);
    out[4 * i + 2] = (uint8_t)(h->st[i] >> 8);
    out[4 * i + 3] = (uint8_t)h->st[i];
  }

  volatile uint8_t *w = (volatile uint8_t *)h;
  for (size_t i = 0; i < sizeof(*h); i++) {
    w[i] = 0;
  }
}

static inline void nl_sha256_hex(const uint8_t d[NL_SHA256_LEN],
                                 char out[NL_SHA256_HEX_LEN]) {
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < NL_SHA256_LEN; i++) {
    out[2 * i] = hx[d[i] >> 4];
    out[2 * i + 1] = hx[d[i] & 15];
  }

  out[2 * NL_SHA256_LEN] = '\0';
}

#endif
