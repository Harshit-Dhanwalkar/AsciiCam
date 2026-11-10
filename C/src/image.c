/*
 * src/image.c - PNG / PNM decoder and picture scaler
 */

#include "platform.h"

#if !defined(__LINUX_NOLIBC__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include "nolibc.h"

#include "image.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

// n bytes of a equal first n of string b
static int eq_bytes(const void *a, const char *b, size_t n) {
  const uint8_t *x = a;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != (uint8_t)b[i]) {
      return 0;
    }
  }

  return 1;
}

const char *image_device_path(const char *device) {
  const size_t pl = sizeof(IMAGE_DEVICE_PREFIX) - 1;
  if (!device || nl_strlen(device) <= pl ||
      !eq_bytes(device, IMAGE_DEVICE_PREFIX, pl)) {
    return NULL;
  }

  return device + pl;
}

void image_free(image_t *img) {
  nl_free(img->rgb);
  img->rgb = NULL;
  img->w = img->h = 0;
}

// Inflate
// RFC 1951 way zlib's puff.c does it: canonical codes decoded bit by bit,
// small and obviously correct, fast enough for a picture loaded once

typedef struct {
  const uint8_t *src;
  size_t n, pos;
  uint32_t bitbuf;
  unsigned bitcnt;
  uint8_t *dst;
  size_t cap, len;
  int err;
} infl_t;

typedef struct {
  uint16_t count[16];   // codes of each length
  uint16_t symbol[288]; // symbols in code order
} huff_t;

static unsigned getbits(infl_t *s, unsigned need) {
  uint32_t v = s->bitbuf;
  while (s->bitcnt < need) {
    if (s->pos >= s->n) {
      s->err = 1;

      return 0;
    }

    v |= (uint32_t)s->src[s->pos++] << s->bitcnt;
    s->bitcnt += 8;
  }

  s->bitbuf = v >> need;
  s->bitcnt -= need;

  return (unsigned)(v & ((1u << need) - 1u));
}

// Build decoding tables from code lengths
// Return 0 on success (complete or incomplete code), negative when lengths
// over-subscribe code space
static int construct(huff_t *h, const uint8_t *length, unsigned n) {
  for (unsigned l = 0; l < 16; l++) {
    h->count[l] = 0;
  }

  for (unsigned i = 0; i < n; i++) {
    h->count[length[i]]++;
  }

  if (h->count[0] == n) {
    return 0;
  }

  int left = 1;
  for (unsigned l = 1; l < 16; l++) {
    left *= 2;
    left -= h->count[l];
    if (left < 0) {
      return left;
    }
  }

  uint16_t offs[16];
  offs[1] = 0;
  for (unsigned l = 1; l < 15; l++) {
    offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
  }

  for (unsigned i = 0; i < n; i++) {
    if (length[i] != 0) {
      h->symbol[offs[length[i]]++] = (uint16_t)i;
    }
  }

  return 0;
}

static int decode(infl_t *s, const huff_t *h) {
  int code = 0, first = 0, index = 0;
  for (unsigned l = 1; l < 16; l++) {
    code += (int)getbits(s, 1);
    if (s->err) {
      return -1;
    }

    int count = h->count[l];
    if (code - count < first) {
      return h->symbol[index + (code - first)];
    }

    index += count;
    first += count;
    first *= 2;
    code *= 2;
  }

  return -1;
}

static const uint16_t len_base[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                      1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                      4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                       4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                       9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(infl_t *s, const huff_t *lencode, const huff_t *distcode) {
  for (;;) {
    int sym = decode(s, lencode);
    if (sym < 0) {
      return -1;
    }

    if (sym < 256) {
      if (s->len >= s->cap) {
        return -1;
      }

      s->dst[s->len++] = (uint8_t)sym;

      continue;
    }

    if (sym == 256) {
      return 0;
    }

    sym -= 257;
    if (sym >= 29) {
      return -1;
    }

    size_t len = len_base[sym] + getbits(s, len_extra[sym]);
    int ds = decode(s, distcode);
    if (ds < 0 || ds >= 30) {
      return -1;
    }

    size_t dist = dist_base[ds] + getbits(s, dist_extra[ds]);
    if (s->err || dist > s->len || len > s->cap - s->len) {
      return -1;
    }

    for (size_t i = 0; i < len; i++) { // may overlap: byte by byte
      s->dst[s->len + i] = s->dst[s->len + i - dist];
    }

    s->len += len;
  }
}

static int stored_block(infl_t *s) {
  s->bitbuf = 0;
  s->bitcnt = 0;
  if (s->pos + 4 > s->n) {
    return -1;
  }

  size_t len = (size_t)s->src[s->pos] | ((size_t)s->src[s->pos + 1] * 256u);
  size_t nlen =
      (size_t)s->src[s->pos + 2] | ((size_t)s->src[s->pos + 3] * 256u);
  s->pos += 4;
  if ((len ^ 0xffffu) != nlen || s->pos + len > s->n || len > s->cap - s->len) {
    return -1;
  }

  nl_memcpy(s->dst + s->len, s->src + s->pos, len);
  s->pos += len;
  s->len += len;

  return 0;
}

static int fixed_block(infl_t *s) {
  uint8_t lengths[288];
  unsigned i = 0;
  for (; i < 144; i++) {
    lengths[i] = 8;
  }

  for (; i < 256; i++) {
    lengths[i] = 9;
  }

  for (; i < 280; i++) {
    lengths[i] = 7;
  }

  for (; i < 288; i++) {
    lengths[i] = 8;
  }

  huff_t lencode, distcode;
  construct(&lencode, lengths, 288);
  for (i = 0; i < 30; i++) {
    lengths[i] = 5;
  }

  construct(&distcode, lengths, 30);

  return codes(s, &lencode, &distcode);
}

static int dynamic_block(infl_t *s) {
  static const uint8_t order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                    11, 4,  12, 3, 13, 2, 14, 1, 15};
  uint8_t lengths[320];
  unsigned nlen = getbits(s, 5) + 257, ndist = getbits(s, 5) + 1,
           ncode = getbits(s, 4) + 4;
  if (s->err || nlen > 286 || ndist > 30) {
    return -1;
  }

  unsigned i = 0;
  for (; i < ncode; i++) {
    lengths[order[i]] = (uint8_t)getbits(s, 3);
  }

  for (; i < 19; i++) {
    lengths[order[i]] = 0;
  }

  huff_t lencode, distcode;
  if (s->err || construct(&lencode, lengths, 19) != 0) {
    return -1;
  }

  unsigned idx = 0;
  while (idx < nlen + ndist) {
    int sym = decode(s, &lencode);
    if (sym < 0) {
      return -1;
    }

    if (sym < 16) {
      lengths[idx++] = (uint8_t)sym;

      continue;
    }

    uint8_t prev = 0;
    unsigned rep;
    if (sym == 16) {
      if (idx == 0) {
        return -1;
      }

      prev = lengths[idx - 1];
      rep = 3 + getbits(s, 2);
    } else if (sym == 17) {
      rep = 3 + getbits(s, 3);
    } else {
      rep = 11 + getbits(s, 7);
    }

    if (s->err || idx + rep > nlen + ndist) {
      return -1;
    }

    while (rep--) {
      lengths[idx++] = prev;
    }
  }

  if (lengths[256] == 0) {
    return -1; // no end-of-block code
  }

  int err = construct(&lencode, lengths, nlen);
  if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) {
    return -1;
  }

  err = construct(&distcode, lengths + nlen, ndist);
  if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) {
    return -1;
  }

  return codes(s, &lencode, &distcode);
}

long image_inflate(const uint8_t *src, size_t n, uint8_t *out, size_t cap) {
  if (n < 2 || (src[0] & 0x0fu) != 8u ||
      ((src[0] * 256u + src[1]) % 31u) != 0 || (src[1] & 0x20u)) {
    return -1; // not zlib / deflate, or wants a preset dictionary
  }

  infl_t s = {.src = src, .n = n, .pos = 2, .dst = out, .cap = cap};
  unsigned last;
  do {
    last = getbits(&s, 1);
    unsigned type = getbits(&s, 2);
    if (s.err) {
      return -1;
    }

    int r = type == 0   ? stored_block(&s)
            : type == 1 ? fixed_block(&s)
            : type == 2 ? dynamic_block(&s)
                        : -1;
    if (r != 0 || s.err) {
      return -1;
    }
  } while (!last);

  return (long)s.len;
}

// PNG

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] * 16777216u) | ((uint32_t)p[1] * 65536u) |
         ((uint32_t)p[2] * 256u) | (uint32_t)p[3];
}

static int paeth(int a, int b, int c) {
  int p = a + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;

  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// Undo per-row filters in place. raw: h rows of 1 filter byte + stride
static int unfilter(uint8_t *raw, int h, size_t stride, size_t bpp) {
  for (int y = 0; y < h; y++) {
    uint8_t *row = raw + (size_t)y * (stride + 1);
    uint8_t ft = row[0];
    uint8_t *cur = row + 1;
    const uint8_t *up = y ? cur - (stride + 1) : NULL;
    for (size_t i = 0; i < stride; i++) {
      int a = i >= bpp ? cur[i - bpp] : 0;
      int b = up ? up[i] : 0;
      int c = (up && i >= bpp) ? up[i - bpp] : 0;
      int add;
      switch (ft) {
      case 0:
        add = 0;
        break;
      case 1:
        add = a;
        break;
      case 2:
        add = b;
        break;
      case 3:
        add = (a + b) / 2;
        break;
      case 4:
        add = paeth(a, b, c);
        break;
      default:
        return -1;
      }

      cur[i] = (uint8_t)(cur[i] + add);
    }
  }

  return 0;
}

// Sample number x (0-based) of a row with `depth` bits per sample, as stored
static unsigned sample(const uint8_t *row, size_t x, unsigned depth) {
  if (depth == 8) {
    return row[x];
  }

  if (depth == 16) {
    return (unsigned)row[x * 2] * 256u + row[x * 2 + 1];
  }

  size_t bit = x * depth;
  unsigned shift = 8u - depth - (unsigned)(bit % 8u);

  return ((unsigned)row[bit / 8] >> shift) & ((1u << depth) - 1u);
}

// 8 bit value of a sample that goes up to maxv
static uint8_t scale8(unsigned v, unsigned depth) {
  if (depth == 16) {
    return (uint8_t)(v / 256u);
  }

  return (uint8_t)(depth == 8 ? v : v * 255u / ((1u << depth) - 1u));
}

static int decode_png(const uint8_t *d, size_t n, image_t *out,
                      const char **err) {
  uint32_t w = 0, h = 0;
  unsigned depth = 0, ctype = 0;
  int have_ihdr = 0;
  uint8_t pal[256 * 3];
  unsigned npal = 0;
  uint8_t *idat = NULL;
  size_t idat_len = 0, idat_cap = 0;
  const char *msg = "damaged PNG";
  int ok = 0;

  size_t pos = 8;
  while (pos + 12 <= n) {
    uint32_t clen = be32(d + pos);
    const uint8_t *type = d + pos + 4;
    const uint8_t *body = d + pos + 8;
    if (clen > n - pos - 12) {
      goto done;
    }

    if (eq_bytes(type, "IHDR", 4)) {
      if (clen != 13) {
        goto done;
      }

      w = be32(body);
      h = be32(body + 4);
      depth = body[8];
      ctype = body[9];
      if (body[10] != 0 || body[11] != 0) {
        goto done;
      }

      if (body[12] != 0) {
        msg = "interlaced PNG is not supported, save it without interlacing";
        goto done;
      }

      have_ihdr = 1;
    } else if (eq_bytes(type, "PLTE", 4)) {
      if (clen % 3 != 0 || clen > 768) {
        goto done;
      }

      npal = clen / 3;
      nl_memcpy(pal, body, clen);
    } else if (eq_bytes(type, "IDAT", 4)) {
      if (idat_len + clen > idat_cap) {
        size_t nc = idat_cap ? idat_cap : 65536;
        while (nc < idat_len + clen) {
          nc *= 2;
        }

        uint8_t *nb = nl_malloc(nc);
        if (!nb) {
          msg = "out of memory";
          goto done;
        }

        if (idat_len) {
          nl_memcpy(nb, idat, idat_len);
        }

        nl_free(idat);
        idat = nb;
        idat_cap = nc;
      }

      nl_memcpy(idat + idat_len, body, clen);
      idat_len += clen;
    } else if (eq_bytes(type, "IEND", 4)) {
      break;
    }

    pos += 12 + (size_t)clen;
  }

  if (!have_ihdr || !idat_len) {
    goto done;
  }

  unsigned channels = ctype == 0   ? 1
                      : ctype == 2 ? 3
                      : ctype == 3 ? 1
                      : ctype == 4 ? 2
                      : ctype == 6 ? 4
                                   : 0;
  // Bit depths PNG standard allows for each colour type
  int depth_ok = ctype == 0 ? (depth == 1 || depth == 2 || depth == 4 ||
                               depth == 8 || depth == 16)
                 : ctype == 3
                     ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                     : (depth == 8 || depth == 16);
  if (!channels || !depth_ok || w == 0 || h == 0) {
    goto done;
  }

  if (w > 0x7fffffu || h > 0x7fffffu || (uint64_t)w * h > IMAGE_MAX_PIXELS) {
    msg = "picture too large (at most 4096 x 4096 pixels)";
    goto done;
  }

  size_t stride = ((size_t)w * channels * depth + 7) / 8;
  size_t raw_len = (stride + 1) * h;
  uint8_t *raw = nl_malloc(raw_len);
  if (!raw) {
    msg = "out of memory";
    goto done;
  }

  long got = image_inflate(idat, idat_len, raw, raw_len);
  size_t bpp = (channels * depth + 7) / 8;
  if (got != (long)raw_len || unfilter(raw, (int)h, stride, bpp) != 0) {
    nl_free(raw);
    goto done;
  }

  uint8_t *rgb = nl_malloc((size_t)w * h * 3);
  if (!rgb) {
    nl_free(raw);
    msg = "out of memory";
    goto done;
  }

  for (uint32_t y = 0; y < h; y++) {
    const uint8_t *row = raw + (size_t)y * (stride + 1) + 1;
    uint8_t *o = rgb + (size_t)y * w * 3;
    for (uint32_t x = 0; x < w; x++, o += 3) {
      unsigned c[4] = {0, 0, 0, 255};
      if (ctype == 3) {
        unsigned idx = sample(row, x, depth);
        if (idx >= npal) {
          nl_free(raw);
          nl_free(rgb);
          msg = "PNG colour index outside its palette";
          goto done;
        }

        o[0] = pal[idx * 3];
        o[1] = pal[idx * 3 + 1];
        o[2] = pal[idx * 3 + 2];

        continue;
      }

      for (unsigned k = 0; k < channels; k++) {
        c[k] = scale8(sample(row, (size_t)x * channels + k, depth), depth);
      }

      unsigned r, g, b, a = 255;
      if (channels <= 2) {
        r = g = b = c[0];
        a = channels == 2 ? c[1] : 255;
      } else {
        r = c[0];
        g = c[1];
        b = c[2];
        a = channels == 4 ? c[3] : 255;
      }

      // Over black
      o[0] = (uint8_t)((r * a + 127) / 255);
      o[1] = (uint8_t)((g * a + 127) / 255);
      o[2] = (uint8_t)((b * a + 127) / 255);
    }
  }

  nl_free(raw);
  out->w = (int)w;
  out->h = (int)h;
  out->rgb = rgb;
  ok = 1;

done:
  nl_free(idat);
  if (!ok && err) {
    *err = msg;
  }

  return ok ? 0 : -1;
}

// PNM

static int pnm_int(const uint8_t *d, size_t n, size_t *pos, unsigned *v) {
  while (*pos < n) {
    if (d[*pos] == '#') {
      while (*pos < n && d[*pos] != '\n') {
        (*pos)++;
      }
    } else if (d[*pos] == ' ' || d[*pos] == '\t' || d[*pos] == '\n' ||
               d[*pos] == '\r') {
      (*pos)++;
    } else {
      break;
    }
  }

  if (*pos >= n || d[*pos] < '0' || d[*pos] > '9') {
    return -1;
  }

  unsigned r = 0;
  while (*pos < n && d[*pos] >= '0' && d[*pos] <= '9') {
    r = r * 10 + (unsigned)(d[*pos] - '0');
    if (r > 1000000u) {
      return -1;
    }

    (*pos)++;
  }

  *v = r;

  return 0;
}

static int decode_pnm(const uint8_t *d, size_t n, image_t *out,
                      const char **err) {
  const char *msg = "damaged PGM/PPM";
  int channels = d[1] == '5' ? 1 : 3;
  size_t pos = 2;
  unsigned w, h, maxv;
  if (pnm_int(d, n, &pos, &w) || pnm_int(d, n, &pos, &h) ||
      pnm_int(d, n, &pos, &maxv) || w == 0 || h == 0 || maxv == 0 ||
      maxv > 65535u || pos >= n) {
    goto fail;
  }

  pos++; // Single whitespace after maxval
  if ((uint64_t)w * h > IMAGE_MAX_PIXELS) {
    msg = "picture too large (at most 4096 x 4096 pixels)";
    goto fail;
  }

  size_t bytes = maxv > 255u ? 2 : 1;
  if ((n - pos) / bytes / (size_t)channels < (size_t)w * h) {
    goto fail;
  }

  uint8_t *rgb = nl_malloc((size_t)w * h * 3);
  if (!rgb) {
    msg = "out of memory";
    goto fail;
  }

  for (size_t i = 0; i < (size_t)w * h; i++) {
    for (int k = 0; k < 3; k++) {
      size_t at =
          pos +
          (i * (size_t)channels + (size_t)(channels == 1 ? 0 : k)) * bytes;
      unsigned v = bytes == 2 ? (unsigned)d[at] * 256u + d[at + 1] : d[at];
      v = v > maxv ? maxv : v;
      rgb[i * 3 + (size_t)k] = (uint8_t)((v * 255u + maxv / 2) / maxv);
    }
  }

  out->w = (int)w;
  out->h = (int)h;
  out->rgb = rgb;

  return 0;

fail:
  if (err) {
    *err = msg;
  }

  return -1;
}

int image_decode(const uint8_t *data, size_t n, image_t *out,
                 const char **err) {
  out->w = out->h = 0;
  out->rgb = NULL;
  if (n >= 8 && eq_bytes(data, "\x89PNG\r\n\x1a\n", 8)) {
    return decode_png(data, n, out, err);
  }

  if (n >= 3 && data[0] == 'P' && (data[1] == '5' || data[1] == '6') &&
      (data[2] == ' ' || data[2] == '\n' || data[2] == '\r' ||
       data[2] == '\t' || data[2] == '#')) {
    return decode_pnm(data, n, out, err);
  }

  if (err) {
    *err = n >= 3 && data[0] == 0xff && data[1] == 0xd8
               ? "JPEG is not supported: use a PNG or PGM/PPM"
               : "not a PNG or PGM/PPM picture";
  }

  return -1;
}

#define IMAGE_MAX_FILE (64u * 1024u * 1024u)

int image_load(const char *path, image_t *out, const char **err) {
  out->w = out->h = 0;
  out->rgb = NULL;
  int fd = open(path, O_RDONLY + O_BINARY, 0);
  if (fd < 0) {
    if (err) {
      *err = "cannot open the file";
    }

    return -1;
  }

  size_t cap = 1048576u, len = 0;
  uint8_t *buf = nl_malloc(cap);
  const char *msg = "cannot read the file";
  int rc = -1;
  while (buf) {
    if (len == cap) {
      if (cap >= IMAGE_MAX_FILE) {
        msg = "file too large";
        break;
      }

      uint8_t *nb = nl_malloc(cap * 2);
      if (!nb) {
        break;
      }

      nl_memcpy(nb, buf, len);
      nl_free(buf);
      buf = nb;
      cap *= 2;
    }

    ssize_t r = read(fd, buf + len, cap - len);
    if (r < 0) {
      break;
    }

    if (r == 0) {
      rc = image_decode(buf, len, out, err);
      msg = NULL;
      break;
    }

    len += (size_t)r;
  }

  close(fd);
  nl_free(buf);
  if (rc != 0 && msg && err) {
    *err = msg;
  }

  return rc;
}

// Scaling

static uint8_t luma(int r, int g, int b) {
  return (uint8_t)(16 + (66 * r + 129 * g + 25 * b + 128) / 256);
}

static uint8_t chroma_u(int r, int g, int b) {
  // Numerators stay positive so division floors
  return (uint8_t)((32768 + 128 - 38 * r - 74 * g + 112 * b) / 256);
}

static uint8_t chroma_v(int r, int g, int b) {
  return (uint8_t)((32768 + 128 + 112 * r - 94 * g - 18 * b) / 256);
}

// Average colour of source rectangle behind destination pixel (x, y) of a
// picture scaled to dw x dh. Upscaling degenerates to nearest neighbour
static void area_avg(const image_t *img, int dw, int dh, int x, int y, int *r,
                     int *g, int *b) {
  int x0 = (int)((int64_t)x * img->w / dw);
  int x1 = (int)((int64_t)(x + 1) * img->w / dw);
  int y0 = (int)((int64_t)y * img->h / dh);
  int y1 = (int)((int64_t)(y + 1) * img->h / dh);
  x1 = x1 <= x0 ? x0 + 1 : x1;
  y1 = y1 <= y0 ? y0 + 1 : y1;
  x1 = x1 > img->w ? img->w : x1;
  y1 = y1 > img->h ? img->h : y1;
  uint64_t sr = 0, sg = 0, sb = 0;
  for (int yy = y0; yy < y1; yy++) {
    const uint8_t *p =
        img->rgb + ((size_t)yy * (size_t)img->w + (size_t)x0) * 3;
    for (int xx = x0; xx < x1; xx++, p += 3) {
      sr += p[0];
      sg += p[1];
      sb += p[2];
    }
  }

  uint64_t cnt = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
  *r = (int)((sr + cnt / 2) / cnt);
  *g = (int)((sg + cnt / 2) / cnt);
  *b = (int)((sb + cnt / 2) / cnt);
}

void image_fill_yuyv(const image_t *img, uint8_t *yuyv, int w, int h,
                     int stride) {
  // Fit inside w x h, aspect kept, centred
  int dw = w;
  int dh = h;
  if (img && img->rgb && img->w > 0 && img->h > 0) {
    if ((int64_t)img->w * h > (int64_t)img->h * w) {
      dh = (int)((int64_t)img->h * w / img->w);
    } else {
      dw = (int)((int64_t)img->w * h / img->h);
    }
  } else {
    dw = dh = 0;
  }

  if (dw > 0) {
    dw = dw < 2 ? 2 : dw;
    dh = dh < 1 ? 1 : dh;
  }

  int ox = (w - dw) / 2, oy = (h - dh) / 2;
  ox -= ox % 2; // keep chroma pairs aligned
  for (int y = 0; y < h; y++) {
    uint8_t *row = yuyv + (size_t)y * (size_t)stride;
    for (int x = 0; x < w; x += 2) {
      int r[2] = {0, 0}, g[2] = {0, 0}, b[2] = {0, 0};
      for (int k = 0; k < 2; k++) {
        int sx = x + k - ox, sy = y - oy;
        if (dw > 0 && sx >= 0 && sx < dw && sy >= 0 && sy < dh) {
          area_avg(img, dw, dh, sx, sy, &r[k], &g[k], &b[k]);
        }
      }

      row[x * 2 + 0] = luma(r[0], g[0], b[0]);
      row[x * 2 + 1] =
          chroma_u((r[0] + r[1]) / 2, (g[0] + g[1]) / 2, (b[0] + b[1]) / 2);
      row[x * 2 + 2] = luma(r[1], g[1], b[1]);
      row[x * 2 + 3] =
          chroma_v((r[0] + r[1]) / 2, (g[0] + g[1]) / 2, (b[0] + b[1]) / 2);
    }
  }
}
