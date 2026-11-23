/*
 * tests/screen.h - a small terminal screen model for tests
 *
 * Feed it bytes a program wrote and read back what a terminal would show,
 * row by row
 * NOTE: It replaces optional `pyte` checks of Python e2e script (which were
 * skipped wherever pyte was not installed). Only what AsciiCam writes is
 * modelled exactly: UTF-8 text, \r \n \b \t, cursor position (H / f, A B C D G
 * d), erase in display / line (J, K) and erase characters (X), deferred
 * auto-wrap (DECAWM, ?7), scrolling at bottom
 * Colours and attributes (m), other private modes and anything unknown are
 * accepted and ignored, an incomplete sequence is carried over to next feed
 *
 *   screen_t s;
 *   screen_init(&s, 50, 120);
 *   screen_feed(&s, bytes, n);
 *   char *text = screen_text(&s);   // Rows joined by '\n', trailing blanks cut
 *   free(text); screen_free(&s);
 *
 * A byte stream that never went through a pty has bare "\n" line ends; set
 * s.lnm = 1 to treat them as "\r\n" like a pty's ONLCR does
 */

#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int rows, cols;
  uint32_t *cell; // Rows * cols code points, ' ' when blank
  int r, c;       // Cursor, 0-based
  int pending;    // Last column was just filled: next glyph wraps
  int autowrap;   // DECAWM
  int lnm;        // Line feed also returns carriage
  int st; // parser: 0 ground, 1 ESC, 2 CSI, 3 OSC, 4 OSC + ESC, 5 skip one
  char par[64]; // CSI parameter bytes ("?25", "1;2" ...)
  int npar;
  uint32_t cp; // UTF-8 being assembled
  int need;
} screen_t;

static void screen_init(screen_t *s, int rows, int cols) {
  memset(s, 0, sizeof *s);
  s->rows = rows;
  s->cols = cols;
  s->autowrap = 1;
  s->cell = (uint32_t *)malloc((size_t)rows * (size_t)cols * sizeof *s->cell);
  for (int i = 0; i < rows * cols; i++) {
    s->cell[i] = ' ';
  }
}

static void screen_free(screen_t *s) {
  free(s->cell);
  s->cell = NULL;
}

static void screen_clear_range(screen_t *s, int from, int to) { // [from, to)
  for (int i = from; i < to; i++) {
    s->cell[i] = ' ';
  }
}

static void screen_lf(screen_t *s) {
  if (s->r == s->rows - 1) {
    memmove(s->cell, s->cell + s->cols,
            (size_t)(s->rows - 1) * (size_t)s->cols * sizeof *s->cell);
    screen_clear_range(s, (s->rows - 1) * s->cols, s->rows * s->cols);
  } else {
    s->r++;
  }

  if (s->lnm) {
    s->c = 0;
    s->pending = 0;
  }
}

static void screen_put(screen_t *s, uint32_t cp) {
  if (s->pending) {
    if (s->autowrap) {
      s->c = 0;
      screen_lf(s);
    }

    s->pending = 0;
  }

  s->cell[s->r * s->cols + s->c] = cp;
  if (s->c == s->cols - 1) {
    s->pending = 1; // Stay on last column until something else is written
  } else {
    s->c++;
  }
}

static int screen_clamp(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

// n-th numeric parameter (0-based) of CSI being parsed; def when missing or
// empty
static int screen_param(const screen_t *s, int idx, int def) {
  const char *p = s->par;
  while (*p == '?' || *p == '>' || *p == '=' || *p == '!') {
    p++;
  }

  for (int i = 0; i < idx; i++) {
    const char *semi = strchr(p, ';');
    if (!semi) {
      return def;
    }

    p = semi + 1;
  }

  if (*p < '0' || *p > '9') {
    return def;
  }

  return atoi(p);
}

static void screen_csi(screen_t *s, char fin) {
  int n = screen_param(s, 0, 1);
  if (n < 1) {
    n = 1;
  }

  int priv = s->par[0] == '?';
  switch (fin) {
  case 'H':
  case 'f': {
    s->r = screen_clamp(screen_param(s, 0, 1), 1, s->rows) - 1;
    s->c = screen_clamp(screen_param(s, 1, 1), 1, s->cols) - 1;
    s->pending = 0;

    break;
  }
  case 'A': {
    s->r = screen_clamp(s->r - n, 0, s->rows - 1);
    s->pending = 0;

    break;
  }
  case 'B': {
    s->r = screen_clamp(s->r + n, 0, s->rows - 1);
    s->pending = 0;

    break;
  }
  case 'C': {
    s->c = screen_clamp(s->c + n, 0, s->cols - 1);
    s->pending = 0;
    break;
  }
  case 'D': {
    s->c = screen_clamp(s->c - n, 0, s->cols - 1);
    s->pending = 0;

    break;
  }
  case 'G': {
    s->c = screen_clamp(n, 1, s->cols) - 1;
    s->pending = 0;

    break;
  }
  case 'd': {
    s->r = screen_clamp(n, 1, s->rows) - 1;
    s->pending = 0;

    break;
  }
  case 'J': {
    int mode = screen_param(s, 0, 0);
    int at = s->r * s->cols + s->c;
    if (mode == 0) {
      screen_clear_range(s, at, s->rows * s->cols);
    } else if (mode == 1) {
      screen_clear_range(s, 0, at + 1);
    } else {
      screen_clear_range(s, 0, s->rows * s->cols);
    }

    break;
  }

  case 'K': {
    int mode = screen_param(s, 0, 0), row = s->r * s->cols;
    if (mode == 0) {
      screen_clear_range(s, row + s->c, row + s->cols);
    } else if (mode == 1) {
      screen_clear_range(s, row, row + s->c + 1);
    } else {
      screen_clear_range(s, row, row + s->cols);
    }

    break;
  }

  case 'X': {
    int at = s->r * s->cols + s->c;
    int end = at + n;
    int lim = (s->r + 1) * s->cols;
    screen_clear_range(s, at, end > lim ? lim : end);
    break;
  }

  case 'h':
  case 'l': {
    if (priv) { // Each ';'-separated mode: only DECAWM matters here
      const char *p = s->par + 1;
      while (*p) {
        if (atoi(p) == 7) {
          s->autowrap = fin == 'h';
        }

        const char *semi = strchr(p, ';');
        if (!semi) {
          break;
        }

        p = semi + 1;
      }
    }

    break;
  }
  default: { // m (colours), s / u, anything else: no effect on text
    break;
  }
  }
}

static void screen_byte(screen_t *s, unsigned char b) {
  switch (s->st) {
  case 1: { // after ESC
    s->st = 0;
    if (b == '[') {
      s->st = 2;
      s->npar = 0;
      s->par[0] = 0;
    } else if (b == ']') {
      s->st = 3;
    } else if (b == '(' || b == ')' || b == '*' || b == '+') {
      s->st = 5;
    }

    return;
  }
  case 2: { // CSI: parameter / intermediate bytes, then a final byte 0x40-0x7e
    if (b >= 0x40 && b <= 0x7e) {
      s->st = 0;
      screen_csi(s, (char)b);
    } else if (b == 0x1b) {
      s->st = 1; // An interrupted sequence starts a new one
    } else if (s->npar < (int)sizeof s->par - 1) {
      s->par[s->npar++] = (char)b;
      s->par[s->npar] = 0;
    }

    return;
  }
  case 3: { // OSC: up to BEL or ESC \ (ST)
    if (b == 0x07) {
      s->st = 0;
    } else if (b == 0x1b) {
      s->st = 4;
    }

    return;
  }
  case 4: {
    s->st = b == '\\' ? 0 : 3;

    return;
  }
  case 5: { // Character set designation: one more byte
    s->st = 0;

    return;
  }
  default: {
    break;
  }
  }

  if (b == 0x1b) {
    s->st = 1;
  } else if (b == '\r') {
    s->c = 0;
    s->pending = 0;
  } else if (b == '\n' || b == 0x0b || b == 0x0c) {
    screen_lf(s);
  } else if (b == '\b') {
    if (s->c > 0) {
      s->c--;
    }

    s->pending = 0;
  } else if (b == '\t') {
    s->c = screen_clamp((s->c / 8 + 1) * 8, 0, s->cols - 1);
    s->pending = 0;
  } else if (b >= 0x20 && b < 0x7f) {
    screen_put(s, b);
  }
  // NOTE: bell, DEL and other controls: no effect
}

// Bytes in; UTF-8 is assembled here so a character split across two feeds works
static void screen_feed(screen_t *s, const unsigned char *d, size_t n) {
  for (size_t i = 0; i < n;) {
    unsigned char b = d[i];
    if (s->need > 0) {
      if ((b & 0xc0) == 0x80) {
        s->cp = (s->cp << 6) | (b & 0x3f);
        i++;
        if (--s->need == 0) {
          screen_put(s, s->cp);
        }

        continue;
      }

      // Cut short: show a replacement, then look at this byte again
      s->need = 0;
      screen_put(s, 0xfffd);
    }

    if (s->st != 0 || b < 0x80) {
      screen_byte(s, b);
    } else if ((b & 0xe0) == 0xc0) {
      s->cp = b & 0x1f;
      s->need = 1;
    } else if ((b & 0xf0) == 0xe0) {
      s->cp = b & 0x0f;
      s->need = 2;
    } else if ((b & 0xf8) == 0xf0) {
      s->cp = b & 0x07;
      s->need = 3;
    } else {
      screen_put(s, 0xfffd); // A stray continuation or an invalid lead byte
    }

    i++;
  }
}

static size_t screen_utf8(uint32_t cp, char *o) {
  if (cp < 0x80) {
    o[0] = (char)cp;

    return 1;
  }

  if (cp < 0x800) {
    o[0] = (char)(0xc0 | (cp >> 6));
    o[1] = (char)(0x80 | (cp & 0x3f));

    return 2;
  }

  if (cp < 0x10000) {
    o[0] = (char)(0xe0 | (cp >> 12));
    o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    o[2] = (char)(0x80 | (cp & 0x3f));

    return 3;
  }

  o[0] = (char)(0xf0 | (cp >> 18));
  o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  o[3] = (char)(0x80 | (cp & 0x3f));

  return 4;
}

// One row as UTF-8, trailing blanks cut
// Out must hold cols * 4 + 1 bytes
static void screen_row(const screen_t *s, int row, char *out) {
  int last = s->cols - 1;
  while (last >= 0 && s->cell[row * s->cols + last] == ' ') {
    last--;
  }

  size_t n = 0;
  for (int i = 0; i <= last; i++) {
    n += screen_utf8(s->cell[row * s->cols + i], out + n);
  }

  out[n] = 0;
}

// Whole screen: every row (blank ones empty) joined by '\n'. malloc'd
static char *screen_text(const screen_t *s) {
  size_t cap = (size_t)s->rows * ((size_t)s->cols * 4 + 2) + 1, n = 0;
  char *t = (char *)malloc(cap);
  char *row = (char *)malloc((size_t)s->cols * 4 + 1);
  for (int r = 0; r < s->rows; r++) {
    screen_row(s, r, row);
    size_t l = strlen(row);
    memcpy(t + n, row, l);
    n += l;
    t[n++] = '\n';
  }

  t[n] = 0;
  free(row);

  return t;
}

#endif
