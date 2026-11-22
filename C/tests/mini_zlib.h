/*
 * tests/mini_zlib.h - just enough zlib / PNG to check files in C tests
 *
 * crc32, adler32 and a complete inflate (stored, fixed and dynamic Huffman
 * blocks) written for tests, so they need no zlib
 * Meant for reading what program wrote, not for speed
 * Structure follows well-known "puff" decoder: canonical Huffman codes decoded
 * bit by bit from count / symbol tables
 *
 *   unsigned char *out; size_t n;
 *   int ok = mz_zlib_decompress(data, len, &out, &n);   // 1 on success
 *   free(out);
 *
 *   mz_zlib_compress(data, len, MZ_DYNAMIC, &z, &zn);   // or MZ_FIXED /
 *                                                             MZ_STORED
 *
 *   mz_png_write("a.png", w, h, pixel_fn, ctx, interlace, MZ_DYNAMIC);
 *
 *
 *   mz_png_info png;
 *   int ok = mz_png_read("a.png", &png);   // checks signature, every chunk CRC
 *                                          // and that pixel data inflates to
 *                                          // exactly h * (1 + w * channels)
 */

#ifndef MINI_ZLIB_H
#define MINI_ZLIB_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t mz_crc32(const unsigned char *p, size_t n, uint32_t crc) {
  static uint32_t table[256];
  if (!table[1]) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) {
        c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      }

      table[i] = c;
    }
  }

  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  }

  return ~crc;
}

static uint32_t mz_adler32(const unsigned char *p, size_t n) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < n; i++) {
    a = (a + p[i]) % 65521;
    b = (b + a) % 65521;
  }

  return (b << 16) | a;
}

/* Inflate */

#define MZ_MAXBITS 15

typedef struct {
  short count[MZ_MAXBITS + 1];
  short symbol[288];
} mz_huff;

typedef struct {
  const unsigned char *in;
  size_t inlen;
  size_t incnt;
  int bitbuf;
  int bitcnt;
  unsigned char *out;
  size_t outlen;
  size_t outcap;
  int err;
} mz_state;

static int mz_bits(mz_state *s, int need) {
  long val = s->bitbuf;
  while (s->bitcnt < need) {
    if (s->incnt >= s->inlen) {
      s->err = 1; // Ran out of input

      return 0;
    }

    val |= (long)s->in[s->incnt++] << s->bitcnt;
    s->bitcnt += 8;
  }

  s->bitbuf = (int)(val >> need);
  s->bitcnt -= need;

  return (int)(val & ((1L << need) - 1));
}

static void mz_put(mz_state *s, unsigned char c) {
  if (s->outlen == s->outcap) {
    s->outcap = s->outcap ? s->outcap * 2 : 65536;
    s->out = (unsigned char *)realloc(s->out, s->outcap);
    if (!s->out) {
      abort();
    }
  }

  s->out[s->outlen++] = c;
}

static int mz_decode(mz_state *s, const mz_huff *h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= MZ_MAXBITS; len++) {
    code |= mz_bits(s, 1);
    if (s->err) {
      return -1;
    }

    int count = h->count[len];
    if (code - count < first) {
      return h->symbol[index + (code - first)];
    }

    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }

  s->err = 2;

  return -1;
}

// Build decoding tables from code lengths
// 0: complete, <0: over-subscribed, >0: incomplete (allowed only for a single
// distance code)
static int mz_construct(mz_huff *h, const short *length, int n) {
  short offs[MZ_MAXBITS + 1];
  for (int len = 0; len <= MZ_MAXBITS; len++) {
    h->count[len] = 0;
  }

  for (int sym = 0; sym < n; sym++) {
    h->count[length[sym]]++;
  }

  if (h->count[0] == n) {
    return 0;
  }

  int left = 1;
  for (int len = 1; len <= MZ_MAXBITS; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) {
      return left;
    }
  }

  offs[1] = 0;
  for (int len = 1; len < MZ_MAXBITS; len++) {
    offs[len + 1] = (short)(offs[len] + h->count[len]);
  }

  for (int sym = 0; sym < n; sym++) {
    if (length[sym]) {
      h->symbol[offs[length[sym]]++] = (short)sym;
    }
  }

  return left;
}

static int mz_codes(mz_state *s, const mz_huff *lencode,
                    const mz_huff *distcode) {
  static const short lens[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                 15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const short dists[30] = {
      1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
      33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const short dext[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                 4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    int sym = mz_decode(s, lencode);
    if (s->err) {
      return -1;
    }

    if (sym < 256) {
      mz_put(s, (unsigned char)sym);
    } else if (sym == 256) {
      return 0;
    } else {
      sym -= 257;
      if (sym >= 29) {
        s->err = 3;

        return -1;
      }

      int len = lens[sym] + mz_bits(s, lext[sym]);
      int ds = mz_decode(s, distcode);
      if (s->err || ds < 0 || ds >= 30) {
        s->err = s->err ? s->err : 4;

        return -1;
      }

      size_t dist = (size_t)dists[ds] + (size_t)mz_bits(s, dext[ds]);
      if (s->err || dist > s->outlen) {
        s->err = s->err ? s->err : 5; // Reaches before start

        return -1;
      }

      while (len--) {
        mz_put(s, s->out[s->outlen - dist]);
      }
    }
  }
}

static int mz_stored(mz_state *s) {
  s->bitbuf = 0;
  s->bitcnt = 0;
  if (s->incnt + 4 > s->inlen) {
    s->err = 6;

    return -1;
  }

  unsigned len = s->in[s->incnt] | (unsigned)(s->in[s->incnt + 1] << 8);
  unsigned nlen = s->in[s->incnt + 2] | (unsigned)(s->in[s->incnt + 3] << 8);
  s->incnt += 4;
  if (len != (~nlen & 0xffff) || s->incnt + len > s->inlen) {
    s->err = 7;

    return -1;
  }

  while (len--) {
    mz_put(s, s->in[s->incnt++]);
  }

  return 0;
}

static int mz_fixed(mz_state *s) {
  static mz_huff lencode, distcode;
  static int built;
  if (!built) {
    short lengths[288];
    int sym = 0;
    for (; sym < 144; sym++) {
      lengths[sym] = 8;
    }

    for (; sym < 256; sym++) {
      lengths[sym] = 9;
    }

    for (; sym < 280; sym++) {
      lengths[sym] = 7;
    }

    for (; sym < 288; sym++) {
      lengths[sym] = 8;
    }

    mz_construct(&lencode, lengths, 288);
    for (sym = 0; sym < 30; sym++) {
      lengths[sym] = 5;
    }

    mz_construct(&distcode, lengths, 30);
    built = 1;
  }

  return mz_codes(s, &lencode, &distcode);
}

static int mz_dynamic(mz_state *s) {
  static const short order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                  11, 4,  12, 3, 13, 2, 14, 1, 15};
  short lengths[320];
  mz_huff lencode, distcode;
  int nlen = mz_bits(s, 5) + 257;
  int ndist = mz_bits(s, 5) + 1;
  int ncode = mz_bits(s, 4) + 4;
  if (s->err || nlen > 286 || ndist > 30) {
    s->err = s->err ? s->err : 8;

    return -1;
  }

  int index = 0;
  for (; index < ncode; index++) {
    lengths[order[index]] = (short)mz_bits(s, 3);
  }

  for (; index < 19; index++) {
    lengths[order[index]] = 0;
  }

  if (s->err || mz_construct(&lencode, lengths, 19) != 0) {
    s->err = s->err ? s->err : 9;

    return -1;
  }

  index = 0;
  while (index < nlen + ndist) {
    int sym = mz_decode(s, &lencode);
    if (s->err) {
      return -1;
    }

    if (sym < 16) {
      lengths[index++] = (short)sym;
    } else {
      int len = 0, rep;
      if (sym == 16) {
        if (index == 0) {
          s->err = 10;

          return -1;
        }

        len = lengths[index - 1];
        rep = 3 + mz_bits(s, 2);
      } else if (sym == 17) {
        rep = 3 + mz_bits(s, 3);
      } else {
        rep = 11 + mz_bits(s, 7);
      }

      if (s->err || index + rep > nlen + ndist) {
        s->err = s->err ? s->err : 11;

        return -1;
      }

      while (rep--) {
        lengths[index++] = (short)len;
      }
    }
  }

  if (lengths[256] == 0) {
    s->err = 12; // No end-of-block code

    return -1;
  }

  int e = mz_construct(&lencode, lengths, nlen);
  if (e < 0 || (e > 0 && nlen - lencode.count[0] != 1)) {
    s->err = 13;

    return -1;
  }

  e = mz_construct(&distcode, lengths + nlen, ndist);
  if (e < 0 || (e > 0 && ndist - distcode.count[0] != 1)) {
    s->err = 14;

    return -1;
  }

  return mz_codes(s, &lencode, &distcode);
}

// Raw deflate stream
// Returns 1 and output (caller frees) on success
static int mz_inflate(const unsigned char *in, size_t inlen,
                      unsigned char **out, size_t *outlen, size_t *consumed) {
  mz_state s;
  memset(&s, 0, sizeof s);
  s.in = in;
  s.inlen = inlen;
  int last;
  do {
    last = mz_bits(&s, 1);
    int type = mz_bits(&s, 2);
    if (s.err) {
      break;
    }

    if (type == 0) {
      mz_stored(&s);
    } else if (type == 1) {
      mz_fixed(&s);
    } else if (type == 2) {
      mz_dynamic(&s);
    } else {
      s.err = 15;
    }
  } while (!last && !s.err);

  if (s.err) {
    free(s.out);
    *out = NULL;
    *outlen = 0;

    return 0;
  }

  *out = s.out ? s.out : (unsigned char *)malloc(1);
  *outlen = s.outlen;
  if (consumed) {
    *consumed = s.incnt;
  }

  return 1;
}

// zlib wrapper: 2-byte header, deflate data, Adler-32 of output
static int mz_zlib_decompress(const unsigned char *in, size_t inlen,
                              unsigned char **out, size_t *outlen) {
  size_t used = 0;
  *out = NULL;
  *outlen = 0;
  if (inlen < 6 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 ||
      (in[1] & 0x20)) {
    return 0;
  }

  if (!mz_inflate(in + 2, inlen - 2, out, outlen, &used)) {
    return 0;
  }

  if (2 + used + 4 > inlen) {
    free(*out);
    *out = NULL;

    return 0;
  }

  const unsigned char *a = in + 2 + used;
  uint32_t want = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
                  ((uint32_t)a[2] << 8) | a[3];
  if (mz_adler32(*out, *outlen) != want) {
    free(*out);
    *out = NULL;

    return 0;
  }

  return 1;
}

/* deflate (encoder) */

// Which kind of deflate block to write
// A test that wants to exercise a decoder's dynamic-Huffman path asks for it
// instead of hoping compressor picks it
typedef enum { MZ_STORED = 0, MZ_FIXED = 1, MZ_DYNAMIC = 2 } mz_mode;

typedef struct {
  unsigned char *buf;
  size_t n;
  size_t cap;
  uint32_t acc;
  int nbits;
} mz_bw;

static void mz_bw_byte(mz_bw *w, unsigned char c) {
  if (w->n == w->cap) {
    w->cap = w->cap ? w->cap * 2 : 4096;
    w->buf = (unsigned char *)realloc(w->buf, w->cap);
    if (!w->buf) {
      abort();
    }
  }

  w->buf[w->n++] = c;
}

// Bits are packed starting at least significant end
static void mz_bw_put(mz_bw *w, uint32_t value, int nbits) {
  w->acc |= value << w->nbits;
  w->nbits += nbits;
  while (w->nbits >= 8) {
    mz_bw_byte(w, (unsigned char)(w->acc & 0xff));
    w->acc >>= 8;
    w->nbits -= 8;
  }
}

static void mz_bw_flush(mz_bw *w) {
  if (w->nbits > 0) {
    mz_bw_byte(w, (unsigned char)(w->acc & 0xff));
  }

  w->acc = 0;
  w->nbits = 0;
}

// Huffman codes go out most significant bit first
static void mz_bw_code(mz_bw *w, uint32_t code, int len) {
  uint32_t r = 0;
  for (int i = 0; i < len; i++) {
    r = (r << 1) | ((code >> i) & 1);
  }

  mz_bw_put(w, r, len);
}

typedef struct {
  uint16_t sym;  // Literal 0..255, or length 3..258
  uint16_t dist; // 0 for a literal
} mz_tok;

// Greedy LZ77 with hash chains over a 32 KiB window
static mz_tok *mz_lz77(const unsigned char *in, size_t n, size_t *ntok) {
  mz_tok *t = (mz_tok *)malloc((n + 1) * sizeof *t);
  size_t k = 0;
  enum { HB = 15, HS = 1 << HB };
  int32_t *head = (int32_t *)malloc(HS * sizeof *head);
  int32_t *prev = (int32_t *)malloc((n + 1) * sizeof *prev);
  if (!t || !head || !prev) {
    abort();
  }

  for (int i = 0; i < HS; i++) {
    head[i] = -1;
  }

  size_t i = 0;
  while (i < n) {
    size_t best = 0;
    size_t bestd = 0;
    uint32_t h = 0;
    if (i + 3 <= n) {
      h = (((uint32_t)in[i] << 10) ^ ((uint32_t)in[i + 1] << 5) ^ in[i + 2]) &
          (HS - 1);
      int32_t c = head[h];
      for (int depth = 0; c >= 0 && depth < 48 && i - (size_t)c <= 32768;
           depth++, c = prev[c]) {
        size_t l = 0;
        while (i + l < n && l < 258 && in[(size_t)c + l] == in[i + l]) {
          l++;
        }

        if (l > best) {
          best = l;
          bestd = i - (size_t)c;
          if (l == 258) {
            break;
          }
        }
      }
    }

    size_t step = 1;
    if (best >= 3) {
      t[k].sym = (uint16_t)best;
      t[k].dist = (uint16_t)bestd;
      step = best;
    } else {
      t[k].sym = in[i];
      t[k].dist = 0;
    }

    k++;
    for (size_t j = 0; j < step; j++) { // Index every position we pass over
      if (i + j + 3 <= n) {
        uint32_t hh = (((uint32_t)in[i + j] << 10) ^
                       ((uint32_t)in[i + j + 1] << 5) ^ in[i + j + 2]) &
                      (HS - 1);
        prev[i + j] = head[hh];
        head[hh] = (int32_t)(i + j);
      }
    }

    i += step;
    (void)h;
  }

  free(head);
  free(prev);
  *ntok = k;

  return t;
}

static const short mz_len_base[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short mz_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                       1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                       4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short mz_dist_base[30] = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short mz_dist_extra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                        4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                        9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int mz_len_sym(int len) { // 3..258 -> 0..28
  int s = 28;
  while (mz_len_base[s] > len) {
    s--;
  }

  return s;
}

static int mz_dist_sym(int dist) { // 1..32768 -> 0..29
  int s = 29;
  while (mz_dist_base[s] > dist) {
    s--;
  }

  return s;
}

// Code lengths from symbol frequencies (Huffman), none longer than `limit`.
// Frequencies are flattened and tree rebuilt until limit holds, same trick zlib
// uses
// A lone symbol gets length 1 and a dummy partner so code stays complete
static void mz_code_lengths(const uint32_t *freq, int n, int limit,
                            short *len) {
  uint32_t f[320];
  for (int i = 0; i < n; i++) {
    f[i] = freq[i];
  }

  for (;;) {
    int used = 0;
    for (int i = 0; i < n; i++) {
      len[i] = 0;
      used += f[i] != 0;
    }

    if (used == 0) {
      return;
    }

    if (used == 1) {
      int only = 0;
      for (int i = 0; i < n; i++) {
        if (f[i]) {
          only = i;
        }
      }

      len[only] = 1;
      len[only == 0 ? 1 : 0] = 1;

      return;
    }

    // Tree nodes: leaves 0..n-1, internal n..; simple O(n^2) selection is
    // plenty for 288 symbols
    uint64_t w[640];
    int parent[640];
    int alive[640];
    int nodes = n;
    for (int i = 0; i < n; i++) {
      w[i] = f[i];
      parent[i] = -1;
      alive[i] = f[i] != 0;
    }

    for (int remaining = used; remaining > 1; remaining--) {
      int a = -1;
      int b = -1;
      for (int i = 0; i < nodes; i++) {
        if (!alive[i]) {
          continue;
        }

        if (a < 0 || w[i] < w[a]) {
          b = a;
          a = i;
        } else if (b < 0 || w[i] < w[b]) {
          b = i;
        }
      }

      w[nodes] = w[a] + w[b];
      parent[nodes] = -1;
      parent[a] = parent[b] = nodes;
      alive[a] = alive[b] = 0;
      alive[nodes] = 1;
      nodes++;
    }

    int maxlen = 0;
    for (int i = 0; i < n; i++) {
      if (!f[i]) {
        continue;
      }

      int d = 0;
      for (int p = i; parent[p] >= 0; p = parent[p]) {
        d++;
      }

      len[i] = (short)d;
      if (d > maxlen) {
        maxlen = d;
      }
    }

    if (maxlen <= limit) {
      return;
    }

    for (int i = 0; i < n; i++) {
      if (f[i]) {
        f[i] = (f[i] >> 1) + 1;
      }
    }
  }
}

// Canonical codes from lengths
static void mz_codes_from_lengths(const short *len, int n, uint16_t *code) {
  int count[MZ_MAXBITS + 1] = {0}, next[MZ_MAXBITS + 2] = {0};
  for (int i = 0; i < n; i++) {
    count[len[i]]++;
  }

  count[0] = 0;
  int c = 0;
  for (int b = 1; b <= MZ_MAXBITS; b++) {
    c = (c + count[b - 1]) << 1;
    next[b] = c;
  }

  for (int i = 0; i < n; i++) {
    code[i] = len[i] ? (uint16_t)next[len[i]]++ : 0;
  }
}

static void mz_emit_tokens(mz_bw *w, const mz_tok *t, size_t nt,
                           const uint16_t *lcode, const short *llen,
                           const uint16_t *dcode, const short *dlen) {
  for (size_t i = 0; i < nt; i++) {
    if (!t[i].dist) {
      mz_bw_code(w, lcode[t[i].sym], llen[t[i].sym]);

      continue;
    }

    int ls = mz_len_sym(t[i].sym);
    mz_bw_code(w, lcode[257 + ls], llen[257 + ls]);
    mz_bw_put(w, (uint32_t)(t[i].sym - mz_len_base[ls]), mz_len_extra[ls]);
    int ds = mz_dist_sym(t[i].dist);
    mz_bw_code(w, dcode[ds], dlen[ds]);
    mz_bw_put(w, (uint32_t)(t[i].dist - mz_dist_base[ds]), mz_dist_extra[ds]);
  }

  mz_bw_code(w, lcode[256], llen[256]);
}

// Raw deflate stream of `in` in requested kind of block (one block, or several
// for stored)
// Output is malloc'd
static void mz_deflate(const unsigned char *in, size_t n, mz_mode mode,
                       unsigned char **out, size_t *outlen) {
  mz_bw w;
  memset(&w, 0, sizeof w);

  if (mode == MZ_STORED) {
    size_t pos = 0;
    do {
      size_t chunk = n - pos > 65535 ? 65535 : n - pos;
      int last = pos + chunk == n;
      mz_bw_put(&w, (uint32_t)last, 1);
      mz_bw_put(&w, 0, 2);
      mz_bw_flush(&w);
      mz_bw_byte(&w, (unsigned char)(chunk & 0xff));
      mz_bw_byte(&w, (unsigned char)(chunk >> 8));
      mz_bw_byte(&w, (unsigned char)(~chunk & 0xff));
      mz_bw_byte(&w, (unsigned char)((~chunk >> 8) & 0xff));
      for (size_t i = 0; i < chunk; i++) {
        mz_bw_byte(&w, in[pos + i]);
      }

      pos += chunk;
    } while (pos < n);
    *out = w.buf;
    *outlen = w.n;

    return;
  }

  size_t nt;
  mz_tok *t = mz_lz77(in, n, &nt);
  short llen[288], dlen[30];
  uint16_t lcode[288], dcode[30];

  if (mode == MZ_FIXED) {
    int i = 0;
    for (; i < 144; i++) {
      llen[i] = 8;
    }

    for (; i < 256; i++) {
      llen[i] = 9;
    }

    for (; i < 280; i++) {
      llen[i] = 7;
    }

    for (; i < 288; i++) {
      llen[i] = 8;
    }

    for (i = 0; i < 30; i++) {
      dlen[i] = 5;
    }

    mz_codes_from_lengths(llen, 288, lcode);
    mz_codes_from_lengths(dlen, 30, dcode);
    mz_bw_put(&w, 1, 1); // Final
    mz_bw_put(&w, 1, 2); // Fixed
    mz_emit_tokens(&w, t, nt, lcode, llen, dcode, dlen);
  } else {
    uint32_t lf[288] = {0};
    uint32_t df[30] = {0};
    for (size_t i = 0; i < nt; i++) {
      if (!t[i].dist) {
        lf[t[i].sym]++;
      } else {
        lf[257 + mz_len_sym(t[i].sym)]++;
        df[mz_dist_sym(t[i].dist)]++;
      }
    }

    lf[256] = 1;
    mz_code_lengths(lf, 288, 15, llen);
    mz_code_lengths(df, 30, 15, dlen);
    mz_codes_from_lengths(llen, 288, lcode);
    mz_codes_from_lengths(dlen, 30, dcode);

    int nlit = 288;
    while (nlit > 257 && llen[nlit - 1] == 0) {
      nlit--;
    }

    int ndist = 30;
    while (ndist > 1 && dlen[ndist - 1] == 0) {
      ndist--;
    }

    unsigned char rl[320 * 2];
    int nrl = 0;
    short all[320];
    for (int i = 0; i < nlit; i++) {
      all[i] = llen[i];
    }

    for (int i = 0; i < ndist; i++) {
      all[nlit + i] = dlen[i];
    }

    int total = nlit + ndist;
    for (int i = 0; i < total;) {
      int run = 1;
      while (i + run < total && all[i + run] == all[i]) {
        run++;
      }

      if (all[i] == 0 && run >= 3) {
        int r = run > 138 ? 138 : run;
        if (r >= 11) {
          rl[nrl++] = 18;
          rl[nrl++] = (unsigned char)(r - 11);
        } else {
          rl[nrl++] = 17;
          rl[nrl++] = (unsigned char)(r - 3);
        }

        i += r;
      } else if (all[i] != 0 && run >= 4) {
        rl[nrl++] = (unsigned char)all[i]; // First one is written plainly
        rl[nrl++] = 0;
        int r = run - 1 > 6 ? 6 : run - 1;
        rl[nrl++] = 16;
        rl[nrl++] = (unsigned char)(r - 3);
        i += 1 + r;
      } else {
        rl[nrl++] = (unsigned char)all[i];
        rl[nrl++] = 0;
        i++;
      }
    }

    // rl holds (symbol, extra) pairs
    uint32_t cf[19] = {0};
    for (int i = 0; i < nrl; i += 2) {
      cf[rl[i]]++;
    }

    short clen[19];
    uint16_t ccode[19];
    mz_code_lengths(cf, 19, 7, clen);
    mz_codes_from_lengths(clen, 19, ccode);
    static const unsigned char order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                            11, 4,  12, 3, 13, 2, 14, 1, 15};
    int nclen = 19;
    while (nclen > 4 && clen[order[nclen - 1]] == 0) {
      nclen--;
    }

    mz_bw_put(&w, 1, 1); // Final
    mz_bw_put(&w, 2, 2); // Dynamic
    mz_bw_put(&w, (uint32_t)(nlit - 257), 5);
    mz_bw_put(&w, (uint32_t)(ndist - 1), 5);
    mz_bw_put(&w, (uint32_t)(nclen - 4), 4);
    for (int i = 0; i < nclen; i++) {
      mz_bw_put(&w, (uint32_t)clen[order[i]], 3);
    }

    for (int i = 0; i < nrl; i += 2) {
      int sym = rl[i];
      mz_bw_code(&w, ccode[sym], clen[sym]);
      if (sym == 16) {
        mz_bw_put(&w, rl[i + 1], 2);
      } else if (sym == 17) {
        mz_bw_put(&w, rl[i + 1], 3);
      } else if (sym == 18) {
        mz_bw_put(&w, rl[i + 1], 7);
      }
    }

    mz_emit_tokens(&w, t, nt, lcode, llen, dcode, dlen);
  }

  mz_bw_flush(&w);
  free(t);
  *out = w.buf;
  *outlen = w.n;
}

// zlib stream: header, deflate data, Adler-32
static void mz_zlib_compress(const unsigned char *in, size_t n, mz_mode mode,
                             unsigned char **out, size_t *outlen) {
  unsigned char *d;
  size_t dn;
  mz_deflate(in, n, mode, &d, &dn);
  unsigned char *z = (unsigned char *)malloc(dn + 6);
  z[0] = 0x78;
  z[1] = 0x9c;
  memcpy(z + 2, d, dn);
  uint32_t a = mz_adler32(in, n);
  z[dn + 2] = (unsigned char)(a >> 24);
  z[dn + 3] = (unsigned char)(a >> 16);
  z[dn + 4] = (unsigned char)(a >> 8);
  z[dn + 5] = (unsigned char)a;
  free(d);
  *out = z;
  *outlen = dn + 6;
}

/* PNG */

typedef struct {
  unsigned w;
  unsigned h;
  int channels; // 3 for truecolour (type 2), else 1
} mz_png_info;

static uint32_t mz_be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

// 1 when file is a PNG whose chunk CRCs are right and whose pixel data inflates
// to exactly h * (1 + w * channels) bytes (8-bit; for an Adam7 file sum over
// its seven passes)
static int mz_png_read(const char *path, mz_png_info *info) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return 0;
  }

  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *d = (unsigned char *)malloc(n > 0 ? (size_t)n : 1);
  if (!d || n < 8 || fread(d, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(d);

    return 0;
  }

  fclose(f);
  static const unsigned char sig[8] = {0x89, 'P',  'N',  'G',
                                       '\r', '\n', 0x1a, '\n'};
  unsigned char *idat = NULL;
  size_t idat_len = 0;
  int ok = !memcmp(d, sig, 8);
  int have_ihdr = 0;
  int ct = 0;
  int interlace = 0;
  unsigned w = 0;
  unsigned h = 0;
  size_t pos = 8;
  while (ok && pos + 12 <= (size_t)n) {
    size_t len = mz_be32(d + pos);
    if (pos + 12 + len > (size_t)n) {
      ok = 0;

      break;
    }

    const unsigned char *typ = d + pos + 4, *body = d + pos + 8;
    uint32_t crc = mz_crc32(typ, 4, 0);
    crc = mz_crc32(body, len, crc);
    if (crc != mz_be32(body + len)) {
      ok = 0;

      break;
    }

    if (!memcmp(typ, "IHDR", 4) && len >= 10) {
      w = mz_be32(body);
      h = mz_be32(body + 4);
      ct = body[9];
      interlace = len >= 13 ? body[12] : 0;
      have_ihdr = 1;
    } else if (!memcmp(typ, "IDAT", 4)) {
      idat = (unsigned char *)realloc(idat, idat_len + len + 1);
      memcpy(idat + idat_len, body, len);
      idat_len += len;
    }

    pos += 12 + len;
  }

  free(d);
  unsigned char *raw = NULL;
  size_t rawlen = 0;
  if (ok && have_ihdr && idat &&
      mz_zlib_decompress(idat, idat_len, &raw, &rawlen)) {
    int ch = ct == 2 ? 3 : 1;
    size_t want = 0;
    if (!interlace) {
      want = (size_t)h * (1 + (size_t)w * (size_t)ch);
    } else {
      // Adam7: seven passes, each a smaller image, an empty one has no rows
      static const int ps[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8},
                                   {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2},
                                   {0, 1, 1, 2}};
      for (int p = 0; p < 7; p++) {
        unsigned pw = w > (unsigned)ps[p][0]
                          ? (w - (unsigned)ps[p][0] + (unsigned)ps[p][2] - 1) /
                                (unsigned)ps[p][2]
                          : 0;
        unsigned ph = h > (unsigned)ps[p][1]
                          ? (h - (unsigned)ps[p][1] + (unsigned)ps[p][3] - 1) /
                                (unsigned)ps[p][3]
                          : 0;
        if (pw && ph) {
          want += (size_t)ph * (1 + (size_t)pw * (size_t)ch);
        }
      }
    }

    ok = rawlen == want;
    info->w = w;
    info->h = h;
    info->channels = ch;
  } else {
    ok = 0;
  }

  free(raw);
  free(idat);

  return ok;
}

// Writing PNGs for tests: 8-bit RGB, optionally Adam7-interlaced
// Pixel is computed by a callback so a fixture is described by what it looks
// like
typedef void (*mz_pixel_fn)(int x, int y, unsigned char rgb[3], void *ctx);

static void mz_png_chunk(FILE *f, const char *type, const unsigned char *d,
                         size_t n) {
  unsigned char hdr[8] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16),
                          (unsigned char)(n >> 8),  (unsigned char)n,
                          (unsigned char)type[0],   (unsigned char)type[1],
                          (unsigned char)type[2],   (unsigned char)type[3]};
  fwrite(hdr, 1, 8, f);
  if (n) {
    fwrite(d, 1, n, f);
  }

  uint32_t crc = mz_crc32((const unsigned char *)type, 4, 0);
  crc = mz_crc32(d, n, crc);
  unsigned char c[4] = {(unsigned char)(crc >> 24), (unsigned char)(crc >> 16),
                        (unsigned char)(crc >> 8), (unsigned char)crc};
  fwrite(c, 1, 4, f);
}

// `mode` is kind of deflate block pixel data is stored in
// Returns 1 when file was written
static int mz_png_write(const char *path, unsigned w, unsigned h,
                        mz_pixel_fn pix, void *ctx, int interlace,
                        mz_mode mode) {
  static const int pass[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8},
                                 {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2},
                                 {0, 1, 1, 2}};
  size_t cap = (size_t)h * (1 + (size_t)w * 3) + 16;
  size_t n = 0;
  unsigned char *raw = (unsigned char *)malloc(cap);
  if (!raw) {
    return 0;
  }

  int passes = interlace ? 7 : 1;
  for (int p = 0; p < passes; p++) {
    int x0 = interlace ? pass[p][0] : 0;
    int y0 = interlace ? pass[p][1] : 0;
    int dx = interlace ? pass[p][2] : 1;
    int dy = interlace ? pass[p][3] : 1;
    if (x0 >= (int)w) {
      continue; // An Adam7 pass with no pixels has no rows at all
    }

    for (int y = y0; y < (int)h; y += dy) {
      raw[n++] = 0; // Filter: none
      for (int x = x0; x < (int)w; x += dx) {
        pix(x, y, raw + n, ctx);
        n += 3;
      }
    }
  }

  unsigned char *z;
  size_t zn;
  mz_zlib_compress(raw, n, mode, &z, &zn);
  free(raw);

  FILE *f = fopen(path, "wb");
  if (!f) {
    free(z);

    return 0;
  }

  static const unsigned char sig[8] = {0x89, 'P',  'N',  'G',
                                       '\r', '\n', 0x1a, '\n'};
  fwrite(sig, 1, 8, f);
  unsigned char ihdr[13] = {(unsigned char)(w >> 24),
                            (unsigned char)(w >> 16),
                            (unsigned char)(w >> 8),
                            (unsigned char)w,
                            (unsigned char)(h >> 24),
                            (unsigned char)(h >> 16),
                            (unsigned char)(h >> 8),
                            (unsigned char)h,
                            8,
                            2,
                            0,
                            0,
                            (unsigned char)(interlace ? 1 : 0)};
  mz_png_chunk(f, "IHDR", ihdr, 13);
  mz_png_chunk(f, "IDAT", z, zn);
  mz_png_chunk(f, "IEND", NULL, 0);
  free(z);

  return fclose(f) == 0;
}

#endif
