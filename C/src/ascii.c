// NOTE:
// SIMD paths:
// -   x86_64 Linux/macOS:  SSE2 via <immintrin.h>
// -   ARM64  macOS:        NEON via <arm_neon.h>
// - TODO: Windows

#include "nolibc.h"

// Emits assembly kernel of nl_sumspan_u8 into this unit (x86-64 Linux)
#define NL_SUMSPAN_IMPL
#include "nl_sgr.h"
#include "nl_sumspan.h"

#include "ascii.h"
#include "platform.h"

// Helpers
static inline uint8_t clamp_u8(int v) {
  return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}

static inline int my_abs(int x) { return x < 0 ? -x : x; }

// Quantize a 24-bit rgb color to standard xterm 256-color palette index
// (16-231 = 6x6x6 color cube, 232-255 = 24-step grayscale ramp), for terminals
// that support ANSI-256 (\033[38;5;<idx>m) but not truecolor
// NOTE: Near-gray inputs go through finer 24-step grayscale ramp instead of
// coarser 6-level cube, since that's where banding is most visible
static inline int rgb_to_ansi256(int r, int g, int b) {
  int max_c = r > g ? (r > b ? r : b) : (g > b ? g : b);
  int min_c = r < g ? (r < b ? r : b) : (g < b ? g : b);
  if (max_c - min_c < 10) {
    int gray = (r + g + b) / 3;
    if (gray < 8) {
      return 16; // black corner of color cube
    }
    if (gray > 248) {
      return 231; // white corner of color cube
    }

    int idx = (gray - 8) * 24 / 247;
    if (idx > 23) {
      idx = 23;
    }

    return 232 + idx;
  }

  int r6 = r * 6 / 256;
  int g6 = g * 6 / 256;
  int b6 = b * 6 / 256;
  if (r6 > 5) {
    r6 = 5;
  }
  if (g6 > 5) {
    g6 = 5;
  }
  if (b6 > 5) {
    b6 = 5;
  }

  return 16 + 36 * r6 + 6 * g6 + b6;
}

static inline double my_sqrt(double x) {
  if (x <= 0.0) {
    return 0.0;
  }

  double guess = x;
  // initial halve exponent by repeated division
  while (guess > 1.0) {
    guess /= 2.0;
  }
  if (guess <= 0.0) {
    guess = 1.0;
  }
  for (int i = 0; i < 12; i++) {
    guess = 0.5 * (guess + x / guess);
  }

  return guess;
}

// YUYV to grayscale
#if defined(ARCH_X86_64)

#ifdef __LINUX_NOLIBC__

#define _MM_MALLOC_H_INCLUDED

#endif

#include <immintrin.h>

void yuyv_to_gray_simd(const uint8_t *yuyv, uint8_t *gray, int width,
                       int height) {
  int total = width * height;
  __m128i mask = _mm_set1_epi16(0x00FF);

  int i = 0;
  for (; i + 16 <= total; i += 16) {
    __m128i lo = _mm_loadu_si128((__m128i *)(yuyv + i * 2));
    __m128i hi = _mm_loadu_si128((__m128i *)(yuyv + i * 2 + 16));
    lo = _mm_and_si128(lo, mask);
    hi = _mm_and_si128(hi, mask);
    _mm_storeu_si128((__m128i *)(gray + i), _mm_packus_epi16(lo, hi));
  }

  for (; i < total; i++) {
    gray[i] = yuyv[i * 2];
  }
}

#elif defined(ARCH_ARM64)

#include <arm_neon.h>

void yuyv_to_gray_simd(const uint8_t *yuyv, uint8_t *gray, int width,
                       int height) {
  int total = width * height;
  // NEON: process 8 YUYV pairs (= 16 px) per iteration
  int i = 0;
  for (; i + 16 <= total; i += 16) {
    // Load 32 bytes: [Y0 U0 Y1 V0 Y2 U1 Y3 V1 ...]
    uint8x16x2_t yuv = vld2q_u8(yuyv + i * 2);
    // yuv.val[0] = all Y bytes (even bytes = luma)
    vst1q_u8(gray + i, yuv.val[0]);
  }

  for (; i < total; i++) {
    gray[i] = yuyv[i * 2];
  }
}

#else

// Fallback
void yuyv_to_gray_simd(const uint8_t *yuyv, uint8_t *gray, int width,
                       int height) {
  int total = width * height;
  for (int i = 0; i < total; i++) {
    gray[i] = yuyv[i * 2];
  }
}
#endif

#if defined(ARCH_X86_64) && defined(__SSE4_1__)

// YUV -> RGB for 8 pixels at once, bit for bit what scalar loops compute:
// 298 * c + 409 * e, 298 * c - 100 * d - 208 * e and 298 * c + 516 * d (c = Y
// - 16, d = U - 128, e = V - 128) are exact 32-bit sums of 16-bit products
// (pmaddwd), then + 128, >> 8 and two saturating packs are clamp to 0..255
// y8, u8, v8 hold 8 bytes each in their low half, chroma already repeated per
// pixel
// Writes 24 bytes
static inline void yuv8_to_rgb24(__m128i y8, __m128i u8, __m128i v8,
                                 uint8_t *out) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i c = _mm_sub_epi16(_mm_cvtepu8_epi16(y8), _mm_set1_epi16(16));
  const __m128i d = _mm_sub_epi16(_mm_cvtepu8_epi16(u8), _mm_set1_epi16(128));
  const __m128i e = _mm_sub_epi16(_mm_cvtepu8_epi16(v8), _mm_set1_epi16(128));

  // madd multiplies 16-bit lanes pairwise and adds each pair into 32 bits:
  // interleave (c, e) / (c, d) so one madd is 298 * c + k * (e or d)
  const __m128i k_r = _mm_set1_epi32((409 << 16) | 298);
  const __m128i k_b = _mm_set1_epi32((516 << 16) | 298);
  const __m128i k_g =
      _mm_set1_epi32((int)((0xFFFFu & (unsigned)-100) << 16) | 298);
  const __m128i k_ge = _mm_set1_epi32((int)(0xFFFFu & (unsigned)-208));
  const __m128i round = _mm_set1_epi32(128);

  const __m128i ce_lo = _mm_unpacklo_epi16(c, e);
  const __m128i ce_hi = _mm_unpackhi_epi16(c, e);
  const __m128i cd_lo = _mm_unpacklo_epi16(c, d);
  const __m128i cd_hi = _mm_unpackhi_epi16(c, d);
  const __m128i e0_lo = _mm_unpacklo_epi16(e, zero);
  const __m128i e0_hi = _mm_unpackhi_epi16(e, zero);

  __m128i r_lo =
      _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(ce_lo, k_r), round), 8);
  __m128i r_hi =
      _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(ce_hi, k_r), round), 8);
  __m128i b_lo =
      _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(cd_lo, k_b), round), 8);
  __m128i b_hi =
      _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(cd_hi, k_b), round), 8);
  __m128i g_lo =
      _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(cd_lo, k_g),
                                                 _mm_madd_epi16(e0_lo, k_ge)),
                                   round),
                     8);
  __m128i g_hi =
      _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(cd_hi, k_g),
                                                 _mm_madd_epi16(e0_hi, k_ge)),
                                   round),
                     8);

  __m128i r16 = _mm_packs_epi32(r_lo, r_hi);
  __m128i g16 = _mm_packs_epi32(g_lo, g_hi);
  __m128i b16 = _mm_packs_epi32(b_lo, b_hi);
  __m128i r8 = _mm_packus_epi16(r16, r16);
  __m128i g8 = _mm_packus_epi16(g16, g16);
  __m128i b8 = _mm_packus_epi16(b16, b16);

  // R0 G0 B0 R1 G1 B1 ... : 24 bytes from three 8-byte planes
  const __m128i rg = _mm_unpacklo_epi8(r8, g8);
  const __m128i rg_a =
      _mm_setr_epi8(0, 1, -1, 2, 3, -1, 4, 5, -1, 6, 7, -1, 8, 9, -1, 10);
  const __m128i b_a =
      _mm_setr_epi8(-1, -1, 0, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, -1, 4, -1);
  const __m128i rg_b = _mm_setr_epi8(11, -1, 12, 13, -1, 14, 15, -1, -1, -1, -1,
                                     -1, -1, -1, -1, -1);
  const __m128i b_b = _mm_setr_epi8(-1, 5, -1, -1, 6, -1, -1, 7, -1, -1, -1, -1,
                                    -1, -1, -1, -1);
  _mm_storeu_si128((__m128i *)out, _mm_or_si128(_mm_shuffle_epi8(rg, rg_a),
                                                _mm_shuffle_epi8(b8, b_a)));
  _mm_storel_epi64(
      (__m128i *)(out + 16),
      _mm_or_si128(_mm_shuffle_epi8(rg, rg_b), _mm_shuffle_epi8(b8, b_b)));
}

// First pairs of a row, 4 pairs (8 pixels) at a time
// Returns pairs done; caller finishes rest
// lrow may be NULL (Y from YUYV itself)
static int yuyv_row_to_rgb_simd(const uint8_t *row, const uint8_t *lrow,
                                uint8_t *out, int pairs) {
  const __m128i y_pick =
      _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1);
  const __m128i u_pick =
      _mm_setr_epi8(1, 1, 5, 5, 9, 9, 13, 13, -1, -1, -1, -1, -1, -1, -1, -1);
  const __m128i v_pick =
      _mm_setr_epi8(3, 3, 7, 7, 11, 11, 15, 15, -1, -1, -1, -1, -1, -1, -1, -1);
  int i = 0;
  for (; i + 4 <= pairs; i += 4) {
    __m128i raw = _mm_loadu_si128((const __m128i *)(row + i * 4));
    __m128i y8 = lrow ? _mm_loadl_epi64((const __m128i *)(lrow + i * 2))
                      : _mm_shuffle_epi8(raw, y_pick);
    yuv8_to_rgb24(y8, _mm_shuffle_epi8(raw, u_pick),
                  _mm_shuffle_epi8(raw, v_pick), out + i * 6);
  }

  return i;
}

// Mirrored row (luma already flipped): pair k of output takes its chroma from
// pair pairs-1-k of camera row
// Even widths only: then output pixels 2k and 2k+1 share one camera pair
// Returns pixels done (multiple of 8)
static int yuyv_row_to_rgb_mirror_simd(const uint8_t *row, const uint8_t *lrow,
                                       uint8_t *out, int pairs) {
  const __m128i u_pick =
      _mm_setr_epi8(13, 13, 9, 9, 5, 5, 1, 1, -1, -1, -1, -1, -1, -1, -1, -1);
  const __m128i v_pick =
      _mm_setr_epi8(15, 15, 11, 11, 7, 7, 3, 3, -1, -1, -1, -1, -1, -1, -1, -1);
  int k = 0;
  for (; k + 4 <= pairs; k += 4) {
    // Output pairs k..k+3 <- camera pairs pairs-1-k .. pairs-4-k, in that order
    __m128i raw = _mm_loadu_si128((const __m128i *)(row + (pairs - 4 - k) * 4));
    __m128i y8 = _mm_loadl_epi64((const __m128i *)(lrow + k * 2));
    yuv8_to_rgb24(y8, _mm_shuffle_epi8(raw, u_pick),
                  _mm_shuffle_epi8(raw, v_pick), out + k * 6);
  }

  return k * 2;
}

#else

static int yuyv_row_to_rgb_simd(const uint8_t *row, const uint8_t *lrow,
                                uint8_t *out, int pairs) {
  (void)row, (void)lrow, (void)out, (void)pairs;

  return 0;
}

static int yuyv_row_to_rgb_mirror_simd(const uint8_t *row, const uint8_t *lrow,
                                       uint8_t *out, int pairs) {
  (void)row, (void)lrow, (void)out, (void)pairs;

  return 0;
}

#endif

void yuyv_to_rgb_ex(const uint8_t *yuyv, int stride, const uint8_t *luma,
                    uint8_t *rgb, int width, int height) {
  if (stride < width * 2) {
    stride = width * 2;
  }

  int pairs = width / 2;
  for (int y = 0; y < height; y++) {
    const uint8_t *row = yuyv + (size_t)y * (size_t)stride;
    const uint8_t *lrow = luma ? luma + (size_t)y * (size_t)width : NULL;
    uint8_t *out = rgb + (size_t)y * (size_t)width * 3;

    int done = yuyv_row_to_rgb_simd(row, lrow, out, pairs);
    for (int i = done; i < pairs; i++) {
      int y0 = lrow ? lrow[i * 2] : row[i * 4 + 0];
      int u = row[i * 4 + 1];
      int y1 = lrow ? lrow[i * 2 + 1] : row[i * 4 + 2];
      int v = row[i * 4 + 3];
      int d = u - 128;
      int e = v - 128;
      for (int p = 0; p < 2; p++) {
        int c = ((p == 0) ? y0 : y1) - 16;
        uint8_t *px = out + (i * 2 + p) * 3;

        px[0] = clamp_u8((298 * c + 409 * e + 128) >> 8);
        px[1] = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
        px[2] = clamp_u8((298 * c + 516 * d + 128) >> 8);
      }
    }
  }
}

void yuyv_to_rgb_mirror(const uint8_t *yuyv, int stride, const uint8_t *luma,
                        uint8_t *rgb, int width, int height) {
  if (stride < width * 2) {
    stride = width * 2;
  }

  int pairs = width / 2;
  int last = pairs * 2 - 1; // Last column that has chroma
  for (int y = 0; y < height; y++) {
    const uint8_t *row = yuyv + (size_t)y * (size_t)stride;
    const uint8_t *lrow = luma + (size_t)y * (size_t)width;
    uint8_t *out = rgb + (size_t)y * (size_t)width * 3;

    int first = width == pairs * 2
                    ? yuyv_row_to_rgb_mirror_simd(row, lrow, out, pairs)
                    : 0;
    for (int x = first; x < pairs * 2; x++) {
      int sx = width - 1 - x; // Camera column this pixel was flipped from
      if (sx > last) {
        sx = last;
      }

      int u = row[(sx / 2) * 4 + 1];
      int v = row[(sx / 2) * 4 + 3];
      int d = u - 128;
      int e = v - 128;
      int c = lrow[x] - 16;
      uint8_t *px = out + x * 3;

      px[0] = clamp_u8((298 * c + 409 * e + 128) / 256);
      px[1] = clamp_u8((298 * c - 100 * d - 208 * e + 128) / 256);
      px[2] = clamp_u8((298 * c + 516 * d + 128) / 256);
    }
  }
}

void yuyv_to_rgb(const uint8_t *yuyv, uint8_t *rgb, int width, int height) {
  yuyv_to_rgb_ex(yuyv, width * 2, NULL, rgb, width, height);
}

// Buffer sizing for multi-byte UTF-8 Braille (3 bytes per block) + color codes
size_t ascii_out_size(int dst_w, int dst_h, int color) {
  return ascii_out_size_for_mode(dst_w, dst_h, color, RENDER_BRAILLE);
}

size_t ascii_out_size_for_mode(int dst_w, int dst_h, int color,
                               render_mode_t mode) {
  int braille_term_w = dst_w / 2;
  int braille_term_h = dst_h / 4;

  switch (mode) {
  case RENDER_HALF_BLOCK: {
    int tw = dst_w;
    int th = dst_h / 2;
    size_t per_cell = color ? 41 : 3;

    return ASCII_ROW_END_MAX +
           (size_t)th * ((size_t)tw * per_cell + ASCII_ROW_END_MAX) + 1;
  }
  case RENDER_BLOCKS:
  case RENDER_ASCII_RAMP:
  case RENDER_DOTS: {
    int tw = braille_term_w;
    int th = braille_term_h;
    size_t per_cell = color ? 22 : 3; // Worst case: 3-byte glyph

    return ASCII_ROW_END_MAX +
           (size_t)th * ((size_t)tw * per_cell + ASCII_ROW_END_MAX) + 1;
  }
  case RENDER_BRAILLE:
  default: {
    int tw = braille_term_w;
    int th = braille_term_h;
    if (color) {
      return ASCII_ROW_END_MAX +
             (size_t)th * ((size_t)tw * 22 + ASCII_ROW_END_MAX) + 1;
    }

    return ASCII_ROW_END_MAX +
           (size_t)th * ((size_t)tw * 3 + ASCII_ROW_END_MAX) + 1;
  }
  }
}

// Sobel edge detection
static void sobel(const uint8_t *in, uint8_t *out, int w, int h) {
  static const int Gx[3][3] = {{-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1}};
  static const int Gy[3][3] = {{-1, -2, -1}, {0, 0, 0}, {1, 2, 1}};
  for (int y = 1; y < h - 1; y++) {
    for (int x = 1; x < w - 1; x++) {
      int gx = 0;
      int gy = 0;
      for (int ky = -1; ky <= 1; ky++) {
        for (int kx = -1; kx <= 1; kx++) {
          int p = in[(y + ky) * w + (x + kx)];

          gx += Gx[ky + 1][kx + 1] * p;
          gy += Gy[ky + 1][kx + 1] * p;
        }
      }

      int mag = my_abs(gx) + my_abs(gy);
      out[y * w + x] = (uint8_t)(mag > 255 ? 255 : mag);
    }
  }
}

static void sobel_dir(const uint8_t *in, uint8_t *out_mag, uint8_t *out_dir,
                      int w, int h) {
  static const int Gx[3][3] = {{-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1}};
  static const int Gy[3][3] = {{-1, -2, -1}, {0, 0, 0}, {1, 2, 1}};
  for (int y = 1; y < h - 1; y++) {
    for (int x = 1; x < w - 1; x++) {
      int gx = 0;
      int gy = 0;
      for (int ky = -1; ky <= 1; ky++) {
        for (int kx = -1; kx <= 1; kx++) {
          int p = in[(y + ky) * w + (x + kx)];
          gx += Gx[ky + 1][kx + 1] * p;
          gy += Gy[ky + 1][kx + 1] * p;
        }
      }

      int mag = my_abs(gx) + my_abs(gy);
      out_mag[y * w + x] = (uint8_t)(mag > 255 ? 255 : mag);

      // |gx| dominates -> gradient mostly horizontal -> vertical edge glyph
      // |gy| dominates, or no gradient at all -> horizontal edge glyph
      //   (no-gradient case picks horizontal arbitrarily but stably)
      uint8_t dir;
      int agx = my_abs(gx), agy = my_abs(gy);
      if (agy * 5 < agx * 2) {
        dir = 2;
      } else if ((agx == 0 && agy == 0) || (agx * 5 < agy * 2)) {
        dir = 0;
      } else {
        dir = ((gx > 0) == (gy > 0)) ? 3 : 1;
      }

      out_dir[y * w + x] = dir;
    }
  }
}

static void laplacian(const uint8_t *in, uint8_t *out, int w, int h) {
  for (int y = 1; y < h - 1; y++) {
    for (int x = 1; x < w - 1; x++) {
      int center = in[y * w + x];
      int lap = 4 * center - in[(y - 1) * w + x] - in[(y + 1) * w + x] -
                in[y * w + (x - 1)] - in[y * w + (x + 1)];

      out[y * w + x] = (uint8_t)(my_abs(lap) > 255 ? 255 : my_abs(lap));
    }
  }
}

static inline uint8_t get_braille_bitmask(int dx, int dy) {
  static const uint8_t braille_dots[2][4] = {
      {0x01, 0x02, 0x04, 0x40}, // Left layout column:  dots 1, 2, 3, 7
      {0x08, 0x10, 0x20, 0x80}  // Right layout column: dots 4, 5, 6, 8
  };

  return braille_dots[dx][dy];
}

// Charset registry: hot-reloadable character ramps from a directory
static void _trim_line(char *s) {
  // Cut at first newline/CR, then trim trailing whitespace
  for (char *p = s; *p; p++) {
    if (*p == '\n' || *p == '\r') {
      *p = '\0';
      break;
    }
  }

  size_t len = nl_strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
    s[--len] = '\0';
  }
}

static void _name_from_path(const char *path, char *out, size_t out_sz) {
  char tmp[256];
  nl_strncpy_safe(tmp, path, sizeof(tmp));
  const char *base = basename(tmp);
  nl_strncpy_safe(out, base, out_sz);
  // Strip extension
  for (size_t i = 0; out[i]; i++) {
    if (out[i] == '.') {
      out[i] = '\0';
      break;
    }
  }
}

int charset_registry_init(charset_registry_t *reg, const char *dir) {
  nl_memset(reg, 0, sizeof(*reg));
  reg->inotify_fd = -1;
  reg->inotify_wd = -1;
  nl_strncpy_safe(reg->dir_path, dir, sizeof(reg->dir_path));

  nl_strncpy_safe(reg->sets[0].name, "default", CHARSET_NAME_LEN);
  nl_strncpy_safe(reg->sets[0].ramp, ASCII_CHARS_DEFAULT, CHARSET_RAMP_LEN);
  reg->count = 1;
  reg->active = 0;

  charset_registry_scan(reg);

  reg->inotify_fd = inotify_init1(IN_NONBLOCK);
  if (reg->inotify_fd >= 0) {
    reg->inotify_wd =
        inotify_add_watch(reg->inotify_fd, dir,
                          IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
  }

  return 0;
}

// TEST:
void charset_registry_scan(charset_registry_t *reg) {
  static const char *known[] = {"blocks",  "ascii", "dots",  "shades",
                                "minimal", "emoji", "lines", NULL};
  for (int i = 0; known[i] && reg->count < MAX_CHARSETS; i++) {
    char path[256];
    nl_snprintf(path, sizeof(path), "%s/%s.txt", reg->dir_path, known[i]);

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
      continue;
    }

    char buf[CHARSET_RAMP_LEN];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
      continue;
    }

    buf[n] = '\0';
    _trim_line(buf);
    if (buf[0] == '\0') {
      continue;
    }

    // Skip if charset with name is already loaded
    int dup = 0;
    for (int j = 0; j < reg->count; j++) {
      char nm[CHARSET_NAME_LEN];
      _name_from_path(path, nm, sizeof(nm));
      if (nl_strcmp(reg->sets[j].name, nm) == 0) {
        nl_strncpy_safe(reg->sets[j].ramp, buf, CHARSET_RAMP_LEN);
        dup = 1;
        break;
      }
    }

    if (dup) {
      continue;
    }

    _name_from_path(path, reg->sets[reg->count].name, CHARSET_NAME_LEN);
    nl_strncpy_safe(reg->sets[reg->count].ramp, buf, CHARSET_RAMP_LEN);
    reg->count++;
  }
}

void charset_registry_check_reload(charset_registry_t *reg) {
  if (reg->inotify_fd < 0) {
    return;
  }

  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  ssize_t n = read(reg->inotify_fd, buf, sizeof(buf));
  if (n <= 0) {
    return;
  }

  charset_registry_scan(reg);
}

const char *charset_registry_active_ramp(const charset_registry_t *reg) {
  if (reg->count == 0) {
    return ASCII_CHARS_DEFAULT;
  }

  int idx = reg->active;
  if (idx < 0 || idx >= reg->count) {
    idx = 0;
  }

  return reg->sets[idx].ramp;
}

void charset_registry_cleanup(charset_registry_t *reg) {
  if (reg->inotify_fd >= 0) {
    close(reg->inotify_fd);
    reg->inotify_fd = -1;
  }
}

// Grayscale to ASCII
// Block-shade ramp
static const char *BLOCK_SHADE_UTF8[] = {" ", "\xe2\x96\x91", "\xe2\x96\x92",
                                         "\xe2\x96\x93", "\xe2\x96\x88"};
#define BLOCK_SHADE_COUNT 5

// Dot ramp
static const char *DOT_UTF8[] = {" ", "\xc2\xb7", "\xe2\x80\xa2",
                                 "\xe2\x97\x8f"};
#define DOT_COUNT 4

// Half block glyph
static const char HALF_BLOCK_UTF8[] = "\xe2\x96\x80";

const char *render_mode_name(render_mode_t m) {
  switch (m) {
  case RENDER_BRAILLE:
    return "braille";
  case RENDER_BLOCKS:
    return "blocks";
  case RENDER_ASCII_RAMP:
    return "ascii";
  case RENDER_HALF_BLOCK:
    return "halfblock";
  case RENDER_DOTS:
    return "dots";
  default:
    return "?";
  }
}

const char *edge_mode_name(edge_mode_t m) {
  switch (m) {
  case EDGE_OFF:
    return "off";
  case EDGE_SOBEL:
    return "sobel";
  case EDGE_SOBEL_DIR:
    return "sobel-dir";
  case EDGE_LAPLACIAN:
    return "laplacian";
  default:
    return "?";
  }
}

// Per-channel version of gray adjustments in grayscale_to_ascii
// (contrast around 128, brightness, optional gamma LUT)
static inline uint8_t adjust_chan(int v, int contrast, int brightness,
                                  const uint8_t *gamma_lut) {
  if (contrast != 100) {
    v = 128 + (v - 128) * contrast / 100;
  }

  v += brightness;
  uint8_t c = clamp_u8(v);

  return gamma_lut ? gamma_lut[c] : c;
}

// End of one frame row
// NOTE: A frame at left edge relies on newline returning to column 1
// one placed elsewhere (origin_x > 1) must move cursor explicitly to first
// column of next row
static int emit_row_end(char *out, size_t out_size, int out_idx, int do_color,
                        int origin_x, int origin_y, int row_done) {
  char tmp[ASCII_ROW_END_MAX];
  int n = 0;

  if (do_color) {
    n = nl_snprintf(tmp, sizeof(tmp), "\033[0m");
  }

  if (origin_x > 1) {
    n += nl_snprintf(tmp + n, sizeof(tmp) - (size_t)n, "\033[%d;%dH",
                     origin_y + row_done, origin_x);
  } else {
    n += nl_snprintf(tmp + n, sizeof(tmp) - (size_t)n, "\n");
  }

  if (n > 0 && (size_t)(out_idx + n) < out_size) {
    nl_memcpy(out + out_idx, tmp, (size_t)n);
    out_idx += n;
  }

  return out_idx;
}

static int emit_glyph(char *out, size_t out_size, int out_idx,
                      const char *glyph, int do_color, color_mode_t cmode,
                      int r, int g, int b) {
  size_t glen = nl_strlen(glyph);
  size_t room = out_size - (size_t)out_idx;
  size_t need = glen + (do_color ? NL_SGR_MAX : 0);

  if (need < room) {
    // Fast path: escape is built in place, no bounds dance
    char *p = out + out_idx;
    if (do_color) {
      p += (cmode == COLOR_256) ? nl_sgr_fg256(p, rgb_to_ansi256(r, g, b))
                                : nl_sgr_fg_rgb(p, r, g, b);
    }
    nl_memcpy(p, glyph, glen);

    return (int)((p + glen) - out);
  }

  // Tail of buffer: build in scratch so exact fit is still honoured
  char tmp[NL_SGR_MAX + 8];
  size_t n = 0;
  if (do_color) {
    n = (cmode == COLOR_256) ? nl_sgr_fg256(tmp, rgb_to_ansi256(r, g, b))
                             : nl_sgr_fg_rgb(tmp, r, g, b);
  }
  if (n + glen < room) {
    nl_memcpy(out + out_idx, tmp, n);
    nl_memcpy(out + out_idx + n, glyph, glen);
    out_idx += (int)(n + glen);
  }

  return out_idx;
}

int grayscale_to_ascii(const uint8_t *gray, const uint8_t *rgb, int src_w,
                       int src_h, int dst_w, int dst_h, char *out,
                       size_t out_size, const ascii_opts_t *opts) {
  render_mode_t render_mode = opts ? opts->render_mode : RENDER_BRAILLE;
  color_mode_t color_mode = opts ? opts->color_mode : COLOR_TRUECOLOR;

  int safe_dst_w = dst_w - (dst_w % 2);
  int safe_dst_h = (render_mode == RENDER_HALF_BLOCK) ? dst_h - (dst_h % 2)
                                                      : dst_h - (dst_h % 4);

  int brightness = opts ? opts->brightness : 0;
  int contrast = opts ? opts->contrast : 100;
  int invert = opts ? opts->invert : 0;
  int do_color = opts && opts->color && rgb;
  int gamma = opts ? opts->gamma : 100;
  if (gamma < 10) {
    gamma = 10;
  }
  if (gamma > 400) {
    gamma = 400;
  }

  edge_mode_t edge_mode = opts ? opts->edges : EDGE_OFF;
  int do_dither = opts ? opts->dither : 0;
  int thresh_limit = opts ? opts->threshold_val : 35;

  const char *ramp = (opts && opts->charset && opts->charset[0])
                         ? opts->charset
                         : ASCII_CHARS_DEFAULT;
  int ramp_len = (int)nl_strlen(ramp);
  if (ramp_len < 1) {
    ramp = ASCII_CHARS_DEFAULT;
    ramp_len = (int)nl_strlen(ramp);
  }

  uint8_t gamma_lut[256];
  if (gamma != 100) {
    // Discrete exponent set
    static const int kGammaSet[] = {25, 50, 75, 100, 150, 200, 300, 400};

    int best = 100, best_diff = 1000;
    for (size_t i = 0; i < sizeof(kGammaSet) / sizeof(kGammaSet[0]); i++) {
      int d = gamma - kGammaSet[i];
      if (d < 0) {
        d = -d;
      }
      if (d < best_diff) {
        best_diff = d;
        best = kGammaSet[i];
      }
    }

    for (int i = 0; i < 256; i++) {
      double x = i / 255.0;
      double y;
      switch (best) {
      case 25:
        y = my_sqrt(my_sqrt(x));
        break;
      case 50:
        y = my_sqrt(x);
        break;
      case 75:
        y = my_sqrt(x) * my_sqrt(my_sqrt(x));
        break;
      case 150:
        y = x * my_sqrt(x);
        break;
      case 200:
        y = x * x;
        break;
      case 300:
        y = x * x * x;
        break;
      case 400:
        y = x * x * x * x;
        break;
      default:
        y = x;
        break;
      }

      gamma_lut[i] = clamp_u8((int)(y * 255.0 + 0.5));
    }
  }

  // Tone curve runs after gamma
  // Colour channels get 2 as one table
  const uint8_t *tone = opts ? opts->tone_lut : NULL;
  uint32_t *tone_hist = opts ? opts->tone_hist : NULL;
  uint8_t tone_gamma_lut[256];
  const uint8_t *chan_lut = gamma != 100 ? gamma_lut : NULL;
  if (tone) {
    for (int i = 0; i < 256; i++) {
      tone_gamma_lut[i] = tone[gamma != 100 ? gamma_lut[i] : i];
    }

    chan_lut = tone_gamma_lut;
  }

  int depth_pop = opts ? opts->depth_pop : 0;
  int depth_invert = opts ? opts->depth_invert : 0;

  double bw = (double)src_w / safe_dst_w;
  double bh = (double)src_h / safe_dst_h;

  uint8_t *subpixel_g = nl_gc_alloc_atomic((size_t)(safe_dst_w * safe_dst_h));
  uint8_t *subpixel_rgb =
      do_color ? nl_gc_alloc_atomic((size_t)(safe_dst_w * safe_dst_h * 3))
               : NULL;

  // Cell edges (start | end) and one 64-bit sum per cell for row in hand
  int32_t *cell_x =
      nl_gc_alloc_atomic(sizeof(int32_t) * 2 * (size_t)safe_dst_w);
  uint64_t *cell_acc =
      nl_gc_alloc_atomic(sizeof(uint64_t) * (size_t)safe_dst_w);
  // Colour: R G B sums per source column for the cell row in hand
  uint32_t *col_acc =
      do_color ? nl_gc_alloc_atomic(sizeof(uint32_t) * 3 * (size_t)src_w)
               : NULL;

  if (!subpixel_g || (do_color && !subpixel_rgb) || !cell_x || !cell_acc ||
      (do_color && !col_acc)) {
    nl_gc_release(subpixel_g);
    nl_gc_release(subpixel_rgb);
    nl_gc_release(cell_x);
    nl_gc_release(cell_acc);
    nl_gc_release(col_acc);

    return -1;
  }

  // Downscale and interpolate high-res frames to subpixel boundaries
  // Edges of cell columns are same for every row, so they are worked out
  // once and clamped to picture; a whole row of cells is then summed by
  // nl_sumspan_u8 (assembly on x86-64 Linux, a C loop elsewhere): same sums
  // either way
  int32_t *cell_xs = cell_x;
  int32_t *cell_xe = cell_x + safe_dst_w;
  for (int x = 0; x < safe_dst_w; x++) {
    int xs = (int)(x * bw), xe = (int)((x + 1) * bw);
    if (xe <= xs) {
      xe = xs + 1;
    }

    if (xe > src_w) {
      xe = src_w;
    }

    if (xs > xe) {
      xs = xe;
    }

    cell_xs[x] = xs;
    cell_xe[x] = xe;
  }

  for (int y = 0; y < safe_dst_h; y++) {
    int ys = (int)(y * bh), ye = (int)((y + 1) * bh);
    if (ye <= ys) {
      ye = ys + 1;
    }

    if (ye > src_h) {
      ye = src_h;
    }

    int rows = ye > ys ? ye - ys : 0;

    for (int x = 0; x < safe_dst_w; x++) {
      cell_acc[x] = 0;
    }

    for (int sy = ys; sy < ye; sy++) {
      nl_sumspan_u8(gray + (size_t)sy * (size_t)src_w, cell_xs, cell_xe,
                    safe_dst_w, cell_acc);
    }

    // Colour: rows of cell row are added up column by column (assembly),
    // cells then only add columns: `rows` times less work than pixel by pixel
    if (do_color) {
      for (size_t i = 0; i < 3 * (size_t)src_w; i++) {
        col_acc[i] = 0;
      }

      for (int sy = ys; sy < ye; sy++) {
        nl_addrow_u8(col_acc, rgb + (size_t)sy * (size_t)src_w * 3,
                     3 * (size_t)src_w);
      }
    }

    for (int x = 0; x < safe_dst_w; x++) {
      int xs = cell_xs[x], xe = cell_xe[x];
      long tg = (long)cell_acc[x];
      long tr = 0;
      long tgv = 0;
      long tb = 0;
      int count = rows * (xe - xs);
      if (do_color) {
        for (int sx = xs; sx < xe; sx++) {
          const uint32_t *px = col_acc + (size_t)sx * 3;

          tr += px[0];
          tgv += px[1];
          tb += px[2];
        }
      }

      if (!count) {
        count = 1;
      }

      int gv = (int)(tg / count);
      if (contrast != 100) {
        gv = 128 + (gv - 128) * contrast / 100;
      }

      gv += brightness;
      int gv_c = clamp_u8(gv);
      if (gamma != 100) {
        gv_c = gamma_lut[gv_c];
      }

      if (tone_hist) {
        tone_hist[gv_c]++;
      }

      if (tone) {
        gv_c = tone[gv_c];
      }

      subpixel_g[y * safe_dst_w + x] = gv_c;

      if (do_color) {
        uint8_t *op = subpixel_rgb + (y * safe_dst_w + x) * 3;
        const uint8_t *glut = chan_lut;

        op[0] = adjust_chan((int)(tr / count), contrast, brightness, glut);
        op[1] = adjust_chan((int)(tgv / count), contrast, brightness, glut);
        op[2] = adjust_chan((int)(tb / count), contrast, brightness, glut);
      }
    }
  }

  nl_gc_release(cell_x);
  nl_gc_release(cell_acc);
  nl_gc_release(col_acc);

  // TEST:
  // Pseudo-3D "pop out" parallax effect
  if (depth_pop > 0) {
    uint8_t *warped_g = nl_gc_alloc_atomic((size_t)(safe_dst_w * safe_dst_h));
    uint8_t *warped_rgb =
        do_color ? nl_gc_alloc_atomic((size_t)(safe_dst_w * safe_dst_h * 3))
                 : NULL;
    if (warped_g && (!do_color || warped_rgb)) {
      double cx = safe_dst_w / 2.0;
      double cy = safe_dst_h / 2.0;
      double max_r = (cx > cy ? cx : cy);
      double strength = (depth_pop / 100.0) * (max_r * 0.35);

      for (int y = 0; y < safe_dst_h; y++) {
        for (int x = 0; x < safe_dst_w; x++) {
          uint8_t luma = subpixel_g[y * safe_dst_w + x];
          double depth01 = depth_invert ? (255 - luma) / 255.0 : luma / 255.0;

          double dx = x - cx, dy = y - cy;
          double r = (dx * dx + dy * dy);
          double rlen = r > 0.0001 ? my_sqrt(r) : 0.0001;
          double nx = dx / rlen, ny = dy / rlen;

          double rnorm = rlen / (max_r > 0.0001 ? max_r : 0.0001);
          double disp = strength * depth01 * rnorm;

          int sx = (int)(x + nx * disp);
          int sy = (int)(y + ny * disp);
          if (sx < 0) {
            sx = 0;
          }
          if (sx >= safe_dst_w) {
            sx = safe_dst_w - 1;
          }
          if (sy < 0) {
            sy = 0;
          }
          if (sy >= safe_dst_h) {
            sy = safe_dst_h - 1;
          }

          warped_g[y * safe_dst_w + x] = subpixel_g[sy * safe_dst_w + sx];
          if (do_color) {
            const uint8_t *src_px = subpixel_rgb + (sy * safe_dst_w + sx) * 3;
            uint8_t *dst_px = warped_rgb + (y * safe_dst_w + x) * 3;

            dst_px[0] = src_px[0];
            dst_px[1] = src_px[1];
            dst_px[2] = src_px[2];
          }
        }
      }

      nl_memcpy(subpixel_g, warped_g, (size_t)(safe_dst_w * safe_dst_h));
      if (do_color) {
        nl_memcpy(subpixel_rgb, warped_rgb,
                  (size_t)(safe_dst_w * safe_dst_h * 3));
      }
    }

    nl_gc_release(warped_g);
    nl_gc_release(warped_rgb);
  }

  // Edge detection dispatch
  uint8_t *dir_buf = NULL;
  if (edge_mode != EDGE_OFF) {
    uint8_t *eb = nl_gc_alloc_atomic((size_t)(safe_dst_w * safe_dst_h));
    if (eb) {
      switch (edge_mode) {
      case EDGE_SOBEL:
        sobel(subpixel_g, eb, safe_dst_w, safe_dst_h);

        break;
      case EDGE_SOBEL_DIR:
        dir_buf = nl_gc_alloc_atomic((size_t)(safe_dst_w * safe_dst_h));
        if (dir_buf) {
          sobel_dir(subpixel_g, eb, dir_buf, safe_dst_w, safe_dst_h);
        } else {
          sobel(subpixel_g, eb, safe_dst_w, safe_dst_h);
        }

        break;
      case EDGE_LAPLACIAN:
        laplacian(subpixel_g, eb, safe_dst_w, safe_dst_h);

        break;

      default:
        break;
      }

      nl_memcpy(subpixel_g, eb, (size_t)(safe_dst_w * safe_dst_h));
      nl_gc_release(eb);
    }
  }

  // Floyd-Steinberg dithering on subpixel grayscale buffer
  if (do_dither) {
    int16_t *err =
        nl_gc_alloc_atomic((size_t)(safe_dst_w * safe_dst_h) * sizeof(int16_t));
    if (err) {
      for (int i = 0; i < safe_dst_w * safe_dst_h; i++) {
        err[i] = (int16_t)subpixel_g[i];
      }

      for (int y = 0; y < safe_dst_h; y++) {
        for (int x = 0; x < safe_dst_w; x++) {
          int idx = y * safe_dst_w + x;
          int16_t old = err[idx];
          int16_t nval = (old > (int16_t)thresh_limit) ? 255 : 0;
          int16_t qerr = (int16_t)(old - nval);
          err[idx] = nval;

          if (x + 1 < safe_dst_w) {
            err[idx + 1] = (int16_t)(err[idx + 1] + ((qerr * 7) >> 4));
          }
          if (y + 1 < safe_dst_h) {
            if (x > 0) {
              err[idx + safe_dst_w - 1] =
                  (int16_t)(err[idx + safe_dst_w - 1] + ((qerr * 3) >> 4));
            }

            err[idx + safe_dst_w] =
                (int16_t)(err[idx + safe_dst_w] + ((qerr * 5) >> 4));
            if (x + 1 < safe_dst_w) {
              err[idx + safe_dst_w + 1] =
                  (int16_t)(err[idx + safe_dst_w + 1] + ((qerr * 1) >> 4));
            }
          }
        }
      }

      for (int i = 0; i < safe_dst_w * safe_dst_h; i++) {
        int16_t v = err[i];
        subpixel_g[i] = (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
      }

      nl_gc_release(err);
    }
  }

  int out_idx = 0;
  // Frame origin, 1-based terminal cell (NOTE: see ascii_opts_t.origin_x)
  int origin_x = (opts && opts->origin_x > 1) ? opts->origin_x : 1;
  int origin_y = (opts && opts->origin_y > 1) ? opts->origin_y : 1;
  if (out_size > ASCII_ROW_END_MAX) {
    int n = (origin_x > 1 || origin_y > 1)
                ? nl_snprintf(out, out_size, "\033[%d;%dH", origin_y, origin_x)
                : nl_snprintf(out, out_size, "\033[H");
    if (n > 0) {
      out_idx = n;
    }
  }

  if (render_mode == RENDER_HALF_BLOCK) {
    int term_w = safe_dst_w;
    int term_h = safe_dst_h / 2;
    for (int ty = 0; ty < term_h; ty++) {
      for (int tx = 0; tx < term_w; tx++) {
        int top_idx = (ty * 2) * safe_dst_w + tx;
        int bot_idx = (ty * 2 + 1) * safe_dst_w + tx;

        if (do_color) {
          const uint8_t *tp = subpixel_rgb + top_idx * 3;
          const uint8_t *bp = subpixel_rgb + bot_idx * 3;

          char tmp[NL_SGR_MAX + 4];
          size_t n =
              (color_mode == COLOR_256)
                  ? nl_sgr_fgbg256(tmp, rgb_to_ansi256(tp[0], tp[1], tp[2]),
                                   rgb_to_ansi256(bp[0], bp[1], bp[2]))
                  : nl_sgr_fgbg_rgb(tmp, tp, bp);
          size_t glen = nl_strlen(HALF_BLOCK_UTF8);

          if (n + glen < out_size - (size_t)out_idx) {
            nl_memcpy(out + out_idx, tmp, n);
            nl_memcpy(out + out_idx + n, HALF_BLOCK_UTF8, glen);
            out_idx += (int)(n + glen);
          }
        } else {
          uint8_t top_l = subpixel_g[top_idx];
          uint8_t bot_l = subpixel_g[bot_idx];
          int active = (top_l > thresh_limit) || (bot_l > thresh_limit);

          if (invert) {
            active = !active;
          }

          const char *glyph = active ? "\xe2\x96\x88" : " ";
          out_idx = emit_glyph(out, out_size, out_idx, glyph, 0,
                               COLOR_TRUECOLOR, 0, 0, 0);
        }
      }

      out_idx = emit_row_end(out, out_size, out_idx, do_color, origin_x,
                             origin_y, ty + 1);
    }

    out[(size_t)out_idx < out_size ? (size_t)out_idx : out_size - 1] = '\0';

    nl_gc_release(subpixel_g);
    nl_gc_release(subpixel_rgb);
    nl_gc_release(dir_buf);

    return out_idx;
  }

  // BRAILLE / BLOCKS / ASCII_RAMP / DOTS
  int term_w = safe_dst_w / 2;
  int term_h = safe_dst_h / 4;

  for (int ty = 0; ty < term_h; ty++) {
    for (int tx = 0; tx < term_w; tx++) {
      uint8_t braille_offset = 0;
      long sum_r = 0;
      long sum_g = 0;
      long sum_b = 0;
      long sum_luma = 0;
      int colored_subpixels = 0;
      int active_count = 0;
      uint8_t dom_dir = 0;
      int dom_dir_votes[4] = {0, 0, 0, 0};

      for (int dy = 0; dy < 4; dy++) {
        for (int dx = 0; dx < 2; dx++) {
          int sub_x = tx * 2 + dx;
          int sub_y = ty * 4 + dy;
          int pixel_idx = sub_y * safe_dst_w + sub_x;

          uint8_t luma = subpixel_g[pixel_idx];
          sum_luma += luma;

          int is_active = (luma > thresh_limit);
          if (invert) {
            is_active = !is_active;
          }

          if (is_active) {
            braille_offset |= get_braille_bitmask(dx, dy);
            active_count++;
            if (dir_buf) {
              dom_dir_votes[dir_buf[pixel_idx]]++;
            }
          }

          if (do_color) {
            const uint8_t *px = subpixel_rgb + pixel_idx * 3;
            sum_r += px[0];
            sum_g += px[1];
            sum_b += px[2];
            colored_subpixels++;
          }
        }
      }

      if (dir_buf) {
        int best = 0;
        for (int d = 1; d < 4; d++) {
          if (dom_dir_votes[d] > dom_dir_votes[best])
            best = d;
        }

        dom_dir = (uint8_t)best;
      }

      int avg_r = 0;
      int avg_g = 0;
      int avg_b = 0;
      if (do_color && colored_subpixels > 0) {
        avg_r = (int)(sum_r / colored_subpixels);
        avg_g = (int)(sum_g / colored_subpixels);
        avg_b = (int)(sum_b / colored_subpixels);
      }

      int avg_luma = (int)(sum_luma / 8);

      switch (render_mode) {
      case RENDER_BRAILLE: {
        uint32_t unicode_val = 0x2800 + braille_offset;
        char utf8_seq[4];

        utf8_seq[0] = (char)(0xE0 | ((unicode_val >> 12) & 0x0F));
        utf8_seq[1] = (char)(0x80 | ((unicode_val >> 6) & 0x3F));
        utf8_seq[2] = (char)(0x80 | (unicode_val & 0x3F));
        utf8_seq[3] = '\0';

        out_idx = emit_glyph(out, out_size, out_idx, utf8_seq, do_color,
                             color_mode, avg_r, avg_g, avg_b);

        break;
      }
      case RENDER_BLOCKS: {
        int shade_idx = avg_luma * BLOCK_SHADE_COUNT / 256;
        if (shade_idx >= BLOCK_SHADE_COUNT) {
          shade_idx = BLOCK_SHADE_COUNT - 1;
        }
        if (invert) {
          shade_idx = BLOCK_SHADE_COUNT - 1 - shade_idx;
        }

        out_idx =
            emit_glyph(out, out_size, out_idx, BLOCK_SHADE_UTF8[shade_idx],
                       do_color, color_mode, avg_r, avg_g, avg_b);

        break;
      }
      case RENDER_DOTS: {
        int dot_idx = avg_luma * DOT_COUNT / 256;
        if (dot_idx >= DOT_COUNT) {
          dot_idx = DOT_COUNT - 1;
        }
        if (invert) {
          dot_idx = DOT_COUNT - 1 - dot_idx;
        }

        out_idx = emit_glyph(out, out_size, out_idx, DOT_UTF8[dot_idx],
                             do_color, color_mode, avg_r, avg_g, avg_b);

        break;
      }
      case RENDER_ASCII_RAMP:
      default: {
        char glyph[2] = {0, 0};
        if (dir_buf && active_count > 0) {
          static const char dirs[4] = {'-', '/', '|', '\\'};
          glyph[0] = dirs[dom_dir];
        } else {
          int ramp_idx = avg_luma * ramp_len / 256;
          if (ramp_idx >= ramp_len) {
            ramp_idx = ramp_len - 1;
          }
          if (invert) {
            ramp_idx = ramp_len - 1 - ramp_idx;
          }

          glyph[0] = ramp[ramp_idx];
        }

        out_idx = emit_glyph(out, out_size, out_idx, glyph, do_color,
                             color_mode, avg_r, avg_g, avg_b);

        break;
      }
      }
    }

    out_idx = emit_row_end(out, out_size, out_idx, do_color, origin_x, origin_y,
                           ty + 1);
  }

  out[(size_t)out_idx < out_size ? (size_t)out_idx : out_size - 1] = '\0';

  nl_gc_release(subpixel_g);
  nl_gc_release(subpixel_rgb);
  nl_gc_release(dir_buf);

  return out_idx;
}

// FPS Overlay Configuration
void overlay_fps_box(int dst_w, double fps, int color_enabled) {
  char buf[80];
  int term_w = dst_w / 2;
  int col = (term_w - 13) / 2 + 1;
  if (col < 1) {
    col = 1;
  }

  char fpsbuf[10];
  nl_fmt_fps(fpsbuf, sizeof(fpsbuf), fps);

  int n;
  if (color_enabled) {
    n = nl_snprintf(
        buf, sizeof(buf),
        "\033[1;%dH\033[38;2;0;255;0m\033[48;2;30;30;30m[ FPS: %s ]\033[0m",
        col, fpsbuf);
  } else {
    n = nl_snprintf(buf, sizeof(buf), "\033[1;%dH[ FPS: %s ]", col, fpsbuf);
  }

  if (n > 0 && n < (int)sizeof(buf)) {
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }
}
