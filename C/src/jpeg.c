/*
 * src/jpeg.c - baseline JPEG decoder, see include/image.h
 *
 * Sequential Huffman JPEG, 8 bit, grayscale or YCbCr (or RGB with an Adobe
 * marker), any sampling factors, restart intervals, several scans. Progressive,
 * arithmetic coded, 12 bit and CMYK files are refused
 * Subsampled chroma is interpolated linearly
 */

#include "platform.h"

#include "nolibc.h"

#include "image.h"

#define MAX_COMP 3

typedef struct {
  uint8_t bits[17]; // codes of each length, from DHT segment
  uint8_t val[256];
  int mincode[17], maxcode[18], valptr[17];
  uint16_t fast[512]; // 9 bit lookahead: length * 256 + symbol, 0 = longer
  int present;
} hufftab_t;

typedef struct {
  int id, h, v, tq;
  int bw, bh;     // blocks across and down, padded to whole MCUs
  int td, ta;     // table numbers of current scan
  int pred;       // DC predictor
  uint8_t *plane; // bw * 8 by bh * 8 samples
} comp_t;

typedef struct {
  const uint8_t *src;
  size_t n, pos;
  uint32_t buf; // bit buffer, MSB first in low `cnt` bits
  unsigned cnt;
  int hit_marker; // ran into a marker: feed zero bits from now on
  int err;
} bits_t;

// 2^n, so no signed shifts
static const int p2[18] = {1,    2,    4,     8,     16,    32,
                           64,   128,  256,   512,   1024,  2048,
                           4096, 8192, 16384, 32768, 65536, 131072};

static const uint8_t zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// IDCT
// libjpeg's jidctint ("islow"): 13 bit constants, exact enough that result
// matches libjpeg's to within 1

#define CONST_BITS 13
#define PASS1_BITS 2
#define F_0_298631336 2446
#define F_0_390180644 3196
#define F_0_541196100 4433
#define F_0_765366865 6270
#define F_0_899976223 7373
#define F_1_175875602 9633
#define F_1_501321110 12299
#define F_1_847759065 15137
#define F_1_961570560 16069
#define F_2_053119869 16819
#define F_2_562915447 20995
#define F_3_072711026 25172

// x / 2^n rounded to nearest, halves up: (x + 2^(n-1)) >> n without a signed
// shift
static int descale(int x, int n) {
  int d = 1;
  for (int i = 0; i < n; i++) {
    d *= 2;
  }

  int v = x + d / 2;

  return v >= 0 ? v / d : -((-v + d - 1) / d);
}

// One 8 point pass over in[0], in[stride] ... in[7 * stride] into out
static void idct_1d(const int *in, int stride, int *out, int ostride,
                    int shift) {
  int z2 = in[2 * stride], z3 = in[6 * stride];
  int z1 = (z2 + z3) * F_0_541196100;
  int tmp2 = z1 + z3 * -F_1_847759065;
  int tmp3 = z1 + z2 * F_0_765366865;
  z2 = in[0];
  z3 = in[4 * stride];
  int tmp0 = (z2 + z3) * 8192;
  int tmp1 = (z2 - z3) * 8192;
  int tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
  int tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;

  tmp0 = in[7 * stride];
  tmp1 = in[5 * stride];
  tmp2 = in[3 * stride];
  tmp3 = in[1 * stride];
  z1 = tmp0 + tmp3;
  z2 = tmp1 + tmp2;
  z3 = tmp0 + tmp2;
  int z4 = tmp1 + tmp3;
  int z5 = (z3 + z4) * F_1_175875602;
  tmp0 *= F_0_298631336;
  tmp1 *= F_2_053119869;
  tmp2 *= F_3_072711026;
  tmp3 *= F_1_501321110;
  z1 *= -F_0_899976223;
  z2 *= -F_2_562915447;
  z3 *= -F_1_961570560;
  z4 *= -F_0_390180644;
  z3 += z5;
  z4 += z5;
  tmp0 += z1 + z3;
  tmp1 += z2 + z4;
  tmp2 += z2 + z3;
  tmp3 += z1 + z4;

  out[0 * ostride] = descale(tmp10 + tmp3, shift);
  out[7 * ostride] = descale(tmp10 - tmp3, shift);
  out[1 * ostride] = descale(tmp11 + tmp2, shift);
  out[6 * ostride] = descale(tmp11 - tmp2, shift);
  out[2 * ostride] = descale(tmp12 + tmp1, shift);
  out[5 * ostride] = descale(tmp12 - tmp1, shift);
  out[3 * ostride] = descale(tmp13 + tmp0, shift);
  out[4 * ostride] = descale(tmp13 - tmp0, shift);
}

void jpeg_idct(const int *coef, uint8_t *out, int stride) {
  int ws[64];
  for (int c = 0; c < 8; c++) {
    idct_1d(coef + c, 8, ws + c, 8, CONST_BITS - PASS1_BITS);
  }

  for (int r = 0; r < 8; r++) {
    int row[8];
    idct_1d(ws + r * 8, 1, row, 1, CONST_BITS + PASS1_BITS + 3);
    for (int i = 0; i < 8; i++) {
      int v = row[i] + 128;
      out[r * stride + i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
  }
}

// Huffman

static int build_huff(hufftab_t *h) {
  int code = 0, k = 0;
  for (int l = 1; l <= 16; l++) {
    h->valptr[l] = k;
    h->mincode[l] = code;
    code += h->bits[l];
    k += h->bits[l];
    if (code > p2[l]) {
      return -1; // more codes than length has room for
    }

    h->maxcode[l] = h->bits[l] ? code - 1 : -1;
    code *= 2;
  }

  h->maxcode[17] = 0x7fffffff;
  for (int i = 0; i < 512; i++) {
    h->fast[i] = 0;
  }

  k = 0;
  code = 0;
  for (int l = 1; l <= 9; l++) {
    for (int i = 0; i < h->bits[l]; i++, k++, code++) {
      int first = code * p2[9 - l];
      for (int j = 0; j < p2[9 - l]; j++) {
        h->fast[first + j] = (uint16_t)(l * 256 + h->val[k]);
      }
    }

    code *= 2;
  }

  h->present = 1;

  return 0;
}

static void fill(bits_t *b) {
  while (b->cnt <= 24) {
    uint32_t byte = 0;
    if (!b->hit_marker && b->pos < b->n) {
      byte = b->src[b->pos];
      if (byte == 0xff) {
        uint32_t next = b->pos + 1 < b->n ? b->src[b->pos + 1] : 0xd9u;
        if (next == 0) {
          b->pos += 2; // stuffed zero after 0xff
        } else {
          b->hit_marker = 1; // a real marker: scan data ended
          byte = 0;
        }
      } else {
        b->pos++;
      }
    } else if (b->pos >= b->n) {
      b->hit_marker = 1;
    }

    b->buf = b->buf * 256u + byte;
    b->cnt += 8;
  }
}

static int get_bits(bits_t *b, int nbits) {
  unsigned n = (unsigned)nbits;
  if (n == 0) {
    return 0;
  }

  if (b->cnt < n) {
    fill(b);
  }

  b->cnt -= n;

  return (int)((b->buf >> b->cnt) & ((1u << n) - 1u));
}

static int decode_sym(bits_t *b, const hufftab_t *h) {
  if (b->cnt < 16) {
    fill(b);
  }

  unsigned f = h->fast[(b->buf >> (b->cnt - 9)) & 511u];
  if (f) { // every code of up to 9 bits is in table
    b->cnt -= f / 256u;

    return (int)(f & 255u);
  }

  for (unsigned l = 10; l <= 16; l++) {
    int code = (int)((b->buf >> (b->cnt - l)) & ((1u << l) - 1u));
    if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) {
      b->cnt -= l;

      return h->val[h->valptr[l] + code - h->mincode[l]];
    }
  }

  b->err = 1;

  return 0;
}

// JPEG's sign extension of an n bit value
static int extend(int v, int n) { return v < p2[n - 1] ? v - p2[n] + 1 : v; }

// Decode

typedef struct {
  hufftab_t dc[4], ac[4];
  uint16_t q[4][64]; // in natural order
  comp_t comp[MAX_COMP];
  int ncomp, w, h, hmax, vmax, mcux, mcuy;
  int restart;
  int adobe_transform; // -1 = no Adobe marker
  int have_sof;
} jpeg_t;

static int decode_block(bits_t *b, jpeg_t *j, comp_t *c, int bx, int by) {
  int coef[64];
  for (int i = 0; i < 64; i++) {
    coef[i] = 0;
  }

  int t = decode_sym(b, &j->dc[c->td]);
  if (b->err || t > 15) {
    return -1;
  }

  int diff = t ? extend(get_bits(b, t), t) : 0;
  c->pred += diff;
  coef[0] = c->pred * j->q[c->tq][0];
  for (int k = 1; k < 64;) {
    int rs = decode_sym(b, &j->ac[c->ta]);
    if (b->err) {
      return -1;
    }

    int r = rs / 16, s = rs % 16;
    if (s == 0) {
      if (r != 15) {
        break; // end of block
      }

      k += 16;

      continue;
    }

    k += r;
    if (k > 63) {
      return -1;
    }

    int v = extend(get_bits(b, s), s);
    coef[zigzag[k]] = v * j->q[c->tq][zigzag[k]];
    k++;
  }

  jpeg_idct(coef,
            c->plane + (size_t)by * 8 * (size_t)c->bw * 8 + (size_t)bx * 8,
            c->bw * 8);

  return 0;
}

static int be16(const uint8_t *p) { return p[0] * 256 + p[1]; }

static int decode_scan(jpeg_t *j, const uint8_t *d, size_t n, size_t *pos,
                       const int *sc, int nsc) {
  bits_t b = {.src = d, .n = n, .pos = *pos};
  int total, mcus_x;
  if (nsc == 1) { // non-interleaved: component's own block grid
    const comp_t *only = &j->comp[sc[0]];
    mcus_x = (j->w * only->h + j->hmax * 8 - 1) / (j->hmax * 8);
    int mcus_y = (j->h * only->v + j->vmax * 8 - 1) / (j->vmax * 8);
    total = mcus_x * mcus_y;
  } else {
    mcus_x = j->mcux;
    total = j->mcux * j->mcuy;
  }

  for (int i = 0; i < nsc; i++) {
    j->comp[sc[i]].pred = 0;
  }

  for (int m = 0; m < total; m++) {
    if (j->restart && m && m % j->restart == 0) {
      // RSTn: drop bits left over, find marker, start predictors over
      b.cnt = 0;
      b.buf = 0;
      b.hit_marker = 0;
      while (b.pos + 1 < n && !(d[b.pos] == 0xff && d[b.pos + 1] >= 0xd0 &&
                                d[b.pos + 1] <= 0xd7)) {
        b.pos++;
      }

      if (b.pos + 1 >= n) {
        return -1;
      }

      b.pos += 2;
      for (int i = 0; i < nsc; i++) {
        j->comp[sc[i]].pred = 0;
      }
    }

    if (nsc == 1) {
      if (decode_block(&b, j, &j->comp[sc[0]], m % mcus_x, m / mcus_x) != 0) {
        return -1;
      }
    } else {
      int mx = m % mcus_x, my = m / mcus_x;
      for (int i = 0; i < nsc; i++) {
        comp_t *c = &j->comp[sc[i]];
        for (int v = 0; v < c->v; v++) {
          for (int h = 0; h < c->h; h++) {
            if (decode_block(&b, j, c, mx * c->h + h, my * c->v + v) != 0) {
              return -1;
            }
          }
        }
      }
    }

    if (b.err) {
      return -1;
    }
  }

  // Next marker search skips entropy coded bytes (stuffed zeros and restart
  // markers are no markers), so where reading stopped will do
  *pos = b.pos;

  return 0;
}

static int floor_div(int a, int d) {
  return a >= 0 ? a / d : -((-a + d - 1) / d);
}

static uint8_t clamp8(int v) {
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// Component sample for output pixel (x, y)
// Subsampled components are interpolated linearly between sample centres, edges
// repeated: same triangle filter libjpeg's "fancy upsampling" uses for common
// 2:1 case
static int sample(const jpeg_t *j, const comp_t *c, int x, int y) {
  if (c->h == j->hmax && c->v == j->vmax) {
    return c->plane[(size_t)y * (size_t)c->bw * 8 + (size_t)x];
  }

  int cw = (j->w * c->h + j->hmax - 1) / j->hmax; // samples picture has
  int ch = (j->h * c->v + j->vmax - 1) / j->vmax;
  int dx = 2 * j->hmax, dy = 2 * j->vmax;
  int nx = (2 * x + 1) * c->h - j->hmax; // position in units of 1/dx samples
  int ny = (2 * y + 1) * c->v - j->vmax;
  int x0 = floor_div(nx, dx), y0 = floor_div(ny, dy);
  int fx = nx - x0 * dx, fy = ny - y0 * dy;
  int x1 = x0 + 1, y1 = y0 + 1;
  x0 = x0 < 0 ? 0 : x0 >= cw ? cw - 1 : x0;
  x1 = x1 < 0 ? 0 : x1 >= cw ? cw - 1 : x1;
  y0 = y0 < 0 ? 0 : y0 >= ch ? ch - 1 : y0;
  y1 = y1 < 0 ? 0 : y1 >= ch ? ch - 1 : y1;
  size_t stride = (size_t)c->bw * 8;
  int a = c->plane[(size_t)y0 * stride + (size_t)x0];
  int b = c->plane[(size_t)y0 * stride + (size_t)x1];
  int cc = c->plane[(size_t)y1 * stride + (size_t)x0];
  int d = c->plane[(size_t)y1 * stride + (size_t)x1];
  int top = a * (dx - fx) + b * fx, bot = cc * (dx - fx) + d * fx;

  return (top * (dy - fy) + bot * fy + dx * dy / 2) / (dx * dy);
}

int jpeg_decode(const uint8_t *d, size_t n, image_t *out, const char **err) {
  static jpeg_t j; // large: not on stack
  nl_memset(&j, 0, sizeof(j));
  j.adobe_transform = -1;
  const char *msg = "damaged JPEG";
  int rc = -1;
  uint8_t *rgb = NULL;

  size_t pos = 2;
  int scans = 0;
  while (pos + 4 <= n) {
    if (d[pos] != 0xff) {
      pos++;

      continue;
    }

    int marker = d[pos + 1];
    if (marker == 0xff || marker == 0 || (marker >= 0xd0 && marker <= 0xd7) ||
        marker == 0x01) {
      pos += marker == 0xff ? 1 : 2;

      continue;
    }

    if (marker == 0xd9) {
      break;
    }

    size_t len = (size_t)be16(d + pos + 2);
    if (len < 2 || pos + 2 + len > n) {
      goto done;
    }

    const uint8_t *seg = d + pos + 4;
    size_t slen = len - 2;
    pos += 2 + len;
    switch (marker) {
    case 0xdb: { // DQT
      size_t o = 0;
      while (o < slen) {
        int pq = seg[o] / 16, tq = seg[o] % 16;
        o++;
        if (tq > 3 || pq > 1 || o + (pq ? 128u : 64u) > slen) {
          goto done;
        }

        for (int i = 0; i < 64; i++) {
          int v = pq ? be16(seg + o + (size_t)i * 2) : seg[o + (size_t)i];
          j.q[tq][zigzag[i]] = (uint16_t)v;
        }

        o += pq ? 128u : 64u;
      }

      break;
    }
    case 0xc4: { // DHT
      size_t o = 0;
      while (o + 17 <= slen) {
        int tc = seg[o] / 16, th = seg[o] % 16;
        if (tc > 1 || th > 3) {
          goto done;
        }

        hufftab_t *h = tc ? &j.ac[th] : &j.dc[th];
        int total = 0;
        h->bits[0] = 0;
        for (int i = 1; i <= 16; i++) {
          h->bits[i] = seg[o + (size_t)i];
          total += h->bits[i];
        }

        o += 17;
        if (total > 256 || o + (size_t)total > slen) {
          goto done;
        }

        nl_memcpy(h->val, seg + o, (size_t)total);
        o += (size_t)total;
        if (build_huff(h) != 0) {
          goto done;
        }
      }

      break;
    }
    case 0xdd:
      if (slen < 2) {
        goto done;
      }

      j.restart = be16(seg);
      break;
    case 0xee: // Adobe: colour transform flag
      if (slen >= 12 && seg[0] == 'A' && seg[1] == 'd' && seg[2] == 'o' &&
          seg[3] == 'b' && seg[4] == 'e') {
        j.adobe_transform = seg[11];
      }

      break;
    case 0xc0:
    case 0xc1: { // SOF baseline / extended sequential
      if (j.have_sof || slen < 6 || seg[0] != 8) {
        msg = seg[0] != 8 ? "JPEG with other than 8 bits per sample is not "
                            "supported"
                          : "damaged JPEG";
        goto done;
      }

      j.h = be16(seg + 1);
      j.w = be16(seg + 3);
      j.ncomp = seg[5];
      if (j.w <= 0 || j.h <= 0 || (j.ncomp != 1 && j.ncomp != 3) ||
          slen < 6 + (size_t)j.ncomp * 3) {
        msg = j.ncomp == 4 ? "CMYK JPEG is not supported" : "damaged JPEG";
        goto done;
      }

      if ((uint64_t)j.w * (uint64_t)j.h > IMAGE_MAX_PIXELS) {
        msg = "picture too large (at most 4096 x 4096 pixels)";
        goto done;
      }

      for (int i = 0; i < j.ncomp; i++) {
        comp_t *c = &j.comp[i];
        c->id = seg[6 + i * 3];
        c->h = seg[7 + i * 3] / 16;
        c->v = seg[7 + i * 3] % 16;
        c->tq = seg[8 + i * 3];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) {
          goto done;
        }

        j.hmax = c->h > j.hmax ? c->h : j.hmax;
        j.vmax = c->v > j.vmax ? c->v : j.vmax;
      }

      j.mcux = (j.w + j.hmax * 8 - 1) / (j.hmax * 8);
      j.mcuy = (j.h + j.vmax * 8 - 1) / (j.vmax * 8);
      for (int i = 0; i < j.ncomp; i++) {
        comp_t *c = &j.comp[i];
        c->bw = j.mcux * c->h;
        c->bh = j.mcuy * c->v;
        c->plane = nl_malloc((size_t)c->bw * 8 * (size_t)c->bh * 8);
        if (!c->plane) {
          msg = "out of memory";
          goto done;
        }
      }

      j.have_sof = 1;
      break;
    }
    case 0xc2:
    case 0xc6:
    case 0xca:
      msg = "progressive JPEG is not supported: re-save it as baseline";
      goto done;
    case 0xc3:
    case 0xc5:
    case 0xc7:
    case 0xc9:
    case 0xcb:
    case 0xcd:
    case 0xce:
    case 0xcf:
      msg = "this kind of JPEG (lossless / arithmetic coded) is not supported";
      goto done;
    case 0xda: { // SOS
      if (!j.have_sof || slen < 1 || seg[0] < 1 || seg[0] > j.ncomp ||
          slen < 1 + (size_t)seg[0] * 2 + 3) {
        goto done;
      }

      int nsc = seg[0], sc[MAX_COMP];
      for (int i = 0; i < nsc; i++) {
        int id = seg[1 + i * 2], found = -1;
        for (int k = 0; k < j.ncomp; k++) {
          if (j.comp[k].id == id) {
            found = k;
          }
        }

        int tbl = seg[2 + i * 2];
        if (found < 0 || tbl / 16 > 3 || tbl % 16 > 3 ||
            !j.dc[tbl / 16].present || !j.ac[tbl % 16].present ||
            !j.q[j.comp[found].tq][0]) {
          goto done;
        }

        sc[i] = found;
        j.comp[found].td = tbl / 16;
        j.comp[found].ta = tbl % 16;
      }

      if (decode_scan(&j, d, n, &pos, sc, nsc) != 0) {
        goto done;
      }

      scans++;
      break;
    }
    default:
      break; // APPn, COM and friends
    }
  }

  if (!j.have_sof || scans == 0) {
    goto done;
  }

  rgb = nl_malloc((size_t)j.w * (size_t)j.h * 3);
  if (!rgb) {
    msg = "out of memory";
    goto done;
  }

  int ycc = j.ncomp == 3 && j.adobe_transform != 0;
  for (int y = 0; y < j.h; y++) {
    uint8_t *o = rgb + (size_t)y * (size_t)j.w * 3;
    for (int x = 0; x < j.w; x++, o += 3) {
      int s[MAX_COMP] = {0};
      for (int i = 0; i < j.ncomp; i++) {
        s[i] = sample(&j, &j.comp[i], x, y);
      }

      if (j.ncomp == 1) {
        o[0] = o[1] = o[2] = (uint8_t)s[0];
      } else if (ycc) {
        int cb = s[1] - 128, cr = s[2] - 128;
        o[0] = clamp8(s[0] + floor_div(91881 * cr + 32768, 65536));
        o[1] =
            clamp8(s[0] + floor_div(-22554 * cb - 46802 * cr + 32768, 65536));
        o[2] = clamp8(s[0] + floor_div(116130 * cb + 32768, 65536));
      } else {
        o[0] = (uint8_t)s[0];
        o[1] = (uint8_t)s[1];
        o[2] = (uint8_t)s[2];
      }
    }
  }

  out->w = j.w;
  out->h = j.h;
  out->rgb = rgb;
  rgb = NULL;
  rc = 0;

done:
  for (int i = 0; i < MAX_COMP; i++) {
    nl_free(j.comp[i].plane);
  }

  nl_free(rgb);
  if (rc != 0 && err) {
    *err = msg;
  }

  return rc;
}
