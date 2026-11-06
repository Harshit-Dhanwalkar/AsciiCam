#include "nolibc.h"

#include "gfx.h"

// Small helpers
typedef struct {
  char *p;
  size_t n, cap;
  int bad;
} wr_t;

static void put(wr_t *w, char c) {
  if (w->n < w->cap) {
    w->p[w->n++] = c;
  } else {
    w->bad = 1;
  }
}

static void puts_(wr_t *w, const char *s) {
  while (*s) {
    put(w, *s++);
  }
}

static void putnum(wr_t *w, unsigned v) {
  char t[12];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v);

  while (n) {
    put(w, t[--n]);
  }
}

static int has(const char *hay, const char *needle) {
  for (; *hay; hay++) {
    size_t k = 0;
    while (needle[k] && hay[k] == needle[k]) {
      k++;
    }

    if (!needle[k]) {
      return 1;
    }
  }

  return 0;
}

int gfx_parse(const char *name, const char *term, const char *term_program,
              int have_kitty_window_id) {
  if (!name || nl_strcmp(name, "off") == 0) {
    return GFX_OFF;
  }

  if (nl_strcmp(name, "kitty") == 0) {
    return GFX_KITTY;
  }

  if (nl_strcmp(name, "sixel") == 0) {
    return GFX_SIXEL;
  }

  if (nl_strcmp(name, "auto") != 0) {
    return -1;
  }

  if (have_kitty_window_id) {
    return GFX_KITTY;
  }

  if (term && (has(term, "kitty") || has(term, "ghostty"))) {
    return GFX_KITTY;
  }

  if (term_program &&
      (has(term_program, "WezTerm") || has(term_program, "ghostty") ||
       has(term_program, "Ghostty"))) {
    return GFX_KITTY;
  }

  if (term && (has(term, "foot") || has(term, "mlterm") || has(term, "sixel") ||
               has(term, "contour") || has(term, "yaft"))) {
    return GFX_SIXEL;
  }

  return GFX_OFF;
}

// Resampling
static int rs_lo[GFX_MAX_W + 1];

int gfx_resample(const uint8_t *gray, const uint8_t *rgb, int sw, int sh,
                 int dw, int dh, uint8_t *dst) {
  if ((!gray && !rgb) || !dst || sw < 1 || sh < 1 || dw < 1 || dh < 1 ||
      sw > GFX_MAX_W || sh > GFX_MAX_H || dw > GFX_MAX_W || dh > GFX_MAX_H) {
    return -1;
  }

  for (int x = 0; x <= dw; x++) {
    rs_lo[x] = (int)((long)x * sw / dw);
  }

  for (int y = 0; y < dh; y++) {
    int y0 = (int)((long)y * sh / dh);
    int y1 = (int)((long)(y + 1) * sh / dh);
    if (y1 <= y0) {
      y1 = y0 + 1;
    }

    for (int x = 0; x < dw; x++) {
      int x0 = rs_lo[x];
      int x1 = rs_lo[x + 1];
      if (x1 <= x0) {
        x1 = x0 + 1;
      }

      unsigned long r = 0, g = 0, b = 0;
      unsigned cnt = (unsigned)((x1 - x0) * (y1 - y0));
      for (int sy = y0; sy < y1; sy++) {
        if (rgb) {
          const uint8_t *s = rgb + ((size_t)sy * (size_t)sw + (size_t)x0) * 3u;
          for (int sx = x0; sx < x1; sx++, s += 3) {
            r += s[0];
            g += s[1];
            b += s[2];
          }
        } else {
          const uint8_t *s = gray + (size_t)sy * (size_t)sw + (size_t)x0;
          for (int sx = x0; sx < x1; sx++, s++) {
            r += *s;
          }
        }
      }

      uint8_t *d = dst + ((size_t)y * (size_t)dw + (size_t)x) * 3u;
      if (rgb) {
        d[0] = (uint8_t)((r + cnt / 2u) / cnt);
        d[1] = (uint8_t)((g + cnt / 2u) / cnt);
        d[2] = (uint8_t)((b + cnt / 2u) / cnt);
      } else {
        d[0] = d[1] = d[2] = (uint8_t)((r + cnt / 2u) / cnt);
      }
    }
  }

  return 0;
}

// Kitty
#define KITTY_CHUNK 4096 // base64 characters per escape, protocol maximum

size_t gfx_kitty_cap(int w, int h) {
  size_t raw = (size_t)w * (size_t)h * 3u;
  size_t b64 = (raw + 2u) / 3u * 4u;

  return b64 + (b64 / KITTY_CHUNK + 2u) * 16u + 96u;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int gfx_kitty_encode(const uint8_t *rgb, int w, int h, int cols, int rows,
                     char *out, size_t cap) {
  if (!rgb || !out || w < 1 || h < 1 || w > GFX_MAX_W || h > GFX_MAX_H ||
      cols < 1 || rows < 1) {
    return -1;
  }

  wr_t o = {out, 0, cap, 0};
  size_t raw = (size_t)w * (size_t)h * 3u;
  size_t i = 0;
  int first = 1;

  while (i < raw || first) {
    // Up to KITTY_CHUNK base64 characters = 3072 source bytes per escape
    size_t take = raw - i;
    if (take > KITTY_CHUNK / 4u * 3u) {
      take = KITTY_CHUNK / 4u * 3u;
    }

    int more = (i + take) < raw;
    puts_(&o, "\033_G");
    if (first) {
      puts_(&o, "a=T,f=24,i=1,p=1,q=2,C=1,z=-1,s=");
      putnum(&o, (unsigned)w);
      puts_(&o, ",v=");
      putnum(&o, (unsigned)h);
      puts_(&o, ",c=");
      putnum(&o, (unsigned)cols);
      puts_(&o, ",r=");
      putnum(&o, (unsigned)rows);
      puts_(&o, ",m=");
    } else {
      puts_(&o, "q=2,m=");
    }

    put(&o, more ? '1' : '0');
    put(&o, ';');

    for (size_t k = 0; k < take; k += 3) {
      unsigned v = (unsigned)rgb[i + k] << 16u;
      size_t left = take - k;
      if (left > 1) {
        v |= (unsigned)rgb[i + k + 1] << 8u;
      }

      if (left > 2) {
        v |= rgb[i + k + 2];
      }

      put(&o, B64[(v >> 18u) & 63u]);
      put(&o, B64[(v >> 12u) & 63u]);
      // put(&o, left > 1 ? B64[(v >> 6u) & 63u] : '=');
      // put(&o, left > 2 ? B64[v & 63u] : '=');
      if (left > 1) {
        put(&o, B64[(v >> 6u) & 63u]);
      } else {
        put(&o, '=');
      }
      if (left > 2) {
        put(&o, B64[v & 63u]);
      } else {
        put(&o, '=');
      }
    }

    puts_(&o, "\033\\");
    i += take;
    first = 0;
  }

  return o.bad ? -1 : (int)o.n;
}

int gfx_kitty_delete(char *out, size_t cap) {
  static const char D[] = "\033_Ga=d,d=I,i=1,q=2\033\\";
  if (!out || cap < sizeof(D) - 1) {
    return -1;
  }

  nl_memcpy(out, D, sizeof(D) - 1);

  return (int)(sizeof(D) - 1);
}

// Sixel
#define SIX_COLORS 216
#define SIX_GREYS 64

// bits[c][x]: which of six rows of current band use colour c at x
static uint8_t six_bits[SIX_COLORS][GFX_MAX_W];
static uint8_t six_used[SIX_COLORS]; // defined in palette of this picture

size_t gfx_sixel_cap(int w, int h) {
  // Worst case is noise: every column of a band holds six different colours,
  // each switched on separately between run-length coded gaps
  return (size_t)w * (size_t)h * 6u + ((size_t)h / 6u + 1u) * 2600u + 16384u;
}

static void emit_run(wr_t *o, int ch, int run) {
  if (run > 3) {
    put(o, '!');
    putnum(o, (unsigned)run);
    put(o, (char)ch);
  } else {
    for (int k = 0; k < run; k++) {
      put(o, (char)ch);
    }
  }
}

static unsigned pct(unsigned v, unsigned max) {
  return (v * 100u + max / 2u) / max;
}

int gfx_sixel_encode(const uint8_t *rgb, int w, int h, int mono, char *out,
                     size_t cap) {
  if (!rgb || !out || w < 1 || h < 1 || w > GFX_MAX_W || h > GFX_MAX_H) {
    return -1;
  }

  int ncol = mono ? SIX_GREYS : SIX_COLORS;
  wr_t o = {out, 0, cap, 0};

  // DCS q, 1:1 pixel aspect, raster attributes
  puts_(&o, "\033P0;1;q\"1;1;");
  putnum(&o, (unsigned)w);
  put(&o, ';');
  putnum(&o, (unsigned)h);

  // palette: every colour that occurs somewhere in picture
  nl_memset(six_used, 0, sizeof(six_used));
  for (int y = 0; y < h; y++) {
    const uint8_t *s = rgb + (size_t)y * (size_t)w * 3u;
    for (int x = 0; x < w; x++, s += 3) {
      unsigned c;
      if (mono) {
        c = (unsigned)s[0] >> 2u;
      } else {
        c = ((s[0] * 5u + 127u) / 255u) * 36u +
            ((s[1] * 5u + 127u) / 255u) * 6u + ((s[2] * 5u + 127u) / 255u);
      }

      six_used[c] = 1;
    }
  }

  for (int c = 0; c < ncol; c++) {
    if (!six_used[c]) {
      continue;
    }

    put(&o, '#');
    putnum(&o, (unsigned)c);
    puts_(&o, ";2;");
    if (mono) {
      unsigned p = pct((unsigned)c, SIX_GREYS - 1u);
      putnum(&o, p);
      put(&o, ';');
      putnum(&o, p);
      put(&o, ';');
      putnum(&o, p);
    } else {
      putnum(&o, pct((unsigned)c / 36u, 5u));
      put(&o, ';');
      putnum(&o, pct((unsigned)c / 6u % 6u, 5u));
      put(&o, ';');
      putnum(&o, pct((unsigned)c % 6u, 5u));
    }
  }

  for (int band = 0; band < h; band += 6) {
    int rows = h - band < 6 ? h - band : 6;
    uint8_t present[SIX_COLORS];
    nl_memset(present, 0, (size_t)ncol);

    for (int r = 0; r < rows; r++) {
      const uint8_t *s = rgb + (size_t)(band + r) * (size_t)w * 3u;
      for (int x = 0; x < w; x++, s += 3) {
        unsigned c;
        if (mono) {
          c = (unsigned)s[0] >> 2u;
        } else {
          c = ((s[0] * 5u + 127u) / 255u) * 36u +
              ((s[1] * 5u + 127u) / 255u) * 6u + ((s[2] * 5u + 127u) / 255u);
        }

        if (!present[c]) {
          present[c] = 1;
          nl_memset(six_bits[c], 0, (size_t)w);
        }

        six_bits[c][x] =
            (uint8_t)((unsigned)six_bits[c][x] | (1u << (unsigned)r));
      }
    }

    int emitted = 0;
    for (int c = 0; c < ncol; c++) {
      if (!present[c]) {
        continue;
      }

      if (emitted) {
        put(&o, '$'); // back to left edge, same band
      }

      emitted = 1;
      put(&o, '#');
      putnum(&o, (unsigned)c);

      int run = 0;
      int prev = -1;
      for (int x = 0; x < w; x++) {
        int ch = 63 + six_bits[c][x];
        if (ch == prev) {
          run++;
        } else {
          if (run) {
            emit_run(&o, prev, run);
          }

          prev = ch;
          run = 1;
        }
      }

      // Trailing empty columns need not be sent
      if (run && prev != 63) {
        emit_run(&o, prev, run);
      }
    }

    put(&o, '-'); // next band
    if (o.bad) {
      return -1;
    }
  }

  puts_(&o, "\033\\");

  return o.bad ? -1 : (int)o.n;
}
