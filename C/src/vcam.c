/*
 * virtual camera
 */

#include "platform.h"

#include "nolibc.h"

#include "vcam.h"

#if defined(__LINUX_NOLIBC__)
#include "nl_v4l2.h"
#endif

#include "vcam_font.inc" // vcam_font[95][16], ASCII 32..126

#define DEF_FG 204
#define DEF_BG 0

static const uint8_t ansi16[16][3] = {
    {0, 0, 0},       {205, 49, 49},   {13, 188, 121}, {229, 229, 16},
    {36, 114, 200},  {188, 63, 188},  {17, 168, 205}, {204, 204, 204},
    {102, 102, 102}, {241, 76, 76},   {35, 209, 139}, {245, 245, 67},
    {59, 142, 234},  {214, 112, 214}, {41, 184, 219}, {255, 255, 255}};

static void xterm256(int n, uint8_t out[3]) {
  if (n < 16) {
    out[0] = ansi16[n][0];
    out[1] = ansi16[n][1];
    out[2] = ansi16[n][2];
  } else if (n < 232) {
    static const uint8_t lv[6] = {0, 95, 135, 175, 215, 255};
    int k = n - 16;
    out[0] = lv[k / 36];
    out[1] = lv[(k / 6) % 6];
    out[2] = lv[k % 6];
  } else {
    int g = 8 + (n - 232) * 10;
    out[0] = out[1] = out[2] = (uint8_t)g;
  }
}

static uint8_t clamp8(int v) {
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

int vcam_init(vcam_t *v, int w, int h) {
  v->fd = -1;
  v->is_video = 0;
  v->frames = 0;
  v->unknown = 0;
  v->rgb = NULL;
  v->yuyv = NULL;
  v->cells = NULL;
  if (w < VCAM_CELL_W || h < VCAM_CELL_H || w % 2 != 0 || w > 8192 ||
      h > 8192) {
    return -1;
  }

  v->w = w;
  v->h = h;
  v->cols = w / VCAM_CELL_W;
  v->rows = h / VCAM_CELL_H;
  v->rgb = nl_calloc((size_t)w * (size_t)h, 3);
  v->yuyv = nl_calloc((size_t)w * (size_t)h, 2);
  v->cells = nl_calloc((size_t)v->cols * (size_t)v->rows, sizeof(vcam_cell_t));
  if (!v->rgb || !v->yuyv || !v->cells) {
    vcam_close(v);

    return -1;
  }

  return 0;
}

int vcam_open(vcam_t *v, const char *path) {
  int fd = open(
      path, (int)((unsigned)O_WRONLY | (unsigned)O_CREAT | (unsigned)O_TRUNC),
      0644);
  if (fd < 0) {
    return -1;
  }

  v->fd = fd;
  v->is_video = 0;
#if defined(__LINUX_NOLIBC__)
  struct v4l2_format f;
  nl_memset(&f, 0, sizeof(f));
  f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  f.fmt.pix.width = (uint32_t)v->w;
  f.fmt.pix.height = (uint32_t)v->h;
  f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; // NOLINT(bugprone-signed-bitwise)
  f.fmt.pix.field = V4L2_FIELD_NONE;
  f.fmt.pix.bytesperline = (uint32_t)v->w * 2;
  f.fmt.pix.sizeimage = (uint32_t)v->w * (uint32_t)v->h * 2;
  f.fmt.pix.colorspace = 1; // V4L2_COLORSPACE_SMPTE170M, matches BT.601
  if (nl_ioctl(fd, VIDIOC_S_FMT, &f) == 0) { // NOLINT(bugprone-signed-bitwise)
    v->is_video = 1;
  }
#endif

  return 0;
}

// Parsing
typedef struct {
  int col, row;
  int minc, minr, maxc, maxr;
  uint8_t fg[3], bg[3];
  int bold, rev;
  int ox, oy; // subtracted from col,row when drawing
} pstate_t;

static void reset_sgr(pstate_t *s) {
  for (int i = 0; i < 3; i++) {
    s->fg[i] = DEF_FG;
    s->bg[i] = DEF_BG;
  }

  s->bold = 0;
  s->rev = 0;
}

static void sgr(pstate_t *s, const int *p, int n) {
  if (n == 0) {
    reset_sgr(s);

    return;
  }

  for (int i = 0; i < n; i++) {
    int c = p[i];
    if (c == 0) {
      reset_sgr(s);
    } else if (c == 1) {
      s->bold = 1;
    } else if (c == 22) {
      s->bold = 0;
    } else if (c == 7) {
      s->rev = 1;
    } else if (c == 27) {
      s->rev = 0;
    } else if ((c >= 30 && c <= 37) || (c >= 90 && c <= 97)) {
      xterm256(c < 90 ? c - 30 : c - 90 + 8, s->fg);
    } else if ((c >= 40 && c <= 47) || (c >= 100 && c <= 107)) {
      xterm256(c < 100 ? c - 40 : c - 100 + 8, s->bg);
    } else if (c == 39) {
      s->fg[0] = s->fg[1] = s->fg[2] = DEF_FG;
    } else if (c == 49) {
      s->bg[0] = s->bg[1] = s->bg[2] = DEF_BG;
    } else if (c == 38 || c == 48) {
      uint8_t *dst = c == 38 ? s->fg : s->bg;
      if (i + 4 < n && p[i + 1] == 2) {
        dst[0] = clamp8(p[i + 2]);
        dst[1] = clamp8(p[i + 3]);
        dst[2] = clamp8(p[i + 4]);
        i += 4;
      } else if (i + 2 < n && p[i + 1] == 5) {
        xterm256(p[i + 2] % 256, dst);
        i += 2;
      } else {
        break; // malformed: ignore rest
      }
    }
  }
}

static void put_cell(vcam_t *v, pstate_t *s, uint32_t cp, int draw) {
  if (!draw) {
    if (s->col < s->minc) {
      s->minc = s->col;
    }
    if (s->col > s->maxc) {
      s->maxc = s->col;
    }
    if (s->row < s->minr) {
      s->minr = s->row;
    }
    if (s->row > s->maxr) {
      s->maxr = s->row;
    }

    return;
  }

  int x = s->col - s->ox, y = s->row - s->oy;
  if (x < 0 || y < 0 || x >= v->cols || y >= v->rows) {
    return;
  }

  vcam_cell_t *c = &v->cells[y * v->cols + x];
  const uint8_t *f = s->rev ? s->bg : s->fg;
  const uint8_t *b = s->rev ? s->fg : s->bg;
  c->cp = cp == ' ' ? 0 : cp;
  for (int i = 0; i < 3; i++) {
    c->fg[i] = f[i];
    c->bg[i] = b[i];
  }

  c->bold = (uint8_t)s->bold;
}

// One pass over stream. draw = 0: only measure extent of cells
static void parse(vcam_t *v, pstate_t *s, const char *a, size_t n, int draw) {
  size_t i = 0;
  while (i < n) {
    unsigned char ch = (unsigned char)a[i];
    if (ch == 0x1b) {
      if (i + 1 < n && a[i + 1] == '[') {
        i += 2;
        int p[16], np = 0, cur = -1, priv = 0;
        while (i < n && (unsigned char)a[i] < 0x40) {
          unsigned char q = (unsigned char)a[i++];
          if (q >= '0' && q <= '9') {
            cur = (cur < 0 ? 0 : cur) * 10 + (q - '0');
            if (cur > 100000) {
              cur = 100000;
            }
          } else if (q == ';' || q == ':') {
            if (np < 16) {
              p[np++] = cur < 0 ? 0 : cur;
            }

            cur = -1;
          } else if (q == '?') {
            priv = 1;
          }
        }

        if (cur >= 0 || np > 0) {
          if (np < 16) {
            p[np++] = cur < 0 ? 0 : cur;
          }
        }

        if (i >= n) {
          return;
        }

        unsigned char fin = (unsigned char)a[i++];
        if (priv) {
          continue;
        }

        if (fin == 'm') {
          sgr(s, p, np);
        } else if (fin == 'H' || fin == 'f') {
          int r = np > 0 && p[0] > 0 ? p[0] : 1;
          int c = np > 1 && p[1] > 0 ? p[1] : 1;
          s->row = r - 1;
          s->col = c - 1;
        } else if (fin == 'J' && draw && np > 0 && p[0] == 2) {
          nl_memset(v->cells, 0,
                    (size_t)v->cols * (size_t)v->rows * sizeof(vcam_cell_t));
        }
      } else {
        i += 2; // ESC and one more byte
      }

      continue;
    }

    if (ch == '\r') {
      s->col = 0;
      i++;
    } else if (ch == '\n') {
      // main writes bare \n and relies on tty turning it into CR LF
      s->col = 0;
      s->row++;
      i++;
    } else if (ch < 0x20 || ch == 0x7f) {
      i++;
    } else {
      uint32_t cp = ch;
      size_t len = 1;
      if (ch >= 0xf0) {
        len = 4;
        cp = ch & 7u;
      } else if (ch >= 0xe0) {
        len = 3;
        cp = ch & 15u;
      } else if (ch >= 0xc0) {
        len = 2;
        cp = ch & 31u;
      }

      if (len > 1) {
        if (i + len > n) {
          return;
        }

        for (size_t k = 1; k < len; k++) {
          cp = cp * 64u + ((unsigned)(unsigned char)a[i + k] % 64u);
        }
      }

      put_cell(v, s, cp, draw);
      s->col++;
      i += len;
    }
  }
}

// Rasteriser
static int known_glyph(uint32_t cp) {
  return (cp >= 32 && cp <= 126) || (cp >= 0x2800 && cp <= 0x28ff) ||
         (cp >= 0x2580 && cp <= 0x259f) || cp == 0xb7 || cp == 0x2022 ||
         cp == 0x25cf;
}

// 0..255 coverage of pixel (x,y) of a cell by glyph cp
static int coverage(vcam_t *v, uint32_t cp, int bold, int x, int y) {
  if (cp >= 32 && cp <= 126) {
    const unsigned char *g = vcam_font[cp - 32];
    unsigned on = ((unsigned)g[y] >> (unsigned)(7 - x)) & 1u;
    if (bold && x > 0) {
      on |= ((unsigned)g[y] >> (unsigned)(8 - x)) & 1u;
    }

    return on ? 255 : 0;
  }

  if (cp >= 0x2800 && cp <= 0x28ff) {
    // dot n of pattern: columns of 4 px, rows of 4 px, 3x3 px dots
    static const uint8_t bit[4][2] = {{0, 3}, {1, 4}, {2, 5}, {6, 7}};
    int dx = x % 4, dy = y % 4;
    if (dx == 3 || dy == 3) {
      return 0;
    }

    return ((cp >> (unsigned)bit[y / 4][x / 4]) & 1u) ? 255 : 0;
  }

  if (cp >= 0x2580 && cp <= 0x259f) {
    int k = (int)cp - 0x2580;
    if (k == 0) {
      return y < 8 ? 255 : 0; // upper half
    }
    if (k >= 1 && k <= 8) {
      return y >= 16 - 2 * k ? 255 : 0; // lower k/8
    }
    if (k >= 9 && k <= 15) {
      return x < 8 - (k - 8) ? 255 : 0; // left (16-k)/8
    }
    if (k == 16) {
      return x >= 4 ? 255 : 0; // right half
    }
    if (k >= 17 && k <= 19) {
      return 64 * (k - 16); // light, medium, dark shade
    }
    if (k == 20) {
      return y < 2 ? 255 : 0; // upper eighth
    }
    if (k == 21) {
      return x >= 7 ? 255 : 0; // right eighth
    }

    // quadrants U+2596..259F: bits UL UR LL LR
    static const uint8_t quad[10] = {0x4, 0x8, 0x1, 0xd, 0x9,
                                     0x7, 0xb, 0x2, 0x6, 0xe};
    int q = quad[k - 22];
    int bitn = (y >= 8 ? 2 : 0) + (x >= 4 ? 1 : 0);
    // 0=UL 1=UR 2=LL 3=LR
    return (((unsigned)q >> (unsigned)bitn) & 1u) ? 255 : 0;
  }

  if (cp == 0xb7 || cp == 0x2022 || cp == 0x25cf) {
    // middle dot, bullet, black circle: discs of radius 1.5, 2.5, 3.5 px
    // (distances in half pixels from cell centre, no sqrt)
    int dx = 2 * x + 1 - 8, dy = 2 * y + 1 - 16;
    int r2 = cp == 0xb7 ? 9 : cp == 0x2022 ? 25 : 49;

    return dx * dx + dy * dy <= r2 ? 255 : 0;
  }

  // anything else: hollow box so it is visible that a glyph is missing
  (void)v;

  return (x == 1 || x == 6 || y == 2 || y == 13) && x >= 1 && x <= 6 &&
                 y >= 2 && y <= 13
             ? 255
             : 0;
}

static void draw_cell(vcam_t *v, int cx, int cy) {
  const vcam_cell_t *c = &v->cells[cy * v->cols + cx];
  if (c->cp && !known_glyph(c->cp)) {
    v->unknown++;
  }

  for (int y = 0; y < VCAM_CELL_H; y++) {
    uint8_t *px = v->rgb + (((size_t)(cy * VCAM_CELL_H + y) * (size_t)v->w) +
                            (size_t)(cx * VCAM_CELL_W)) *
                               3;
    for (int x = 0; x < VCAM_CELL_W; x++) {
      int a = c->cp ? coverage(v, c->cp, c->bold, x, y) : 0;
      for (int k = 0; k < 3; k++) {
        px[x * 3 + k] =
            (uint8_t)((c->fg[k] * a + c->bg[k] * (255 - a) + 127) / 255);
      }
    }
  }
}

void vcam_render(vcam_t *v, const char *ansi, size_t len) {
  pstate_t s;
  nl_memset(&s, 0, sizeof(s));
  s.minc = s.minr = 1073741824;
  s.maxc = s.maxr = -1;
  reset_sgr(&s);
  parse(v, &s, ansi, len, 0);

  nl_memset(v->cells, 0,
            (size_t)v->cols * (size_t)v->rows * sizeof(vcam_cell_t));
  nl_memset(v->rgb, 0, (size_t)v->w * (size_t)v->h * 3);
  for (int i = 0; i < v->cols * v->rows; i++) {
    for (int k = 0; k < 3; k++) {
      v->cells[i].fg[k] = DEF_FG;
      v->cells[i].bg[k] = DEF_BG;
    }
  }

  if (s.maxc >= 0) {
    int fw = s.maxc - s.minc + 1, fh = s.maxr - s.minr + 1;
    pstate_t d;
    nl_memset(&d, 0, sizeof(d));
    reset_sgr(&d);
    d.ox = s.minc - (fw < v->cols ? (v->cols - fw) / 2 : 0);
    d.oy = s.minr - (fh < v->rows ? (v->rows - fh) / 2 : 0);
    parse(v, &d, ansi, len, 1);
  }

  for (int cy = 0; cy < v->rows; cy++) {
    for (int cx = 0; cx < v->cols; cx++) {
      draw_cell(v, cx, cy);
    }
  }
}

const uint8_t *vcam_pixels(const vcam_t *v) { return v->rgb; }

void vcam_to_yuyv(vcam_t *v) {
  const uint8_t *s = v->rgb;
  uint8_t *d = v->yuyv;
  size_t pairs = (size_t)v->w * (size_t)v->h / 2;
  for (size_t i = 0; i < pairs; i++, s += 6, d += 4) {
    int r0 = s[0], g0 = s[1], b0 = s[2];
    int r1 = s[3], g1 = s[4], b1 = s[5];
    int r = (r0 + r1) / 2, g = (g0 + g1) / 2, b = (b0 + b1) / 2;
    d[0] = (uint8_t)((16 * 256 + 128 + 66 * r0 + 129 * g0 + 25 * b0) / 256);
    d[1] = (uint8_t)((128 * 256 + 128 - 38 * r - 74 * g + 112 * b) / 256);
    d[2] = (uint8_t)((16 * 256 + 128 + 66 * r1 + 129 * g1 + 25 * b1) / 256);
    d[3] = (uint8_t)((128 * 256 + 128 + 112 * r - 94 * g - 18 * b) / 256);
  }
}

int vcam_frame(vcam_t *v, const char *ansi, size_t len) {
  vcam_render(v, ansi, len);
  vcam_to_yuyv(v);
  v->frames++;
  if (v->fd < 0) {
    return 0;
  }

  const uint8_t *p = v->yuyv;
  size_t left = (size_t)v->w * (size_t)v->h * 2;
  while (left > 0) {
    ssize_t w = write(v->fd, p, left);
    if (w <= 0) {
      return -1;
    }

    p += w;
    left -= (size_t)w;
  }

  return 0;
}

void vcam_close(vcam_t *v) {
  if (v->fd >= 0) {
    close(v->fd);
    v->fd = -1;
  }

  nl_free(v->rgb);
  nl_free(v->yuyv);
  nl_free(v->cells);
  v->rgb = NULL;
  v->yuyv = NULL;
  v->cells = NULL;
}
