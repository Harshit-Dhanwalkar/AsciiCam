#include "ascii.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helpers
static inline uint8_t clamp_u8(int v) {
  return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}

// Image conversion
void yuyv_to_gray(const uint8_t *yuyv, uint8_t *gray, int width, int height) {
  int n = width * height;
  for (int i = 0; i < n; i++)
    gray[i] = yuyv[i * 2]; // Y sample at even bytes in YUYV format
}

void yuyv_to_rgb(const uint8_t *yuyv, uint8_t *rgb, int width, int height) {
  int pairs = (width * height) / 2;
  for (int i = 0; i < pairs; i++) {
    int y0 = yuyv[i * 4 + 0];
    int u = yuyv[i * 4 + 1];
    int y1 = yuyv[i * 4 + 2];
    int v = yuyv[i * 4 + 3];
    int d = u - 128;
    int e = v - 128;

    for (int p = 0; p < 2; p++) {
      int c = ((p == 0) ? y0 : y1) - 16;
      uint8_t *px = rgb + (i * 2 + p) * 3;
      px[0] = clamp_u8((298 * c + 409 * e + 128) >> 8);           // R
      px[1] = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8); // G
      px[2] = clamp_u8((298 * c + 516 * d + 128) >> 8);           // B
    }
  }
}

// Buffer sizing
size_t ascii_out_size(int dst_w, int dst_h, int color) {
  /* prefix: 3 bytes + 1 Char */
  if (color) {
    /* Per cell: "\033[38;2;255;255;255m" (20) + char (1) = 21
     * Per row end: "\033[0m\n" (6) */
    return 3 + (size_t)dst_h * ((size_t)dst_w * 21 + 6) + 1;
  } else {
    /* Per cell: 1 byte; per row: + newline */
    return 3 + (size_t)dst_h * ((size_t)dst_w + 1) + 1;
  }
}

//  Grayscale to ascii
int grayscale_to_ascii(const uint8_t *gray, const uint8_t *rgb, int src_w,
                       int src_h, int dst_w, int dst_h, char *out,
                       size_t out_size, const ascii_opts_t *opts) {
  const char *charset =
      (opts && opts->charset) ? opts->charset : ASCII_CHARS_DEFAULT;
  int nchars = (int)strlen(charset);
  int brightness = opts ? opts->brightness : 0;
  int contrast = opts ? opts->contrast : 100;
  int invert = opts ? opts->invert : 0;
  int do_color = opts && opts->color && (rgb != NULL);
  int do_dither = opts ? opts->dither : 0;

  // Blocking Widht and height pixels in source pixels
  double bw = (double)src_w / dst_w;
  double bh = (double)src_h / dst_h;

  // Downsample to (dst_w x dst_h)
  uint8_t *small_g = malloc(dst_w * dst_h);
  uint8_t *small_rgb = do_color ? malloc(dst_w * dst_h * 3) : NULL;

  if (!small_g || (do_color && !small_rgb)) {
    free(small_g);
    free(small_rgb);
    return -1;
  }

  for (int y = 0; y < dst_h; y++) {
    int ys = (int)(y * bh);
    int ye = (int)((y + 1) * bh);
    if (ye <= ys)
      ye = ys + 1;

    for (int x = 0; x < dst_w; x++) {
      int xs = (int)(x * bw);
      int xe = (int)((x + 1) * bw);
      if (xe <= xs)
        xe = xs + 1;

      long tg = 0, tr = 0, tgv = 0, tb = 0;
      int cnt = 0;

      for (int sy = ys; sy < ye && sy < src_h; sy++) {
        for (int sx = xs; sx < xe && sx < src_w; sx++) {
          tg += gray[sy * src_w + sx];
          if (do_color) {
            const uint8_t *px = rgb + (sy * src_w + sx) * 3;
            tr += px[0];
            tgv += px[1];
            tb += px[2];
          }
          cnt++;
        }
      }
      if (cnt == 0)
        cnt = 1;

      // Brightness and contrast
      int gv = (int)(tg / cnt);
      if (contrast != 100)
        gv = 128 + (gv - 128) * contrast / 100;
      gv += brightness;
      small_g[y * dst_w + x] = clamp_u8(gv);

      if (do_color) {
        uint8_t *out_px = small_rgb + (y * dst_w + x) * 3;
        out_px[0] = clamp_u8((int)(tr / cnt));
        out_px[1] = clamp_u8((int)(tgv / cnt));
        out_px[2] = clamp_u8((int)(tb / cnt));
      }
    }
  }

  // Floyd-Steinberg dithering
  if (do_dither) {
    int16_t *eb = malloc(dst_w * dst_h * sizeof(int16_t));
    if (eb) {
      for (int i = 0; i < dst_w * dst_h; i++)
        eb[i] = (int16_t)small_g[i];

      for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
          int old_v = eb[y * dst_w + x];

          // Quantise to nearest charset level
          int qi = old_v * (nchars - 1) / 255;
          if (qi < 0)
            qi = 0;
          if (qi >= nchars)
            qi = nchars - 1;
          int new_v = qi * 255 / (nchars - 1);

          eb[y * dst_w + x] = (int16_t)new_v;
          int err = old_v - new_v;

#define FS_ADD(DY, DX, NUM)                                                    \
  do {                                                                         \
    int ny = y + (DY), nx = x + (DX);                                          \
    if (nx >= 0 && nx < dst_w && ny >= 0 && ny < dst_h)                        \
      eb[ny * dst_w + nx] += (int16_t)(err * (NUM) / 16);                      \
  } while (0)
          FS_ADD(0, 1, 7);
          FS_ADD(1, -1, 3);
          FS_ADD(1, 0, 5);
          FS_ADD(1, 1, 1);
#undef FS_ADD
        }
      }

      for (int i = 0; i < dst_w * dst_h; i++)
        small_g[i] = clamp_u8(eb[i]);

      free(eb);
    }
    /* HACK: If malloc fails silently fall back to no dithering */
  }

  // Render into caller's buffer
  int out_idx = 0;

  // Cursor repositions without erasing
  static const char HOME[] = "\033[H";
  if (out_size > sizeof(HOME)) {
    memcpy(out, HOME, sizeof(HOME) - 1);
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
      if (w > 0)
        out_idx += w;
    } else {
      if ((size_t)(out_idx + 1) < out_size)
        out[out_idx++] = '\n';
    }
  }

  if ((size_t)out_idx < out_size)
    out[out_idx] = '\0';
  else
    out[out_size - 1] = '\0';

  free(small_g);
  free(small_rgb);
  return out_idx;
}
