#include "nolibc.h"

#include "netenc.h"

#define SCORE_UNKNOWN 1000 // cells never sent: most urgent
#define BINS 16
#define GAP_MAX 8 // longest run of unchanged cells re-sent to save a move
#define CELL_MAX_BYTES 80 // cursor move + colour escape + glyph, with margin

// Colour values
#define KIND(c) ((c) / 16777216u)
#define NE_RGB(r, g, b)                                                        \
  (16777216u + (uint32_t)(r) * 65536u + (uint32_t)(g) * 256u + (uint32_t)(b))
#define PAL(i) (33554432u + (uint32_t)(i))

// Parser

typedef struct {
  const char *p;
  const char *end;
} cursor_t;

// Parses CSI parameters
// Returns final byte or -1
static int parse_csi(cursor_t *c, int *params, int max_params, int *nparams) {
  int np = 0;
  int cur = 0;
  int have = 0;
  while (c->p < c->end) {
    unsigned char ch = (unsigned char)*c->p++;
    if (ch >= '0' && ch <= '9') {
      cur = cur * 10 + (ch - '0');
      have = 1;
      if (cur > 100000) {
        return -1;
      }
    } else if (ch == ';') {
      if (np >= max_params) {
        return -1;
      }

      params[np++] = have ? cur : 0;
      cur = 0;
      have = 0;
    } else if (ch >= 0x40 && ch <= 0x7E) {
      if (have || np > 0) {
        if (np >= max_params) {
          return -1;
        }

        params[np++] = have ? cur : 0;
      }

      *nparams = np;

      return ch;
    } else {
      return -1; // private markers, intermediates: not ours
    }
  }

  return -1;
}

static int utf8_len(unsigned char b) {
  if (b < 0x80) {
    return 1;
  }
  if ((b & 0xE0u) == 0xC0u) {
    return 2;
  }
  if ((b & 0xF0u) == 0xE0u) {
    return 3;
  }
  if ((b & 0xF8u) == 0xF0u) {
    return 4;
  }

  return 0;
}

// Parse a fram
// With grid == NULL only extent is measured
static int parse_frame(const char *in, size_t n, netcell_t *grid, uint8_t *seen,
                       int cols_stride, int *max_row, int *max_col) {
  cursor_t c = {in, in + n};
  int row = 1;
  int col = 1;
  uint32_t fg = 0;
  uint32_t bg = 0;
  int mr = 0;
  int mc = 0;

  while (c.p < c.end) {
    unsigned char b = (unsigned char)*c.p;
    if (b == 0x1B) {
      c.p++;
      if (c.p >= c.end || *c.p != '[') {
        return -1;
      }

      c.p++;
      int params[16];
      int np = 0;
      int fin = parse_csi(&c, params, 16, &np);
      if (fin == 'H') {
        row = np > 0 && params[0] > 0 ? params[0] : 1;
        col = np > 1 && params[1] > 0 ? params[1] : 1;
      } else if (fin == 'm') {
        if (np == 0) {
          fg = 0;
          bg = 0;
        }

        for (int i = 0; i < np; i++) {
          int v = params[i];
          if (v == 0) {
            fg = 0;
            bg = 0;
          } else if (v == 39) {
            fg = 0;
          } else if (v == 49) {
            bg = 0;
          } else if ((v == 38 || v == 48) && i + 2 < np && params[i + 1] == 5) {
            uint32_t pc = PAL((unsigned)params[i + 2] & 255u);
            if (v == 38) {
              fg = pc;
            } else {
              bg = pc;
            }

            i += 2;
          } else if ((v == 38 || v == 48) && i + 4 < np && params[i + 1] == 2) {
            uint32_t rc = NE_RGB((unsigned)params[i + 2] & 255u,
                                 (unsigned)params[i + 3] & 255u,
                                 (unsigned)params[i + 4] & 255u);
            if (v == 38) {
              fg = rc;
            } else {
              bg = rc;
            }

            i += 4;
          } else {
            return -1;
          }
        }
      } else {
        return -1;
      }

      continue;
    }

    if (b == '\n' || b == '\r') {
      c.p++;
      col = 1;
      if (b == '\n') {
        row++;
      }

      continue;
    }

    if (b < 0x20 || b == 0x7F) {
      return -1;
    }

    int len = utf8_len(b);
    if (len == 0 || c.p + len > c.end) {
      return -1;
    }

    uint32_t glyph = 0;
    for (int i = 0; i < len; i++) {
      unsigned char gb = (unsigned char)c.p[i];
      if (i > 0 && (gb & 0xC0u) != 0x80u) {
        return -1;
      }

      glyph |= (uint32_t)gb << (8u * (unsigned)i);
    }

    c.p += len;
    if (row > mr) {
      mr = row;
    }
    if (col > mc) {
      mc = col;
    }
    if (grid) {
      int idx = (row - 1) * cols_stride + (col - 1);
      grid[idx].glyph = glyph;
      grid[idx].fg = fg;
      grid[idx].bg = bg;
      seen[idx] = 1;
    }

    col++;
  }

  *max_row = mr;
  *max_col = mc;

  return 0;
}

// Scoring

static int chan_diff(uint32_t a, uint32_t b) {
  int d = 0;
  for (unsigned s = 0; s < 24; s += 8) {
    int x = (int)((a >> s) & 255u);
    int y = (int)((b >> s) & 255u);
    d += x > y ? x - y : y - x;
  }

  return d;
}

static int colour_score(uint32_t a, uint32_t b) {
  if (a == b) {
    return 0;
  }
  if (KIND(a) == 1 && KIND(b) == 1) {
    return chan_diff(a, b);
  }

  return 255; // default <-> colour, palette changes: always visible
}

static int cell_score(const netcell_t *prev, const netcell_t *cur) {
  if (prev->fg == NETCELL_UNKNOWN) {
    return SCORE_UNKNOWN;
  }

  int s = colour_score(prev->fg, cur->fg) + colour_score(prev->bg, cur->bg);
  if (prev->glyph != cur->glyph) {
    s += 96;
  }

  return s;
}

static int colour_cost(uint32_t c) {
  return KIND(c) == 1 ? 19 : KIND(c) == 2 ? 11 : 0;
}

static int cell_cost(const netcell_t *c) {
  return 4 + colour_cost(c->fg) + colour_cost(c->bg) +
         (c->glyph > 0xFFFFFFu ? 4
          : c->glyph > 0xFFFFu ? 3
          : c->glyph > 0xFFu   ? 2
                               : 1);
}

static int bin_of(int score) {
  if (score >= SCORE_UNKNOWN) {
    return BINS - 1;
  }

  int b = score / 52;

  return b > BINS - 2 ? BINS - 2 : b;
}

// Output

typedef struct {
  char *out;
  size_t cap;
  size_t len;
  int started;     // leading colour reset has been written
  int row, col;    // where terminal cursor is (0 = unknown)
  uint32_t fg, bg; // colours currently selected on terminal
} emit_t;

static void put_ch(emit_t *e, char c) { e->out[e->len++] = c; }

static void put_num(emit_t *e, int v) {
  char tmp[12];
  int n = 0;
  if (v == 0) {
    tmp[n++] = '0';
  }
  while (v > 0) {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n > 0) {
    put_ch(e, tmp[--n]);
  }
}

static void put_colour(emit_t *e, int base, uint32_t c) {
  if (KIND(c) == 0) {
    put_num(e, base + 1); // 39 / 49
  } else if (KIND(c) == 1) {
    put_num(e, base);
    put_ch(e, ';');
    put_ch(e, '2');
    put_ch(e, ';');
    put_num(e, (int)((c >> 16u) & 255u));
    put_ch(e, ';');
    put_num(e, (int)((c >> 8u) & 255u));
    put_ch(e, ';');
    put_num(e, (int)(c & 255u));
  } else {
    put_num(e, base);
    put_ch(e, ';');
    put_ch(e, '5');
    put_ch(e, ';');
    put_num(e, (int)(c & 255u));
  }
}

static void emit_cell(emit_t *e, int row, int col, const netcell_t *c) {
  if (!e->started) {
    // drawn before might have left colours selected
    put_ch(e, '\033');
    put_ch(e, '[');
    put_ch(e, '0');
    put_ch(e, 'm');
    e->started = 1;
  }

  if (e->row != row || e->col != col) {
    if (e->row == row && col > e->col && col - e->col <= 4) {
      // short hop right: re-print is not possible (cells differ), so CSI nC
      put_ch(e, '\033');
      put_ch(e, '[');
      put_num(e, col - e->col);
      put_ch(e, 'C');
    } else {
      put_ch(e, '\033');
      put_ch(e, '[');
      put_num(e, row);
      if (col > 1) {
        put_ch(e, ';');
        put_num(e, col);
      }

      put_ch(e, 'H');
    }
  }

  if (c->fg != e->fg || c->bg != e->bg) {
    put_ch(e, '\033');
    put_ch(e, '[');
    if (c->fg != e->fg) {
      put_colour(e, 38, c->fg);
    }
    if (c->fg != e->fg && c->bg != e->bg) {
      put_ch(e, ';');
    }
    if (c->bg != e->bg) {
      put_colour(e, 48, c->bg);
    }

    put_ch(e, 'm');
    e->fg = c->fg;
    e->bg = c->bg;
  }

  for (unsigned i = 0; i < 4; i++) {
    unsigned char b = (unsigned char)((c->glyph >> (8u * i)) & 255u);
    if (b == 0 && i > 0) {
      break;
    }

    put_ch(e, (char)b);
  }

  e->row = row;
  e->col = col + 1;
}

static int glyph_len(const netcell_t *c) {
  return c->glyph > 0xFFFFFFu ? 4
         : c->glyph > 0xFFFFu ? 3
         : c->glyph > 0xFFu   ? 2
                              : 1;
}

// Next cell to send is a few columns to right of cursor, re-sending cells in
// between can be cheaper than a cursor move (about 4 bytes): common in plain
// ASCII, where a cell costs one byte
// Cells frame did not write are never touched
// Returns number of cells filled
static int fill_gap(netenc_t *e, emit_t *em, int i, int row, int col) {
  if (em->row != row || col <= em->col || col - em->col > GAP_MAX) {
    return 0;
  }

  int gap = col - em->col;
  if (em->len + (size_t)(gap + 1) * CELL_MAX_BYTES + 12 > em->cap) {
    return 0;
  }

  for (int g = 1; g <= gap; g++) {
    if (!e->seen[i - g]) {
      return 0;
    }
  }

  int fill = 0;
  uint32_t fg = em->fg;
  uint32_t bg = em->bg;
  for (int g = gap; g >= 1; g--) {
    const netcell_t *c = &e->cur[i - g];
    fill += glyph_len(c);
    if (c->fg != fg) {
      fill += colour_cost(c->fg);
      fg = c->fg;
    }
    if (c->bg != bg) {
      fill += colour_cost(c->bg);
      bg = c->bg;
    }
  }

  int move = gap < 10 ? 4 : 5;
  if (fill > move) {
    return 0;
  }

  for (int g = gap; g >= 1; g--) {
    emit_cell(em, row, col - g, &e->cur[i - g]);
    e->prev[i - g] = e->cur[i - g];
  }

  return gap;
}

// Public API

void netenc_init(netenc_t *e, int min_score, int budget) {
  nl_memset(e, 0, sizeof(*e));
  e->min_score = min_score < 0 ? 0 : min_score;
  e->budget = budget < 0 ? 0 : budget;
}

void netenc_free(netenc_t *e) {
  nl_free(e->prev);
  nl_free(e->cur);
  nl_free(e->seen);
  e->prev = NULL;
  e->cur = NULL;
  e->seen = NULL;
  e->cap_cells = 0;
  e->rows = 0;
  e->cols = 0;
}

void netenc_invalidate(netenc_t *e) {
  for (int i = 0; i < e->cap_cells && e->prev; i++) {
    e->prev[i].fg = NETCELL_UNKNOWN;
  }
}

static int ensure_grid(netenc_t *e, int rows, int cols) {
  if (rows <= 0 || cols <= 0 || rows > NETENC_MAX_CELLS / cols) {
    return -1;
  }

  if (e->rows == rows && e->cols == cols && e->prev) {
    return 0;
  }

  // Geometry
  netenc_free(e);
  int cells = rows * cols;
  e->prev = nl_malloc((size_t)cells * sizeof(netcell_t));
  e->cur = nl_malloc((size_t)cells * sizeof(netcell_t));
  e->seen = nl_malloc((size_t)cells);
  if (!e->prev || !e->cur || !e->seen) {
    netenc_free(e);

    return -1;
  }

  e->cap_cells = cells;
  e->rows = rows;
  e->cols = cols;
  netenc_invalidate(e);

  return 0;
}

int netenc_frame(netenc_t *e, const char *in, size_t n, char *out, size_t cap) {
  int rows = 0;
  int cols = 0;
  if (parse_frame(in, n, NULL, NULL, 0, &rows, &cols) != 0) {
    return -1;
  }
  if (rows == 0) {
    e->frames++;
    e->in_bytes += n;

    return 0; // nothing drawn
  }

  // A grid that only grows with frames would keep stale geometry: use extent of
  // this frame and start over when it changes
  if (ensure_grid(e, rows, cols) != 0) {
    return -1;
  }

  nl_memset(e->seen, 0, (size_t)e->cap_cells);
  if (parse_frame(in, n, e->cur, e->seen, e->cols, &rows, &cols) != 0) {
    return -1;
  }

  // Pass 1: which cells are worth sending and what would they cost
  int hist_n[BINS] = {0};
  int hist_cost[BINS] = {0};
  int cells = e->rows * e->cols;
  for (int i = 0; i < cells; i++) {
    if (!e->seen[i]) {
      continue;
    }

    int s = cell_score(&e->prev[i], &e->cur[i]);
    if (s == 0) {
      continue;
    }
    if (e->prev[i].fg != NETCELL_UNKNOWN &&
        e->prev[i].glyph == e->cur[i].glyph && s < e->min_score) {
      continue; // colour noise below hysteresis: leave it as shown
    }

    int b = bin_of(s);
    hist_n[b]++;
    hist_cost[b] += cell_cost(&e->cur[i]);
  }

  // Lossless and nearly everything changed: renderer's own stream is already
  // cheapest way to say so
  if (e->budget == 0 && e->min_score == 0 && n <= cap) {
    int est = 0;
    for (int b = 0; b < BINS; b++) {
      est += hist_cost[b];
    }

    if ((size_t)est >= n) {
      nl_memcpy(out, in, n);
      int sent_all = 0;
      for (int i = 0; i < cells; i++) {
        if (e->seen[i]) {
          e->prev[i] = e->cur[i];
          sent_all++;
        }
      }

      e->frames++;
      e->in_bytes += n;
      e->out_bytes += n;
      e->cells_sent += (uint64_t)sent_all;
      e->pending = 0;

      return (int)n;
    }
  }

  // Pick lowest bin that still fits budget; boundary bin is taken in raster
  // order until its share of budget runs out
  int thr = 0;
  int thr_room = 1073741824; // effectively unlimited
  if (e->budget > 0) {
    int used = 0;
    thr = BINS;
    for (int b = BINS - 1; b >= 0; b--) {
      if (hist_n[b] == 0) {
        continue;
      }
      if (used + hist_cost[b] <= e->budget) {
        used += hist_cost[b];
        thr = b;

        continue;
      }

      thr = b; // boundary bin: partially
      thr_room = e->budget - used;

      break;
    }

    if (thr == BINS) {
      thr = BINS - 1; // nothing at all pending
    }
  }

  // Pass 2: emit in raster order
  emit_t em = {out, cap, 0, 0, 0, 0, 0, 0};
  int sent = 0;
  int pending = 0;
  int room_left = thr_room;
  size_t reserve = CELL_MAX_BYTES + 12; // one cell plus closing reset
  for (int i = 0; i < cells; i++) {
    if (!e->seen[i]) {
      continue;
    }

    int s = cell_score(&e->prev[i], &e->cur[i]);
    if (s == 0) {
      continue;
    }
    if (e->prev[i].fg != NETCELL_UNKNOWN &&
        e->prev[i].glyph == e->cur[i].glyph && s < e->min_score) {
      continue;
    }

    int b = bin_of(s);
    int take = b > thr;
    if (b == thr && e->budget > 0) {
      int cost = cell_cost(&e->cur[i]);
      if (room_left >= cost) {
        take = 1;
        room_left -= cost;
      }
    } else if (b == thr) {
      take = 1;
    }

    if (take && em.len + reserve > cap) {
      take = 0; // out of room: rest waits for next frame
    }

    if (!take) {
      pending++;

      continue;
    }

    int row = i / e->cols + 1;
    int col = i % e->cols + 1;
    sent += fill_gap(e, &em, i, row, col);
    emit_cell(&em, row, col, &e->cur[i]);
    e->prev[i] = e->cur[i];
    sent++;
  }

  if (em.fg != 0 || em.bg != 0) {
    if (em.len + 4 <= cap) {
      put_ch(&em, '\033');
      put_ch(&em, '[');
      put_ch(&em, '0');
      put_ch(&em, 'm');
    }
  }

  e->frames++;
  e->in_bytes += n;
  e->out_bytes += em.len;
  e->cells_sent += (uint64_t)sent;
  e->pending = pending;

  return (int)em.len;
}
