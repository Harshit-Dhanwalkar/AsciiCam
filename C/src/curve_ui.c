/*
 * src/curve_ui.c - tone curve editor panel, see include/curve_ui.h
 */

#include "platform.h"

#include "nolibc.h"

#include "curve_ui.h"

#define GW 30       // graph width in cells
#define DW (GW * 2) // and in braille dots
#define MAX_GH 12

typedef struct {
  int col; // panel's first column
  int h;   // panel rows (bottom border row)
  int gx;  // first graph column
  int gy;  // first graph row
  int gh;  // graph height in cells
} geom_t;

static int geom(int term_h, int term_w, geom_t *g) {
  if (!browser_panel_geom(term_h, term_w, &g->col, &g->h)) {
    return 0;
  }

  // Rows: tab bar, title, graph, info, preset, 4 hints, bottom border
  g->gh = g->h - 9;
  if (g->gh > MAX_GH) {
    g->gh = MAX_GH;
  }

  if (g->gh < 5) {
    return 0;
  }

  g->gx = g->col + 2; // border + one blank column
  g->gy = 3;

  return 1;
}

void curve_ui_init(curve_ui_t *ui) { nl_memset(ui, 0, sizeof(*ui)); }

int curve_ui_grabbing(const curve_ui_t *ui) { return ui->grab; }

// Input

static void clamp_sel(curve_ui_t *ui, const curve_t *c) {
  if (ui->sel < 0) {
    ui->sel = 0;
  }

  if (ui->sel >= c->n) {
    ui->sel = c->n - 1;
  }
}

static curve_act_t nudge(curve_ui_t *ui, curve_t *c, int dx, int dy) {
  clamp_sel(ui, c);
  int sel = ui->sel;
  if (sel < 0 || sel >= c->n || sel >= CURVE_MAX_POINTS) {
    return CUI_NONE;
  }

  curve_move(c, sel, c->p[sel].x + dx, c->p[sel].y + dy);
  ui->status[0] = '\0';

  return CUI_CHANGED;
}

curve_act_t curve_ui_key(curve_ui_t *ui, curve_t *c, brows_keykind_t kind,
                         char ch) {
  clamp_sel(ui, c);
  switch (kind) {
  case BR_KEY_ESC:
    return CUI_CLOSE;
  case BR_KEY_UP:
    return nudge(ui, c, 0, 1);
  case BR_KEY_DOWN:
    return nudge(ui, c, 0, -1);
  case BR_KEY_LEFT:
    return nudge(ui, c, -1, 0);
  case BR_KEY_RIGHT:
    return nudge(ui, c, 1, 0);
  case BR_KEY_ENTER:
    return CUI_NONE;
  case BR_KEY_CHAR:
    break;
  }

  switch (ch) {
  case 'q':
  case 'Q':
  case 'k':
  case 'K':
    return CUI_CLOSE;
  case 'b':
  case 'B':
  case '\t':
    return CUI_TAB_FILES;
  case 'n':
    ui->sel = (ui->sel + 1) % c->n;

    return CUI_NONE;
  case 'p':
    ui->sel = (ui->sel + c->n - 1) % c->n;

    return CUI_NONE;
  case '<':
  case ',':
    return nudge(ui, c, -8, 0);
  case '>':
  case '.':
    return nudge(ui, c, 8, 0);
  case '+':
  case '=':
    return nudge(ui, c, 0, 8);
  case '-':
  case '_':
    return nudge(ui, c, 0, -8);
  case ']':
  case '[': {
    int cnt = curve_preset_count();
    int cur = c->preset < 0 ? 0 : c->preset;
    int next = (cur + (ch == ']' ? 1 : cnt - 1)) % cnt;
    int keep = c->enabled;
    curve_set_preset(c, next);
    c->enabled = keep;
    ui->sel = 0;
    nl_snprintf(ui->status, sizeof(ui->status), "%s", curve_preset_name(next));

    return CUI_CHANGED;
  }
  case 'r':
  case 'R':
    curve_reset(c);
    ui->sel = 0;
    nl_snprintf(ui->status, sizeof(ui->status), "reset");

    return CUI_CHANGED;
  case 'x':
  case 'X':
    c->enabled = !c->enabled;
    nl_snprintf(ui->status, sizeof(ui->status), c->enabled ? "on" : "bypassed");

    return CUI_CHANGED;
  case 'a':
  case 'A': {
    // New point in middle of widest gap, on curve as it is now
    int at = 0, widest = 0;
    for (int k = 0; k + 1 < c->n; k++) {
      int w = c->p[k + 1].x - c->p[k].x;
      if (w > widest) {
        widest = w;
        at = k;
      }
    }

    int x = c->p[at].x + widest / 2;
    int idx = curve_add(c, x, c->lut[x]);
    if (idx < 0) {
      nl_snprintf(ui->status, sizeof(ui->status), "no room for a point");

      return CUI_NONE;
    }

    ui->sel = idx;
    ui->status[0] = '\0';

    return CUI_CHANGED;
  }
  case 'd':
  case 'D':
  case 127:
  case 8:
    if (curve_remove(c, ui->sel) == 0) {
      clamp_sel(ui, c);
      ui->status[0] = '\0';

      return CUI_CHANGED;
    }

    nl_snprintf(ui->status, sizeof(ui->status), "end points stay");

    return CUI_NONE;
  case 'w':
  case 'W':
    return CUI_SAVE;
  default:
    break;
  }

  if (ch >= '1' && ch <= '8') {
    int i = ch - '1';
    if (i < c->n) {
      ui->sel = i;
    }
  }

  return CUI_NONE;
}

// terminal cell -> curve coordinates, centre of cell
static void cell_to_xy(const geom_t *g, int col, int row, int *x, int *y) {
  int cx = col - g->gx, cy = row - g->gy;
  cx = cx < 0 ? 0 : cx >= GW ? GW - 1 : cx;
  cy = cy < 0 ? 0 : cy >= g->gh ? g->gh - 1 : cy;
  int dx = cx * 2 + 1, dy = cy * 4 + 2;
  int dh = g->gh * 4;
  *x = (dx * 255 + (DW - 1) / 2) / (DW - 1);
  *y = 255 - (dy * 255 + (dh - 1) / 2) / (dh - 1);
  if (*x > 255) {
    *x = 255;
  }

  if (*y < 0) {
    *y = 0;
  }
}

// Point whose marker is within one cell of (col, row), nearest first
static int hit_point(const geom_t *g, const curve_t *c, int col, int row) {
  int best = -1, bd = 1048576;
  int dh = g->gh * 4;
  for (int k = 0; k < c->n; k++) {
    int pc = g->gx + (c->p[k].x * (DW - 1) / 255) / 2;
    int pr = g->gy + ((255 - c->p[k].y) * (dh - 1) / 255) / 4;
    int dc = pc - col, dr = pr - row;
    if (dc < -1 || dc > 1 || dr < -1 || dr > 1) {
      continue;
    }

    int d = dc * dc * 4 + dr * dr; // a column is about half a row wide
    if (d < bd) {
      bd = d;
      best = k;
    }
  }

  return best;
}

curve_act_t curve_ui_mouse(curve_ui_t *ui, curve_t *c, const mouse_event_t *ev,
                           int term_h, int term_w) {
  geom_t g;
  if (!geom(term_h, term_w, &g)) {
    ui->grab = 0;

    return CUI_NONE;
  }

  clamp_sel(ui, c);
  if (ev->wheel) {
    return CUI_NONE;
  }

  if (ui->grab) {
    if (!ev->pressed) {
      ui->grab = 0; // released, even outside panel

      return CUI_NONE;
    }

    if (ev->motion) {
      int x, y;
      cell_to_xy(&g, ev->x, ev->y, &x, &y);
      curve_move(c, ui->sel, x, y);
      ui->status[0] = '\0';

      return CUI_CHANGED;
    }

    return CUI_NONE;
  }

  int in_graph = ev->x >= g.gx && ev->x < g.gx + GW && ev->y >= g.gy &&
                 ev->y < g.gy + g.gh;
  if (!in_graph || !ev->pressed || ev->motion) {
    return CUI_NONE;
  }

  int hit = hit_point(&g, c, ev->x, ev->y);
  if (ev->button == 2) { // right click deletes
    if (hit >= 0 && curve_remove(c, hit) == 0) {
      clamp_sel(ui, c);
      ui->status[0] = '\0';

      return CUI_CHANGED;
    }

    return CUI_NONE;
  }

  if (ev->button != 0) {
    return CUI_NONE;
  }

  if (hit >= 0) {
    ui->sel = hit;
    ui->grab = 1;

    return CUI_NONE;
  }

  int x, y;
  cell_to_xy(&g, ev->x, ev->y, &x, &y);
  int idx = curve_add(c, x, y);
  if (idx < 0) {
    nl_snprintf(ui->status, sizeof(ui->status), "no room for a point");

    return CUI_NONE;
  }

  ui->sel = idx;
  ui->grab = 1;
  ui->status[0] = '\0';

  return CUI_CHANGED;
}

// Drawing

enum { K_NONE, K_HIST, K_DIAG, K_CURVE, K_POINT, K_SEL };

static int bitlen(uint32_t v) {
  int n = 0;
  while (v) {
    n++;
    v /= 2;
  }

  return n;
}

// Braille bit of dot (cx 0..1, cy 0..3) in a cell
static uint8_t dot_bit(int cx, int cy) {
  static const uint8_t bits[2][4] = {{0x01, 0x02, 0x04, 0x40},
                                     {0x08, 0x10, 0x20, 0x80}};

  return bits[cx][cy];
}

typedef struct {
  // Dots per layer: a cell shows curve if it has any dot of it, else histogram,
  // else diagonal
  // Layers are not merged, a filled histogram would turn every cell of curve
  // into a solid block
  uint8_t layer[3][MAX_GH][GW]; // [0] diagonal, [1] histogram, [2] curve
  uint8_t marker[MAX_GH][GW];   // 1 = control point, 2 = selected one
} canvas_t;

static void plot(canvas_t *cv, int gh, int dx, int dy, int kind) {
  if (dx < 0 || dx >= DW || dy < 0 || dy >= gh * 4) {
    return;
  }

  cv->layer[kind == K_CURVE  ? 2
            : kind == K_HIST ? 1
                             : 0][dy / 4][dx / 2] |= dot_bit(dx % 2, dy % 4);
}

static int y_to_dy(int y, int dh) { return (255 - y) * (dh - 1) / 255; }

static void draw_graph(canvas_t *cv, const curve_ui_t *ui, const curve_t *c,
                       int gh) {
  int dh = gh * 4;

  // Histogram: bars on a log scale so a dark room does not flatten rest
  uint32_t bin[DW];
  uint32_t top = 0;
  for (int b = 0; b < DW; b++) {
    uint32_t s = 0;
    for (int v = b * 256 / DW; v < (b + 1) * 256 / DW; v++) {
      s += ui->hist[v];
    }

    bin[b] = s;
    if (s > top) {
      top = s;
    }
  }

  if (top > 0) {
    int tl = bitlen(top);
    for (int b = 0; b < DW; b++) {
      if (!bin[b]) {
        continue;
      }

      int hgt = bitlen(bin[b]) * (dh * 3 / 5) / tl; // leave room for curve
      hgt = hgt < 1 ? 1 : hgt;
      for (int j = 0; j < hgt; j++) {
        plot(cv, gh, b, dh - 1 - j, K_HIST);
      }
    }
  }

  // Identity diagonal
  for (int dx = 0; dx < DW; dx++) {
    plot(cv, gh, dx, y_to_dy(dx * 255 / (DW - 1), dh), K_DIAG);
  }

  // Curve; vertical runs are joined so steep parts stay connected
  int prev = -1;
  for (int dx = 0; dx < DW; dx++) {
    int dy = y_to_dy(c->lut[dx * 255 / (DW - 1)], dh);
    int from = prev < 0 ? dy : prev;
    int step = dy >= from ? 1 : -1;
    for (int r = from;; r += step) {
      plot(cv, gh, dx, r, K_CURVE);
      if (r == dy) {
        break;
      }
    }

    prev = dy;
  }

  // Control points
  for (int k = 0; k < c->n; k++) {
    int dx = c->p[k].x * (DW - 1) / 255;
    int dy = y_to_dy(c->p[k].y, dh);
    int cx = dx / 2, cy = dy / 4;
    int sel = k == ui->sel;
    if (!cv->marker[cy][cx] || sel) {
      cv->marker[cy][cx] = sel ? 2 : 1;
    }
  }
}

static int put(char *out, size_t outsz, int *pos, const char *s) {
  int n = nl_snprintf(out + *pos, outsz - (size_t)*pos, "%s", s);
  if (n < 0 || (size_t)n >= outsz - (size_t)*pos) {
    return -1;
  }

  *pos += n;

  return 0;
}

#define PUTF(...)                                                              \
  do {                                                                         \
    int n_ = nl_snprintf(out + pos, outsz - (size_t)pos, __VA_ARGS__);         \
    if (n_ < 0 || (size_t)n_ >= outsz - (size_t)pos) {                         \
      return 0;                                                                \
    }                                                                          \
    pos += n_;                                                                 \
  } while (0)

// One panel row: "│ text<pad> │" with row's colours. text is plain ASCII
static int text_row(char *out, size_t outsz, int *pos_io, int row, int col,
                    const char *bg, const char *fg, const char *text) {
  int pos = *pos_io;
  PUTF("\033[%d;%dH%s│%s", row, col, bg, fg);
  int shown = 0;
  PUTF(" ");
  shown++;
  for (const char *t = text; *t && shown < BROWS_PANEL_W - 3; t++, shown++) {
    PUTF("%c", *t);
  }

  for (; shown < BROWS_PANEL_W - 2; shown++) {
    PUTF(" ");
  }

  PUTF("│\033[0m");
  *pos_io = pos;

  return 1;
}

int curve_ui_render(const curve_ui_t *ui, const curve_t *c, int term_h,
                    int term_w, int color, char *out, size_t outsz) {
  geom_t g;
  if (!geom(term_h, term_w, &g) || outsz < 4096) {
    return 0; // a full panel is a few KB
  }

  const char *border_fg = color ? "\033[38;2;120;140;180m" : "";
  const char *title_fg = color ? "\033[38;2;180;200;255m\033[1m" : "";
  const char *bg = color ? "\033[48;2;18;18;28m" : "";
  const char *text_fg = color ? "\033[38;2;170;170;200m" : "";
  const char *hint_fg = color ? "\033[38;2;140;140;160m" : "";
  const char *reset = "\033[0m";
  const char *fg_of[6] = {
      "",
      color ? "\033[38;2;60;75;115m" : "",
      color ? "\033[38;2;70;70;95m" : "",
      color ? "\033[38;2;110;220;255m" : "",
      color ? "\033[38;2;240;240;240m" : "",
      color ? "\033[38;2;255;210;60m\033[1m" : "\033[7m",
  };

  canvas_t cv;
  nl_memset(&cv, 0, sizeof(cv));
  draw_graph(&cv, ui, c, g.gh);

  int pos = 0;
  out[0] = '\0';
  PUTF("\033[1;%dH%s┌─ Tone curve ", g.col, border_fg);
  for (int i = 14; i < BROWS_PANEL_W - 1; i++) {
    if (put(out, outsz, &pos, "─") < 0) {
      return 0;
    }
  }

  PUTF("┐%s", reset);

  char line[64];
  // Title row
  nl_snprintf(line, sizeof(line), "%s", ui->status);
  if (!text_row(out, outsz, &pos, 2, g.col, bg, title_fg, line)) {
    return 0;
  }

  // Graph rows
  for (int r = 0; r < g.gh; r++) {
    PUTF("\033[%d;%dH%s%s│%s%s ", g.gy + r, g.col, bg, border_fg, reset, bg);
    int last_kind = -1;
    for (int cx = 0; cx < GW; cx++) {
      int marker = cv.marker[r][cx];
      const char *glyph;
      char braille[4];
      int style;
      int mask = 0, kind = K_NONE;
      if (cv.layer[2][r][cx]) {
        mask = cv.layer[2][r][cx];
        kind = K_CURVE;
      } else if (cv.layer[1][r][cx]) {
        mask = cv.layer[1][r][cx];
        kind = K_HIST;
      } else if (cv.layer[0][r][cx]) {
        mask = cv.layer[0][r][cx];
        kind = K_DIAG;
      }

      if (marker) {
        glyph = "◆";
        style = marker == 2 ? K_SEL : K_POINT;
      } else if (mask) {
        if (!color && kind < K_CURVE) {
          glyph = " "; // histogram and diagonal need colour to be told apart
          style = K_NONE;
        } else {
          uint32_t cp = 0x2800u + (uint32_t)mask;
          braille[0] = (char)(0xe0u | (cp / 4096u));
          braille[1] = (char)(0x80u | ((cp / 64u) % 64u));
          braille[2] = (char)(0x80u | (cp % 64u));
          braille[3] = '\0';
          glyph = braille;
          style = kind;
        }
      } else {
        glyph = " ";
        style = K_NONE;
      }

      if (style != last_kind) {
        PUTF("%s%s%s", reset, bg, fg_of[style]);
        last_kind = style;
      }

      PUTF("%s", glyph);
    }

    PUTF("%s%s %s│%s", reset, bg, border_fg, reset);
  }

  // Info rows
  clamp_sel((curve_ui_t *)ui, c);
  int row = g.gy + g.gh;
  nl_snprintf(line, sizeof(line), "pt %d/%d   in %d  out %d", ui->sel + 1, c->n,
              c->p[ui->sel].x, c->p[ui->sel].y);
  text_row(out, outsz, &pos, row++, g.col, bg, text_fg, line);
  nl_snprintf(line, sizeof(line), "preset: %s%s", curve_preset_name(c->preset),
              c->enabled ? "" : "  [bypassed]");
  text_row(out, outsz, &pos, row++, g.col, bg, text_fg, line);

  static const char *const hints[] = {
      "click add/drag  rclick delete",
      "n/p pick  arrows <> +- move",
      "[ ] preset a add d del r reset",
      "x on/off  w save  tab: next tab",
  };
  for (int i = 0; i < 4; i++) {
    text_row(out, outsz, &pos, row++, g.col, bg, hint_fg, hints[i]);
  }

  // Blank rows down to bottom border
  for (; row < g.h; row++) {
    text_row(out, outsz, &pos, row, g.col, bg, text_fg, "");
  }

  PUTF("\033[%d;%dH%s└", g.h, g.col, border_fg);
  for (int i = 1; i < BROWS_PANEL_W - 1; i++) {
    if (put(out, outsz, &pos, "─") < 0) {
      return 0;
    }
  }

  PUTF("┘%s", reset);
  (void)hint_fg;

  return pos;
}
