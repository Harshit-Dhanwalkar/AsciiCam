/*
 * src/cellcodec.c - grid of cells and its wire form, see include/cellcodec.h
 */

#include "platform.h"

#include "nolibc.h"

#include "cellcodec.h"

#define F_KEY 1u
#define F_MONO 2u
#define HDR 5

int cc_grid_size(cc_grid_t *g, int cols, int rows) {
  if (cols < 1 || rows < 1 || cols > CC_MAX_COLS || rows > CC_MAX_ROWS) {
    return -1;
  }

  if (g->cell && g->cols == cols && g->rows == rows) {
    // Same size: only blank it
  } else {
    nl_free(g->cell);
    g->cell = nl_malloc((size_t)cols * (size_t)rows * sizeof(netcell_t));
    if (!g->cell) {
      g->cols = g->rows = 0;

      return -1;
    }
  }

  g->cols = cols;
  g->rows = rows;
  for (int i = 0; i < cols * rows; i++) {
    g->cell[i] = (netcell_t){' ', 0, 0};
  }

  return 0;
}

void cc_grid_free(cc_grid_t *g) {
  nl_free(g->cell);
  g->cell = NULL;
  g->cols = g->rows = 0;
}

// Palette colours (kind 2) are not carried
static int plain_colour(uint32_t c) { return c == 0 || (c >> 24u) == 1u; }

int cc_grid_from_ansi(cc_grid_t *g, const char *ansi, size_t n) {
  int rows = 0;
  int cols = 0;
  if (netenc_extent(ansi, n, &rows, &cols) != 0 || rows < 1 || cols < 1 ||
      rows > CC_MAX_ROWS || cols > CC_MAX_COLS) {
    return -1;
  }

  if (cc_grid_size(g, cols, rows) != 0) {
    return -1;
  }

  uint8_t *seen = nl_malloc((size_t)cols * (size_t)rows);
  if (!seen) {
    return -1;
  }

  nl_memset(seen, 0, (size_t)cols * (size_t)rows);
  int rc = netenc_parse(ansi, n, g->cell, seen, rows, cols);
  nl_free(seen);
  if (rc != 0) {
    return -1;
  }

  for (int i = 0; i < cols * rows; i++) {
    if (!plain_colour(g->cell[i].fg) || !plain_colour(g->cell[i].bg)) {
      return -1;
    }
  }

  return 0;
}

static int same_cell(const netcell_t *a, const netcell_t *b) {
  return a->glyph == b->glyph && a->fg == b->fg && a->bg == b->bg;
}

static int glyph_bytes(uint32_t glyph) {
  return glyph > 0xFFFFFFu ? 4 : glyph > 0xFFFFu ? 3 : glyph > 0xFFu ? 2 : 1;
}

size_t cc_max_encoded(int cols, int rows) {
  return HDR + (size_t)cols * (size_t)rows * 11u + 16u;
}

typedef struct {
  uint8_t *p;
  size_t cap;
  size_t n;
  int over;
} wr_t;

static void put(wr_t *w, unsigned v) {
  if (w->n < w->cap) {
    w->p[w->n++] = (uint8_t)(v & 255u);
  } else {
    w->over = 1;
  }
}

static void put_varint(wr_t *w, unsigned v) {
  while (v >= 128u) {
    put(w, (v & 127u) + 128u);
    v /= 128u;
  }

  put(w, v);
}

static void put_cell(wr_t *w, const netcell_t *c, int mono) {
  if (!mono) {
    unsigned tag = (c->fg ? 1u : 0u) + (c->bg ? 2u : 0u);
    put(w, tag);
  }

  int len = glyph_bytes(c->glyph);
  for (int i = 0; i < len; i++) {
    put(w, (c->glyph >> (8u * (unsigned)i)) & 255u);
  }

  if (!mono) {
    uint32_t col[2] = {c->fg, c->bg};
    for (int k = 0; k < 2; k++) {
      if (col[k]) {
        put(w, (col[k] >> 16u) & 255u);
        put(w, (col[k] >> 8u) & 255u);
        put(w, col[k] & 255u);
      }
    }
  }
}

size_t cc_encode(const cc_grid_t *cur, const cc_grid_t *prev, int key,
                 uint8_t *out, size_t cap) {
  if (!cur->cell || cur->cols < 1 || cur->rows < 1) {
    return 0;
  }

  int cells = cur->cols * cur->rows;
  if (!prev || !prev->cell || prev->cols != cur->cols ||
      prev->rows != cur->rows) {
    key = 1;
  }

  int mono = 1;
  for (int i = 0; i < cells && mono; i++) {
    mono = cur->cell[i].fg == 0 && cur->cell[i].bg == 0;
  }

  wr_t w = {out, cap, 0, 0};
  put(&w, (key ? F_KEY : 0u) + (mono ? F_MONO : 0u));
  put(&w, (unsigned)cur->cols / 256u);
  put(&w, (unsigned)cur->cols);
  put(&w, (unsigned)cur->rows / 256u);
  put(&w, (unsigned)cur->rows);

  if (key) {
    for (int i = 0; i < cells; i++) {
      put_cell(&w, &cur->cell[i], mono);
    }
  } else {
    int i = 0;
    while (i < cells) {
      int skip = 0;
      while (i < cells && same_cell(&cur->cell[i], &prev->cell[i])) {
        i++;
        skip++;
      }

      put_varint(&w, (unsigned)skip);
      int start = i;
      while (i < cells && !same_cell(&cur->cell[i], &prev->cell[i])) {
        i++;
      }

      put_varint(&w, (unsigned)(i - start));
      for (int k = start; k < i; k++) {
        put_cell(&w, &cur->cell[k], mono);
      }
    }
  }

  return w.over ? 0 : w.n;
}

static int header(const uint8_t *in, size_t n, unsigned *flags, int *cols,
                  int *rows) {
  if (n < HDR || in[0] / 4u != 0) {
    return -1;
  }

  *flags = in[0];
  *cols = in[1] * 256 + in[2];
  *rows = in[3] * 256 + in[4];
  if (*cols < 1 || *cols > CC_MAX_COLS || *rows < 1 || *rows > CC_MAX_ROWS) {
    return -1;
  }

  return 0;
}

int cc_is_key(const uint8_t *in, size_t n) {
  unsigned flags;
  int cols;
  int rows;
  if (header(in, n, &flags, &cols, &rows) != 0) {
    return -1;
  }

  return (flags & F_KEY) != 0;
}

typedef struct {
  const uint8_t *p;
  const uint8_t *end;
} rd_t;

static int get(rd_t *r) { return r->p < r->end ? *r->p++ : -1; }

static long get_varint(rd_t *r) {
  long v = 0;
  long mul = 1;
  for (int part = 0; part < 3; part++, mul *= 128) {
    int b = get(r);
    if (b < 0) {
      return -1;
    }

    v += (long)(b % 128) * mul;
    if (b < 128) {
      return v;
    }
  }

  return -1;
}

static int get_cell(rd_t *r, netcell_t *c, int mono) {
  int tag = 0;
  if (!mono) {
    tag = get(r);
    if (tag < 0 || tag > 3) {
      return -1;
    }
  }
  unsigned has[2] = {(unsigned)tag % 2u, (unsigned)tag / 2u};

  int b0 = get(r);
  if (b0 < 0x20 || b0 == 0x7F) {
    return -1;
  }

  int len = b0 < 0x80    ? 1
            : b0 >= 0xF0 ? 4
            : b0 >= 0xE0 ? 3
            : b0 >= 0xC2 ? 2
                         : 0;
  if (len == 0 || b0 > 0xF4) {
    return -1;
  }

  uint32_t glyph = (uint32_t)b0;
  for (int i = 1; i < len; i++) {
    int b = get(r);
    if (b < 0 || b / 64 != 2) {
      return -1;
    }

    glyph += (uint32_t)b << (8u * (unsigned)i);
  }

  uint32_t col[2] = {0, 0};
  for (int k = 0; k < 2; k++) {
    if (has[k]) {
      int cr = get(r);
      int cg = get(r);
      int cb = get(r);
      if (cb < 0) {
        return -1;
      }

      col[k] = CC_RGB(cr, cg, cb);
    }
  }

  *c = (netcell_t){glyph, col[0], col[1]};

  return 0;
}

int cc_decode(cc_grid_t *state, const uint8_t *in, size_t n) {
  unsigned flags;
  int cols;
  int rows;
  if (header(in, n, &flags, &cols, &rows) != 0) {
    return -1;
  }

  int key = (flags & F_KEY) != 0;
  int mono = (flags & F_MONO) != 0;
  int cells = cols * rows;
  rd_t r = {in + HDR, in + n};

  if (!key && (!state->cell || state->cols != cols || state->rows != rows)) {
    return -2;
  }

  // Decode into a copy: a bad frame must not leave half a picture behind
  netcell_t *next = nl_malloc((size_t)cells * sizeof(netcell_t));
  if (!next) {
    return -1;
  }

  if (key) {
    for (int i = 0; i < cells; i++) {
      next[i] = (netcell_t){' ', 0, 0};
    }
  } else {
    nl_memcpy(next, state->cell, (size_t)cells * sizeof(netcell_t));
  }

  int ok = 1;
  if (key) {
    for (int i = 0; i < cells && ok; i++) {
      ok = get_cell(&r, &next[i], mono) == 0;
    }
  } else {
    int i = 0;
    while (i < cells && ok) {
      long skip = get_varint(&r);
      long run = skip < 0 ? -1 : get_varint(&r);
      ok = skip >= 0 && run >= 0 && skip + i <= cells &&
           skip + run + i <= cells && skip + run > 0;
      if (!ok) {
        break;
      }

      i += (int)skip;
      for (long k = 0; k < run && ok; k++) {
        ok = get_cell(&r, &next[i + (int)k], mono) == 0;
      }

      i += (int)run;
    }
  }

  if (!ok || r.p != r.end) {
    nl_free(next);

    return -1;
  }

  if (!state->cell || state->cols != cols || state->rows != rows) {
    nl_free(state->cell);
    state->cell = next;
    state->cols = cols;
    state->rows = rows;
  } else {
    nl_memcpy(state->cell, next, (size_t)cells * sizeof(netcell_t));
    nl_free(next);
  }

  return 0;
}

size_t cc_max_draw(int cols, int rows) {
  return (size_t)cols * (size_t)rows * 48u + (size_t)rows * 16u + 16u;
}

typedef struct {
  char *p;
  size_t cap;
  size_t n;
  int over;
} ow_t;

static void oc(ow_t *w, char c) {
  if (w->n < w->cap) {
    w->p[w->n++] = c;
  } else {
    w->over = 1;
  }
}

static void onum(ow_t *w, unsigned v) {
  char tmp[12];
  int k = 0;
  do {
    tmp[k++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v > 0);

  while (k > 0) {
    oc(w, tmp[--k]);
  }
}

static void ocolour(ow_t *w, int base, uint32_t c) {
  onum(w, (unsigned)base);
  if (c == 0) {
    oc(w, 'm'); // not reached: callers use 39/49 for default
    return;
  }

  oc(w, ';');
  oc(w, '2');
  oc(w, ';');
  onum(w, (c >> 16u) & 255u);
  oc(w, ';');
  onum(w, (c >> 8u) & 255u);
  oc(w, ';');
  onum(w, c & 255u);
}

size_t cc_draw_diff(const cc_grid_t *g, const cc_grid_t *shown, int x, int y,
                    int maxw, int maxh, char *out, size_t cap) {
  if (!g->cell || x < 1 || y < 1) {
    return 0;
  }

  int w = g->cols < maxw ? g->cols : maxw;
  int h = g->rows < maxh ? g->rows : maxh;
  ow_t o = {out, cap, 0, 0};
  uint32_t fg = 0xFFFFFFFFu; // unknown
  uint32_t bg = 0xFFFFFFFFu;
  // Cells screen already shows are skipped, a cursor move starts each run
  int same_size =
      shown && shown->cell && shown->cols == g->cols && shown->rows == g->rows;
  for (int row = 0; row < h; row++) {
    int at = -1; // column cursor is at, -1: unknown
    for (int col = 0; col < w; col++) {
      const netcell_t *c = &g->cell[row * g->cols + col];
      if (same_size) {
        const netcell_t *old = &shown->cell[row * g->cols + col];
        if (old->glyph == c->glyph && old->fg == c->fg && old->bg == c->bg) {
          continue;
        }
      }

      if (at != col) {
        oc(&o, '\033');
        oc(&o, '[');
        onum(&o, (unsigned)(y + row));
        oc(&o, ';');
        onum(&o, (unsigned)(x + col));
        oc(&o, 'H');
      }

      at = col + 1;
      if (c->fg != fg || c->bg != bg) {
        oc(&o, '\033');
        oc(&o, '[');
        if (c->fg != fg) {
          if (c->fg == 0) {
            onum(&o, 39);
          } else {
            ocolour(&o, 38, c->fg);
          }
        }

        if (c->fg != fg && c->bg != bg) {
          oc(&o, ';');
        }

        if (c->bg != bg) {
          if (c->bg == 0) {
            onum(&o, 49);
          } else {
            ocolour(&o, 48, c->bg);
          }
        }

        oc(&o, 'm');
        fg = c->fg;
        bg = c->bg;
      }

      int len = glyph_bytes(c->glyph);
      for (int i = 0; i < len; i++) {
        oc(&o, (char)((c->glyph >> (8u * (unsigned)i)) & 255u));
      }
    }
  }

  oc(&o, '\033');
  oc(&o, '[');
  oc(&o, '0');
  oc(&o, 'm');

  return o.over ? 0 : o.n;
}

size_t cc_draw(const cc_grid_t *g, int x, int y, int maxw, int maxh, char *out,
               size_t cap) {
  return cc_draw_diff(g, NULL, x, y, maxw, maxh, out, cap);
}
