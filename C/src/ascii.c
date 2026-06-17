// NOTE:
// SIMD paths:
// -   x86_64 Linux/macOS: SSE2 via <immintrin.h>
// -   ARM64  macOS:       NEON via <arm_neon.h>

#include "nolibc.h"

#include "ascii.h"
#include "platform.h"

#include <stdint.h>
#include <sys/inotify.h>

// Helpers
static inline uint8_t clamp_u8(int v) {
  return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}
static inline int my_abs(int x) { return x < 0 ? -x : x; }

static inline double my_sqrt(double x) {
  if (x <= 0.0)
    return 0.0;
  double guess = x;
  // initial halve the exponent by repeated division
  while (guess > 1.0) {
    guess /= 2.0;
  }
  if (guess <= 0.0)
    guess = 1.0;
  for (int i = 0; i < 12; i++) {
    guess = 0.5 * (guess + x / guess);
  }
  return guess;
}

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
    return 3 + (size_t)th * ((size_t)tw * per_cell + 5) + 1;
  }
  case RENDER_BLOCKS:
  case RENDER_ASCII_RAMP:
  case RENDER_DOTS: {
    int tw = braille_term_w;
    int th = braille_term_h;
    size_t per_cell = color ? 22 : 3; // worst case: 3-byte glyph
    return 3 + (size_t)th * ((size_t)tw * per_cell + 5) + 1;
  }
  case RENDER_BRAILLE:
  default: {
    int tw = braille_term_w;
    int th = braille_term_h;
    if (color)
      return 3 + (size_t)th * ((size_t)tw * 22 + 5) + 1;
    return 3 + (size_t)th * ((size_t)tw * 3 + 1) + 1;
  }
  }
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

static void sobel_dir(const uint8_t *in, uint8_t *out_mag, uint8_t *out_dir,
                      int w, int h) {
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
      out_mag[y * w + x] = (uint8_t)(mag > 255 ? 255 : mag);

      uint8_t dir;
      int agx = my_abs(gx), agy = my_abs(gy);
      if (agx == 0 && agy == 0) {
        dir = 0;
      } else if (agy * 5 < agx * 2) {
        dir = 2;
      } else if (agx * 5 < agy * 2) {
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

// Charset registry: hot-reloadable character ramps from a directory.
static void _trim_line(char *s) {
  // Cut at first newline/CR, then trim trailing whitespace.
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
  // strip extension
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
    if (fd < 0)
      continue;

    char buf[CHARSET_RAMP_LEN];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
      continue;
    buf[n] = '\0';
    _trim_line(buf);
    if (buf[0] == '\0')
      continue;

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
    if (dup)
      continue;

    _name_from_path(path, reg->sets[reg->count].name, CHARSET_NAME_LEN);
    nl_strncpy_safe(reg->sets[reg->count].ramp, buf, CHARSET_RAMP_LEN);
    reg->count++;
  }
}

void charset_registry_check_reload(charset_registry_t *reg) {
  if (reg->inotify_fd < 0)
    return;

  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  ssize_t n = read(reg->inotify_fd, buf, sizeof(buf));
  if (n <= 0)
    return;

  charset_registry_scan(reg);
}

const char *charset_registry_active_ramp(const charset_registry_t *reg) {
  if (reg->count == 0)
    return ASCII_CHARS_DEFAULT;
  int idx = reg->active;
  if (idx < 0 || idx >= reg->count)
    idx = 0;
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

static int emit_glyph(char *out, size_t out_size, int out_idx,
                      const char *glyph, int do_color, int r, int g, int b) {
  if (do_color) {
    int written = snprintf(out + out_idx, out_size - (size_t)out_idx,
                           "\033[38;2;%d;%d;%dm%s", r, g, b, glyph);
    if (written > 0 && (size_t)(out_idx + written) < out_size)
      out_idx += written;
  } else {
    size_t glen = nl_strlen(glyph);
    if ((size_t)(out_idx + (int)glen) < out_size) {
      nl_memcpy(out + out_idx, glyph, glen);
      out_idx += (int)glen;
    }
  }
  return out_idx;
}

int grayscale_to_ascii(const uint8_t *gray, const uint8_t *rgb, int src_w,
                       int src_h, int dst_w, int dst_h, char *out,
                       size_t out_size, const ascii_opts_t *opts) {
  int safe_dst_w = dst_w - (dst_w % 2);
  int safe_dst_h = dst_h - (dst_h % 4);

  int brightness = opts ? opts->brightness : 0;
  int contrast = opts ? opts->contrast : 100;
  int invert = opts ? opts->invert : 0;
  int do_color = opts && opts->color && rgb;
  edge_mode_t edge_mode = opts ? opts->edges : EDGE_OFF;
  int do_dither = opts ? opts->dither : 0;
  int thresh_limit = opts ? opts->threshold_val : 35;
  render_mode_t render_mode = opts ? opts->render_mode : RENDER_BRAILLE;
  const char *ramp = (opts && opts->charset && opts->charset[0])
                         ? opts->charset
                         : ASCII_CHARS_DEFAULT;
  int ramp_len = (int)nl_strlen(ramp);
  if (ramp_len < 1) {
    ramp = ASCII_CHARS_DEFAULT;
    ramp_len = (int)nl_strlen(ramp);
  }

  int depth_pop = opts ? opts->depth_pop : 0;
  int depth_invert = opts ? opts->depth_invert : 0;

  double bw = (double)src_w / safe_dst_w;
  double bh = (double)src_h / safe_dst_h;

  uint8_t *subpixel_g = malloc((size_t)(safe_dst_w * safe_dst_h));
  uint8_t *subpixel_rgb =
      do_color ? malloc((size_t)(safe_dst_w * safe_dst_h * 3)) : NULL;

  if (!subpixel_g || (do_color && !subpixel_rgb)) {
    free(subpixel_g);
    free(subpixel_rgb);
    return -1;
  }

  // Downscale and interpolate the high-res frames to subpixel boundaries
  for (int y = 0; y < safe_dst_h; y++) {
    int ys = (int)(y * bh), ye = (int)((y + 1) * bh);
    if (ye <= ys)
      ye = ys + 1;
    for (int x = 0; x < safe_dst_w; x++) {
      int xs = (int)(x * bw), xe = (int)((x + 1) * bw);
      if (xe <= xs)
        xe = xs + 1;

      long tg = 0, tr = 0, tgv = 0, tb = 0;
      int count = 0;
      for (int sy = ys; sy < ye && sy < src_h; sy++) {
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
      }
      if (!count)
        count = 1;

      int gv = (int)(tg / count);
      if (contrast != 100)
        gv = 128 + (gv - 128) * contrast / 100;
      gv += brightness;
      subpixel_g[y * safe_dst_w + x] = clamp_u8(gv);

      if (do_color) {
        uint8_t *op = subpixel_rgb + (y * safe_dst_w + x) * 3;
        op[0] = clamp_u8((int)(tr / count));
        op[1] = clamp_u8((int)(tgv / count));
        op[2] = clamp_u8((int)(tb / count));
      }
    }
  }

  // TEST:
  // Pseudo-3D "pop out" parallax effect.
  if (depth_pop > 0) {
    uint8_t *warped_g = malloc((size_t)(safe_dst_w * safe_dst_h));
    uint8_t *warped_rgb =
        do_color ? malloc((size_t)(safe_dst_w * safe_dst_h * 3)) : NULL;
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
          if (sx < 0)
            sx = 0;
          if (sx >= safe_dst_w)
            sx = safe_dst_w - 1;
          if (sy < 0)
            sy = 0;
          if (sy >= safe_dst_h)
            sy = safe_dst_h - 1;

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
      if (do_color)
        nl_memcpy(subpixel_rgb, warped_rgb,
                  (size_t)(safe_dst_w * safe_dst_h * 3));
    }
    free(warped_g);
    free(warped_rgb);
  }

  // Edge detection dispatch
  uint8_t *dir_buf = NULL;
  if (edge_mode != EDGE_OFF) {
    uint8_t *eb = calloc((size_t)(safe_dst_w * safe_dst_h), 1);
    if (eb) {
      switch (edge_mode) {
      case EDGE_SOBEL:
        sobel(subpixel_g, eb, safe_dst_w, safe_dst_h);
        break;
      case EDGE_SOBEL_DIR:
        dir_buf = calloc((size_t)(safe_dst_w * safe_dst_h), 1);
        if (dir_buf)
          sobel_dir(subpixel_g, eb, dir_buf, safe_dst_w, safe_dst_h);
        else
          sobel(subpixel_g, eb, safe_dst_w, safe_dst_h);
        break;
      case EDGE_LAPLACIAN:
        laplacian(subpixel_g, eb, safe_dst_w, safe_dst_h);
        break;
      default:
        break;
      }
      nl_memcpy(subpixel_g, eb, (size_t)(safe_dst_w * safe_dst_h));
      free(eb);
    }
  }

  // Floyd-Steinberg dithering on the subpixel grayscale buffer
  if (do_dither) {
    int16_t *err = calloc((size_t)(safe_dst_w * safe_dst_h), sizeof(int16_t));
    if (err) {
      for (int i = 0; i < safe_dst_w * safe_dst_h; i++)
        err[i] = (int16_t)subpixel_g[i];

      for (int y = 0; y < safe_dst_h; y++) {
        for (int x = 0; x < safe_dst_w; x++) {
          int idx = y * safe_dst_w + x;
          int16_t old = err[idx];
          int16_t nval = (old > (int16_t)thresh_limit) ? 255 : 0;
          int16_t qerr = old - nval;
          err[idx] = nval;

          if (x + 1 < safe_dst_w)
            err[idx + 1] += (qerr * 7) >> 4;
          if (y + 1 < safe_dst_h) {
            if (x > 0)
              err[idx + safe_dst_w - 1] += (qerr * 3) >> 4;
            err[idx + safe_dst_w] += (qerr * 5) >> 4;
            if (x + 1 < safe_dst_w)
              err[idx + safe_dst_w + 1] += (qerr * 1) >> 4;
          }
        }
      }

      for (int i = 0; i < safe_dst_w * safe_dst_h; i++) {
        int16_t v = err[i];
        subpixel_g[i] = (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
      }
      free(err);
    }
  }

  int out_idx = 0;
  static const char HOME[] = "\033[H";
  if (out_size > sizeof(HOME)) {
    nl_memcpy(out, HOME, sizeof(HOME) - 1);
    out_idx = sizeof(HOME) - 1;
  }

  if (render_mode == RENDER_HALF_BLOCK) {
    int term_w = safe_dst_w;
    int term_h = safe_dst_h / 2;
    for (int ty = 0; ty < term_h; ty++) {
      for (int tx = 0; tx < term_w; tx++) {
        int top_idx = (ty * 2) * safe_dst_w + tx;
        int bot_idx = (ty * 2 + 1) * safe_dst_w + tx;
        uint8_t top_l = subpixel_g[top_idx];
        uint8_t bot_l = subpixel_g[bot_idx];

        if (do_color) {
          const uint8_t *tp = subpixel_rgb + top_idx * 3;
          const uint8_t *bp = subpixel_rgb + bot_idx * 3;
          int written =
              snprintf(out + out_idx, out_size - (size_t)out_idx,
                       "\033[38;2;%d;%d;%dm\033[48;2;%d;%d;%dm%s", tp[0], tp[1],
                       tp[2], bp[0], bp[1], bp[2], HALF_BLOCK_UTF8);
          if (written > 0 && (size_t)(out_idx + written) < out_size)
            out_idx += written;
        } else {
          int active = (top_l > thresh_limit) || (bot_l > thresh_limit);
          if (invert)
            active = !active;
          const char *glyph = active ? "\xe2\x96\x88" : " ";
          out_idx = emit_glyph(out, out_size, out_idx, glyph, 0, 0, 0, 0);
        }
      }
      if (do_color) {
        int written =
            snprintf(out + out_idx, out_size - (size_t)out_idx, "\033[0m\n");
        if (written > 0 && (size_t)(out_idx + written) < out_size)
          out_idx += written;
      } else {
        if ((size_t)(out_idx + 1) < out_size)
          out[out_idx++] = '\n';
      }
    }
    out[(size_t)out_idx < out_size ? (size_t)out_idx : out_size - 1] = '\0';
    free(subpixel_g);
    free(subpixel_rgb);
    free(dir_buf);
    return out_idx;
  }

  // BRAILLE / BLOCKS / ASCII_RAMP / DOTS
  int term_w = safe_dst_w / 2;
  int term_h = safe_dst_h / 4;

  for (int ty = 0; ty < term_h; ty++) {
    for (int tx = 0; tx < term_w; tx++) {
      uint8_t braille_offset = 0;
      long sum_r = 0, sum_g = 0, sum_b = 0;
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
          if (invert)
            is_active = !is_active;

          if (is_active) {
            braille_offset |= get_braille_bitmask(dx, dy);
            active_count++;
            if (dir_buf)
              dom_dir_votes[dir_buf[pixel_idx]]++;
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
        for (int d = 1; d < 4; d++)
          if (dom_dir_votes[d] > dom_dir_votes[best])
            best = d;
        dom_dir = (uint8_t)best;
      }

      int avg_r = 0, avg_g = 0, avg_b = 0;
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
        out_idx = emit_glyph(out, out_size, out_idx, utf8_seq, do_color, avg_r,
                             avg_g, avg_b);
        break;
      }
      case RENDER_BLOCKS: {
        int shade_idx = avg_luma * BLOCK_SHADE_COUNT / 256;
        if (shade_idx >= BLOCK_SHADE_COUNT)
          shade_idx = BLOCK_SHADE_COUNT - 1;
        if (invert)
          shade_idx = BLOCK_SHADE_COUNT - 1 - shade_idx;
        out_idx =
            emit_glyph(out, out_size, out_idx, BLOCK_SHADE_UTF8[shade_idx],
                       do_color, avg_r, avg_g, avg_b);
        break;
      }
      case RENDER_DOTS: {
        int dot_idx = avg_luma * DOT_COUNT / 256;
        if (dot_idx >= DOT_COUNT)
          dot_idx = DOT_COUNT - 1;
        if (invert)
          dot_idx = DOT_COUNT - 1 - dot_idx;
        out_idx = emit_glyph(out, out_size, out_idx, DOT_UTF8[dot_idx],
                             do_color, avg_r, avg_g, avg_b);
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
          if (ramp_idx >= ramp_len)
            ramp_idx = ramp_len - 1;
          if (invert)
            ramp_idx = ramp_len - 1 - ramp_idx;
          glyph[0] = ramp[ramp_idx];
        }
        out_idx = emit_glyph(out, out_size, out_idx, glyph, do_color, avg_r,
                             avg_g, avg_b);
        break;
      }
      }
    }

    if (do_color) {
      int written =
          snprintf(out + out_idx, out_size - (size_t)out_idx, "\033[0m\n");
      if (written > 0 && (size_t)(out_idx + written) < out_size)
        out_idx += written;
    } else {
      if ((size_t)(out_idx + 1) < out_size)
        out[out_idx++] = '\n';
    }
  }

  out[(size_t)out_idx < out_size ? (size_t)out_idx : out_size - 1] = '\0';

  free(subpixel_g);
  free(subpixel_rgb);
  free(dir_buf);
  return out_idx;
}

// FPS Overlay Configuration
void overlay_fps_box(int dst_w, double fps, int color_enabled) {
  char buf[80];
  int term_w = dst_w / 2;
  int col = (term_w - 13) / 2 + 1;
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
