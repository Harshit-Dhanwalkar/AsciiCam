/*
 * tests/test_curve_ui.c - src/curve_ui.c: keys, mouse (add, grab, drag,
 * release, right click delete) and panel drawing: every row exactly as wide as
 * panel, inside screen, one marker per control point
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_curve_ui.c src/curve_ui.c src/curve.c \
 *     src/browser.c src/recorder.c lib/nl_printf.c lib/nl_errno.c \
 *     lib/nl_alloc.c lib/nl_start.c lib/nl_mem.c -Iinclude -Ilib -o /tmp/tcu
 */

#include "platform.h"

#include "../lib/nolibc.h"
#include "curve_ui.h"

static int pass_n, fail_n;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

#define TW 120
#define TH 50
// Panel: columns 87..120, rows 1..45; graph columns 89..118, rows 3..14

static curve_ui_t ui;
static curve_t c;

static void fresh(void) {
  curve_ui_init(&ui);
  curve_reset(&c);
}

static curve_act_t key(char ch) {
  return curve_ui_key(&ui, &c, BR_KEY_CHAR, ch);
}

static curve_act_t mouse(int x, int y, int button, int pressed, int motion) {
  mouse_event_t ev = {x, y, button, motion, 0, pressed};

  return curve_ui_mouse(&ui, &c, &ev, TH, TW);
}

static int lut_ok(void) {
  for (int k = 0; k < c.n; k++) {
    if (c.lut[c.p[k].x] != c.p[k].y) {
      return 0;
    }
  }

  return 1;
}

static int has_str(const char *buf, int len, const char *needle) {
  int nl = (int)nl_strlen(needle);
  for (int i = 0; i + nl <= len; i++) {
    int k = 0;
    while (k < nl && buf[i + k] == needle[k]) {
      k++;
    }

    if (k == nl) {
      return i + 1;
    }
  }

  return 0;
}

static int count_str(const char *buf, int len, const char *needle) {
  int n = 0, nl = (int)nl_strlen(needle);
  for (int i = 0; i + nl <= len; i++) {
    int k = 0;
    while (k < nl && buf[i + k] == needle[k]) {
      k++;
    }

    n += k == nl;
  }

  return n;
}

// Walk output: every cursor move must land in column place_col on a row of
// panel, and text after it up to next move must be exactly BROWS_PANEL_W cells
// wide (one cell per UTF-8 character, escapes are free)
static int rows_ok(const char *buf, int len, int place_col, int panel_h,
                   int *rows_seen) {
  int rows = 0, width = -1, ok = 1;
  for (int i = 0; i < len;) {
    if (buf[i] == '\033' && i + 1 < len && buf[i + 1] == '[') {
      int j = i + 2, a = 0, b = 0, semi = 0;
      while (j < len && (buf[j] == ';' || (buf[j] >= '0' && buf[j] <= '9'))) {
        if (buf[j] == ';') {
          semi = 1;
        } else if (semi) {
          b = b * 10 + (buf[j] - '0');
        } else {
          a = a * 10 + (buf[j] - '0');
        }

        j++;
      }

      if (j < len && buf[j] == 'H') {
        if (width >= 0 && width != BROWS_PANEL_W) {
          ok = 0;
        }

        if (b != place_col || a < 1 || a > panel_h) {
          ok = 0;
        }

        width = 0;
        rows++;
      }

      i = j + 1;
    } else {
      if (((unsigned char)buf[i] & 0xc0) != 0x80) {
        width++;
      }

      i++;
    }
  }

  if (width != BROWS_PANEL_W) {
    ok = 0;
  }

  *rows_seen = rows;

  return ok;
}

static int valid(void) {
  if (c.n < 2 || c.n > CURVE_MAX_POINTS || c.p[0].x != 0 ||
      c.p[c.n - 1].x != 255 || ui.sel < 0 || ui.sel >= c.n) {
    return 0;
  }

  for (int k = 1; k < c.n; k++) {
    if (c.p[k].x <= c.p[k - 1].x) {
      return 0;
    }
  }

  return lut_ok();
}

static int count_braille(const char *buf, int len) {
  int n = 0;
  for (int i = 0; i + 2 < len; i++) {
    n += (unsigned char)buf[i] == 0xe2 && (unsigned char)buf[i + 1] >= 0xa0 &&
         (unsigned char)buf[i + 1] <= 0xa3;
  }

  return n;
}

static char buf[1 << 15];

int main(void) {
  // Keys
  fresh();
  CHECK(key('n') == CUI_NONE && ui.sel == 1 && key('n') == CUI_NONE &&
            ui.sel == 0,
        "n cycles through the two points");
  CHECK(key('p') == CUI_NONE && ui.sel == 1, "p goes back");
  curve_ui_key(&ui, &c, BR_KEY_UP, 0);
  CHECK(c.p[1].y == 255, "up on a point at 255 stays 255");
  ui.sel = 0;
  CHECK(curve_ui_key(&ui, &c, BR_KEY_UP, 0) == CUI_CHANGED && c.p[0].y == 1 &&
            !c.identity,
        "up raises the black point, the line is no longer straight");
  CHECK(curve_ui_key(&ui, &c, BR_KEY_DOWN, 0) == CUI_CHANGED && c.p[0].y == 0 &&
            c.identity,
        "down puts it back");
  curve_ui_key(&ui, &c, BR_KEY_RIGHT, 0);
  CHECK(c.p[0].x == 0, "an end point cannot move sideways");

  CHECK(key('a') == CUI_CHANGED && c.n == 3 && c.p[1].x == 127 && ui.sel == 1,
        "a adds a point in the middle (%d)", c.p[1].x);
  CHECK(key('>') == CUI_CHANGED && c.p[1].x == 135, "> moves 8 right");
  CHECK(key('+') == CUI_CHANGED && c.p[1].y == 135, "+ moves 8 up");
  CHECK(key('-') == CUI_CHANGED && key('<') == CUI_CHANGED && c.p[1].x == 127 &&
            c.p[1].y == 127,
        "- and < move back");
  curve_ui_key(&ui, &c, BR_KEY_LEFT, 0);
  curve_ui_key(&ui, &c, BR_KEY_UP, 0);
  CHECK(c.p[1].x == 126 && c.p[1].y == 128, "arrows move by one");
  key('3');
  CHECK(ui.sel == 2, "digit picks a point");
  key('9');
  CHECK(ui.sel == 2, "digit beyond the last point is ignored");
  CHECK(key('d') == CUI_NONE && c.n == 3, "an end point is not deleted");
  CHECK(has_str(ui.status, 40, "end points"), "and says so");
  key('2');
  CHECK(key('d') == CUI_CHANGED && c.n == 2 && ui.sel == 1, "d deletes");
  while (c.n < CURVE_MAX_POINTS) {
    key('a');
  }

  CHECK(c.n == 8 && key('a') == CUI_NONE && c.n == 8, "no ninth point");
  CHECK(valid(), "curve valid after adding 6 points");
  CHECK(key(']') == CUI_CHANGED && c.preset == 1 && ui.sel == 0 && c.n == 4,
        "] loads the next preset (s-curve)");
  CHECK(key('[') == CUI_CHANGED && c.preset == 0 && c.identity,
        "[ goes back to linear");
  CHECK(key('[') == CUI_CHANGED && c.preset == curve_preset_count() - 1,
        "[ wraps to the last preset");
  CHECK(key('x') == CUI_CHANGED && !c.enabled && !curve_active(&c),
        "x bypasses");
  key(']');
  CHECK(!c.enabled, "bypass survives a preset change");
  CHECK(key('x') == CUI_CHANGED && c.enabled, "x turns it back on");
  CHECK(key('r') == CUI_CHANGED && c.identity && c.n == 2 && c.enabled,
        "r resets");
  CHECK(key('w') == CUI_SAVE, "w asks to save");
  CHECK(key('b') == CUI_TAB_FILES && key('\t') == CUI_TAB_FILES,
        "b and tab go to the captures tab");
  CHECK(key('k') == CUI_CLOSE && key('q') == CUI_CLOSE &&
            curve_ui_key(&ui, &c, BR_KEY_ESC, 0) == CUI_CLOSE,
        "k, q and esc close");
  CHECK(key('%') == CUI_NONE && key('\0') == CUI_NONE, "other keys do nothing");

  // Mouse
  fresh();
  CHECK(mouse(50, 10, 0, 1, 0) == CUI_NONE && c.n == 2 && !ui.grab,
        "click outside the panel does nothing");
  CHECK(mouse(87, 10, 0, 1, 0) == CUI_NONE && c.n == 2,
        "click on the panel border does nothing");
  CHECK(mouse(95, 20, 0, 1, 0) == CUI_NONE && c.n == 2,
        "click on the hint rows does nothing");
  mouse_event_t wheel = {95, 8, 0, 0, 1, 1};
  CHECK(curve_ui_mouse(&ui, &c, &wheel, TH, TW) == CUI_NONE && c.n == 2,
        "wheel is ignored");
  CHECK(mouse(95, 8, 0, 1, 0) == CUI_CHANGED && c.n == 3 && ui.grab &&
            ui.sel == 1 && c.p[1].x == 56 && c.p[1].y == 136,
        "click in the graph adds a point and grabs it (%d,%d)", c.p[1].x,
        c.p[1].y);
  CHECK(mouse(104, 3, 0, 1, 1) == CUI_CHANGED && c.p[1].x == 134 &&
            c.p[1].y == 244 && lut_ok(),
        "dragging moves it (%d,%d)", c.p[1].x, c.p[1].y);
  CHECK(mouse(130, 0, 0, 1, 1) == CUI_CHANGED && c.p[1].x == 254 &&
            c.p[1].y == 244,
        "dragging past the edge stops at the neighbour (%d,%d)", c.p[1].x,
        c.p[1].y);
  CHECK(mouse(130, 0, 0, 0, 0) == CUI_NONE && !ui.grab,
        "release outside the panel ends the drag");
  CHECK(curve_ui_grabbing(&ui) == 0, "not grabbing");
  CHECK(mouse(104, 3, 0, 1, 1) == CUI_NONE && c.p[1].x == 254,
        "motion without a grab does nothing");

  CHECK(mouse(118, 3, 0, 1, 0) == CUI_CHANGED || ui.grab, "grab a point");
  fresh();
  curve_add(&c, 56, 136);
  curve_move(&c, 1, 254, 244);
  CHECK(mouse(118, 3, 0, 1, 0) == CUI_NONE && ui.grab && ui.sel == 1,
        "a click on a point grabs it without adding another (%d)", c.n);
  mouse(118, 3, 0, 0, 0);
  CHECK(mouse(118, 3, 2, 1, 0) == CUI_CHANGED && c.n == 2,
        "right click deletes it");
  CHECK(mouse(118, 3, 2, 1, 0) == CUI_NONE && c.n == 2,
        "right click on an end point does not");
  CHECK(mouse(118, 3, 0, 1, 0) == CUI_NONE && ui.grab && ui.sel == 1,
        "grab the white end point");
  CHECK(mouse(118, 8, 0, 1, 1) == CUI_CHANGED && c.p[1].x == 255 &&
            c.p[1].y == 136,
        "it moves up and down only (%d,%d)", c.p[1].x, c.p[1].y);
  mouse(118, 8, 0, 0, 0);
  CHECK(mouse(95, 8, 1, 1, 0) == CUI_NONE && c.n == 2,
        "middle click does nothing");
  CHECK(valid(), "curve valid after the mouse tests");

  // Drawing
  fresh();
  int len = curve_ui_render(&ui, &c, TH, TW, 1, buf, sizeof(buf));
  int rows = 0;
  CHECK(len > 0 && rows_ok(buf, len, 87, 45, &rows) && rows == 45,
        "panel: %d bytes, %d rows, each %d cells wide", len, rows,
        BROWS_PANEL_W);
  CHECK(has_str(buf, len, "Tone curve"), "panel is titled");
  CHECK(count_str(buf, len, "\xe2\x97\x86") == 2, "one marker per point (%d)",
        count_str(buf, len, "\xe2\x97\x86"));
  CHECK(count_braille(buf, len) > 20, "the curve is drawn in braille (%d)",
        count_braille(buf, len));
  CHECK(has_str(buf, len, "preset: linear"), "preset name shown");
  CHECK(!has_str(buf, len, "60;75;115"), "no histogram without data");
  for (int v = 90; v < 110; v++) {
    ui.hist[v] = 1000;
  }

  len = curve_ui_render(&ui, &c, TH, TW, 1, buf, sizeof(buf));
  CHECK(has_str(buf, len, "60;75;115"), "histogram drawn in its colour");
  key('a');
  key('a');
  len = curve_ui_render(&ui, &c, TH, TW, 1, buf, sizeof(buf));
  CHECK(count_str(buf, len, "\xe2\x97\x86") == 4 && has_str(buf, len, "pt 3/4"),
        "markers follow the points, info follows the selection");
  c.enabled = 0;
  len = curve_ui_render(&ui, &c, TH, TW, 1, buf, sizeof(buf));
  CHECK(has_str(buf, len, "bypassed"), "bypass is shown");
  c.enabled = 1;

  len = curve_ui_render(&ui, &c, TH, TW, 0, buf, sizeof(buf));
  CHECK(len > 0 && rows_ok(buf, len, 87, 45, &rows) &&
            !has_str(buf, len, "38;2;"),
        "without colour: same layout, no truecolor sequences");
  CHECK(has_str(buf, len, "\033[7m"), "selected point is reverse video");

  CHECK(curve_ui_render(&ui, &c, TH, 40, 1, buf, sizeof(buf)) == 0,
        "terminal too narrow: nothing");
  CHECK(curve_ui_render(&ui, &c, 16, TW, 1, buf, sizeof(buf)) == 0,
        "terminal too short: nothing");
  len = curve_ui_render(&ui, &c, 19, TW, 1, buf, sizeof(buf));
  CHECK(len > 0 && rows_ok(buf, len, 87, 14, &rows),
        "smallest terminal that fits (19 rows)");
  CHECK(curve_ui_render(&ui, &c, TH, TW, 1, buf, 200) == 0,
        "a buffer that is too small is refused, not overrun");
  len = curve_ui_render(&ui, &c, 100, 200, 1, buf, sizeof(buf));
  CHECK(len > 0 && rows_ok(buf, len, 167, 95, &rows),
        "big terminal: panel stays at the right edge");

  // Random keys and mice: curve always stays valid
  fresh();
  uint32_t rng = 0x9e3779b9;
  int good = 1;
  for (int i = 0; i < 30000; i++) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    if (rng % 3 == 0) {
      static const char keys[] = "npadxr[]<>+-,.=_12345678bwk\x7f\x08";
      key(keys[(rng >> 8) % (sizeof(keys) - 1)]);
    } else if (rng % 3 == 1) {
      curve_ui_key(&ui, &c, (brows_keykind_t)((rng >> 8) % 7), 0);
    } else {
      int x = 80 + (int)((rng >> 8) % 45);
      int y = (int)((rng >> 16) % 20);
      int btn = (int)((rng >> 20) % 4);
      mouse(x, y, btn, (int)((rng >> 24) & 1), (int)((rng >> 25) & 1));
    }

    if (!valid()) {
      good = 0;
      fprintf(stderr, "  invalid after step %d\n", i);
      break;
    }

    if (i % 500 == 0) {
      len = curve_ui_render(&ui, &c, TH, TW, (int)((rng >> 3) & 1), buf,
                            sizeof(buf));
      if (len <= 0 || !rows_ok(buf, len, 87, 45, &rows)) {
        good = 0;
        fprintf(stderr, "  bad render at step %d\n", i);
        break;
      }
    }
  }

  CHECK(good, "30000 random keys and mouse events: always a valid curve");

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
