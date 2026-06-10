// NOTE:
// SIMD paths:
// -   x86_64 Linux/macOS: SSE2 via <immintrin.h>
// -   ARM64  macOS:       NEON via <arm_neon.h>

#include "ascii.h"
#include <stdint.h>

#include "nolibc.h"
#include "platform.h"

// Helpers
static inline uint8_t clamp_u8(int v) {
  return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}
static inline int my_abs(int x) { return x < 0 ? -x : x; }

// YUYV to grayscale
#if defined(ARCH_X86_64)
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
  for (; i < total; i++)
    gray[i] = yuyv[i * 2];
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
  for (; i < total; i++)
    gray[i] = yuyv[i * 2];
}

#else
// fallback
void yuyv_to_gray_simd(const uint8_t *yuyv, uint8_t *gray, int width,
                       int height) {
  int total = width * height;
  for (int i = 0; i < total; i++)
    gray[i] = yuyv[i * 2];
}
#endif

void yuyv_to_rgb(const uint8_t *yuyv, uint8_t *rgb, int width, int height) {
  int pairs = (width * height) / 2;
  for (int i = 0; i < pairs; i++) {
    int y0 = yuyv[i * 4 + 0], u = yuyv[i * 4 + 1];
    int y1 = yuyv[i * 4 + 2], v = yuyv[i * 4 + 3];
    int d = u - 128, e = v - 128;
    for (int p = 0; p < 2; p++) {
      int c = ((p == 0) ? y0 : y1) - 16;
      uint8_t *px = rgb + (i * 2 + p) * 3;
      px[0] = clamp_u8((298 * c + 409 * e + 128) >> 8);
      px[1] = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
      px[2] = clamp_u8((298 * c + 516 * d + 128) >> 8);
    }
  }
}

// Buffer sizing
size_t ascii_out_size(int dst_w, int dst_h, int color) {
  if (color)
    return 3 + (size_t)dst_h * ((size_t)dst_w * 21 + 6) + 1;
  return 3 + (size_t)dst_h * ((size_t)dst_w + 1) + 1;
}

// Sobel edge detection
static void sobel(const uint8_t *in, uint8_t *out, int w, int h) {
  static const int Gx[3][3] = {{-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1}};
  static const int Gy[3][3] = {{-1, -2, -1}, {0, 0, 0}, {1, 2, 1}};
  for (int y = 1; y < h - 1; y++) {
    for (int x = 1; x < w - 1; x++) {
      int gx = 0, gy = 0;
      for (int ky = -1; ky <= 1; ky++)
        for (int kx = -1; kx <= 1; kx++) {
          int p = in[(y + ky) * w + (x + kx)];
          gx += Gx[ky + 1][kx + 1] * p;
          gy += Gy[ky + 1][kx + 1] * p;
        }
      int mag = my_abs(gx) + my_abs(gy);
      out[y * w + x] = (uint8_t)(mag > 255 ? 255 : mag);
    }
  }
}

// Grayscale to ASCII
int grayscale_to_ascii(const uint8_t *gray, const uint8_t *rgb, int src_w,
                       int src_h, int dst_w, int dst_h, char *out,
                       size_t out_size, const ascii_opts_t *opts) {
  const char *charset =
      (opts && opts->charset) ? opts->charset : ASCII_CHARS_DEFAULT;
  int nchars = (int)strlen(charset);
  int brightness = opts ? opts->brightness : 0;
  int contrast = opts ? opts->contrast : 100;
  int invert = opts ? opts->invert : 0;
  int do_color = opts && opts->color && rgb;
  int do_edges = opts ? opts->edges : 0;
  int do_dither = opts ? opts->dither : 0;

  double bw = (double)src_w / dst_w;
  double bh = (double)src_h / dst_h;

  uint8_t *small_g = malloc((size_t)(dst_w * dst_h));
  uint8_t *small_rgb = do_color ? malloc((size_t)(dst_w * dst_h * 3)) : NULL;

  if (!small_g || (do_color && !small_rgb)) {
    free(small_g);
    free(small_rgb);
    return -1;
  }

  for (int y = 0; y < dst_h; y++) {
    int ys = (int)(y * bh), ye = (int)((y + 1) * bh);
    if (ye <= ys)
      ye = ys + 1;
    for (int x = 0; x < dst_w; x++) {
      int xs = (int)(x * bw), xe = (int)((x + 1) * bw);
      if (xe <= xs)
        xe = xs + 1;

      long tg = 0, tr = 0, tgv = 0, tb = 0;
      int count = 0;
      for (int sy = ys; sy < ye && sy < src_h; sy++)
        for (int sx = xs; sx < xe && sx < src_w; sx++) {
          tg += gray[sy * src_w + sx];
          if (do_color) {
            const uint8_t *px = rgb + (sy * src_w + sx) * 3;
            tr += px[0];
            tgv += px[1];
            tb += px[2];
          }
          count++;
        }
      if (!count)
        count = 1;

      int gv = (int)(tg / count);
      if (contrast != 100)
        gv = 128 + (gv - 128) * contrast / 100;
      gv += brightness;
      small_g[y * dst_w + x] = clamp_u8(gv);

      if (do_color) {
        uint8_t *op = small_rgb + (y * dst_w + x) * 3;
        op[0] = clamp_u8((int)(tr / count));
        op[1] = clamp_u8((int)(tgv / count));
        op[2] = clamp_u8((int)(tb / count));
      }
    }
  }

  if (do_edges) {
    uint8_t *eb = calloc((size_t)(dst_w * dst_h), 1);
    if (eb) {
      sobel(small_g, eb, dst_w, dst_h);
      nl_memcpy(small_g, eb, (size_t)(dst_w * dst_h));
      free(eb);
    }
  }

  if (do_dither) {
    int16_t *eb = malloc((size_t)(dst_w * dst_h) * sizeof(int16_t));
    if (eb) {
      for (int i = 0; i < dst_w * dst_h; i++)
        eb[i] = (int16_t)small_g[i];
      for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
          int old_v = eb[y * dst_w + x];
          int qi = old_v * (nchars - 1) / 255;
          if (qi < 0)
            qi = 0;
          if (qi >= nchars)
            qi = nchars - 1;
          int new_v = qi * 255 / (nchars - 1);
          eb[y * dst_w + x] = (int16_t)new_v;
          int err = old_v - new_v;
#define FS(DY, DX, N)                                                          \
  do {                                                                         \
    int ny = y + (DY), nx = x + (DX);                                          \
    if (nx >= 0 && nx < dst_w && ny >= 0 && ny < dst_h)                        \
      eb[ny * dst_w + nx] += (int16_t)(err * (N) / 16);                        \
  } while (0)
          FS(0, 1, 7);
          FS(1, -1, 3);
          FS(1, 0, 5);
          FS(1, 1, 1);
#undef FS
        }
      }
      for (int i = 0; i < dst_w * dst_h; i++)
        small_g[i] = clamp_u8(eb[i]);
      free(eb);
    }
  }

  int out_idx = 0;
  static const char HOME[] = "\033[H";
  if (out_size > sizeof(HOME)) {
    nl_memcpy(out, HOME, sizeof(HOME) - 1);
    out_idx = sizeof(HOME) - 1;
  }

  for (int y = 0; y < dst_h; y++) {
    for (int x = 0; x < dst_w; x++) {
      int gv = small_g[y * dst_w + x];
      int idx = gv * (nchars - 1) / 255;
      if (idx < 0)
        idx = 0;
      if (idx >= nchars)
        idx = nchars - 1;
      if (invert)
        idx = nchars - 1 - idx;
      char ch = charset[idx];

      if (do_color) {
        const uint8_t *px = small_rgb + (y * dst_w + x) * 3;
        int w = snprintf(out + out_idx, out_size - (size_t)out_idx,
                         "\033[38;2;%d;%d;%dm%c", px[0], px[1], px[2], ch);
        if (w > 0 && (size_t)(out_idx + w) < out_size)
          out_idx += w;
      } else {
        if ((size_t)(out_idx + 1) < out_size)
          out[out_idx++] = ch;
      }
    }
    if (do_color) {
      int w = snprintf(out + out_idx, out_size - (size_t)out_idx, "\033[0m\n");
      if (w > 0 && (size_t)(out_idx + w) < out_size)
        out_idx += w;
    } else {
      if ((size_t)(out_idx + 1) < out_size)
        out[out_idx++] = '\n';
    }
  }

  out[(size_t)out_idx < out_size ? (size_t)out_idx : out_size - 1] = '\0';
  free(small_g);
  free(small_rgb);
  return out_idx;
}

// FPS overlay
void overlay_fps_box(int dst_w, double fps, int color_enabled) {
  char buf[80];
  int col = (dst_w - 13) / 2 + 1;
  if (col < 1)
    col = 1;

  char fpsbuf[10];
  nl_fmt_fps(fpsbuf, sizeof(fpsbuf), fps);

  int n;
  if (color_enabled)
    n = snprintf(
        buf, sizeof(buf),
        "\033[1;%dH\033[38;2;0;255;0m\033[48;2;30;30;30m[ FPS: %s ]\033[0m",
        col, fpsbuf);
  else
    n = snprintf(buf, sizeof(buf), "\033[1;%dH[ FPS: %s ]", col, fpsbuf);

  if (n > 0 && n < (int)sizeof(buf))
    (void)write(STDOUT_FILENO, buf, (size_t)n);
}
