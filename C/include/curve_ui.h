#ifndef CURVE_UI_H
#define CURVE_UI_H

/*
 * Tone curve editor: "Curve" tab of side panel (browser.h)
 *
 * A braille graph (input brightness left to right, output bottom to top)
 * with live histogram of picture behind it, curve, its control points and
 * identity diagonal
 * Mouse: click a point to grab it and drag, click empty space to add a point
 * there, right-click a point to delete it Keys are in hint rows of panel,
 * mirrored by curve_ui_key
 *
 * Panel draws on right edge, same place and size as captures list
 */

#include "browser.h"
#include "curve.h"
#include "mouse.h"
#include "nl_types.h"

typedef enum {
  CUI_NONE = 0,
  CUI_CHANGED,   // curve edited: caller re-reads curve_active / lut
  CUI_CLOSE,     // close panel
  CUI_TAB_FILES, // switch to captures tab
  CUI_SAVE,      // write curve to a file (caller knows where)
} curve_act_t;

typedef struct {
  int sel;            // selected point
  int grab;           // 1 while a point is dragged with mouse
  uint32_t hist[256]; // cells per brightness, filled by ascii.c (tone_hist)
  char status[40];    // short message shown in title row
} curve_ui_t;

void curve_ui_init(curve_ui_t *ui);

// Keyboard while Curve tab is open. kind is a BR_KEY_*, ch for BR_KEY_CHAR
curve_act_t curve_ui_key(curve_ui_t *ui, curve_t *c, brows_keykind_t kind,
                         char ch);

// Mouse event anywhere on terminal
// Returns CUI_CHANGED when curve moved
// Events outside panel are ignored unless a drag is in progress
curve_act_t curve_ui_mouse(curve_ui_t *ui, curve_t *c, const mouse_event_t *ev,
                           int term_h, int term_w);

// A point is being dragged: caller must route mouse events here even when they
// leave panel
int curve_ui_grabbing(const curve_ui_t *ui);

// Draw tab
// Returns bytes written, 0 when terminal is too small
int curve_ui_render(const curve_ui_t *ui, const curve_t *c, int term_h,
                    int term_w, int color, char *out, size_t outsz);

#endif
