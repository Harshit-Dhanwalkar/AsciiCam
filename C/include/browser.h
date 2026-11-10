#ifndef BROWSER_H
#define BROWSER_H

/*
 * Retractable side panel that lists screenshots and recordings saved by
 * rec_start / shot_save, with in-terminal preview / replay.
 *
 *   .ansi  -> cat file to screen, wait for any key (same visual .png would
 *             show, since .ansi is ASCII rendering with colours and cursor
 *             positions baked in)
 *   .cast  -> replay asciinema v2 events with their original timing, in main
 *             terminal area; any key aborts playback
 *   .png   -> not previewable in this build (would need a PNG decoder); panel
 *             shows an info toast pointing at .ansi sibling
 *
 * Toggle with 'b' when panel is closed
 * While open, panel owns keyboard: up/down or j/k move, enter opens, esc or 'b'
 * closes. Panel overlays right side of live ASCII frame; opening it does not
 * resize frame, so part of frame may be hidden behind it - press 'a' to
 * auto-fit frame to remaining width if wanted
 */

#include "nl_types.h"

#define BROWS_MAX_FILES 64
#define BROWS_NAME_LEN 48
#define BROWS_PATH_LEN 256
#define BROWS_PANEL_W 34 // columns side panel occupies on screen

// Tabs of side panel: captures list and tone curve editor (curve_ui.h)
#define BROWS_TAB_FILES 0
#define BROWS_TAB_CURVE 1
#define BROWS_TAB_COUNT 2
#define BROWS_STRIP_W 3  // columns of the tab strip
#define BROWS_TAB_ROWS 7 // rows of one tab in the strip

typedef enum {
  BROWS_UNKNOWN,
  BROWS_CAST, // asciinema v2 recording - replayable
  BROWS_PNG,  // screenshot image - not previewable without a PNG decoder
  BROWS_ANSI, // screenshot text - cat to terminal
} brows_ftype_t;

typedef struct {
  char name[BROWS_NAME_LEN]; // display name (filename)
  char path[BROWS_PATH_LEN]; // full path for open()
  brows_ftype_t type;
} brows_entry_t;

typedef enum {
  BR_KEY_CHAR, // regular character (in ch)
  BR_KEY_UP,
  BR_KEY_DOWN,
  BR_KEY_ESC,
  BR_KEY_ENTER,
  BR_KEY_LEFT,
  BR_KEY_RIGHT,

} brows_keykind_t;

typedef enum {
  BROWS_ACT_NONE,
  BROWS_ACT_CLOSE,        // user closed panel - hide it
  BROWS_ACT_PREVIEW_ANSI, // enter on .ansi - out_path is set
  BROWS_ACT_REPLAY_CAST,  // enter on .cast - out_path is set
  BROWS_ACT_INFO,         // enter on .png - b->info_msg is set
} brows_action_t;

typedef struct {
  int active;
  int tab; // BROWS_TAB_*: which tab is showing
  int count;
  int selected;
  int scroll;
  brows_entry_t e[BROWS_MAX_FILES];
  char status[64];    // "12 files" / "no captures yet" / last scan error
  char info_msg[128]; // set when BROWS_ACT_INFO is returned
} browser_t;

void browser_init(browser_t *b);

// Re-scan cap_dir, repopulate e[]
// Resets selected/scroll
// Returns 0 on success
int browser_scan(browser_t *b, const char *cap_dir);

// Flip active. On open, resets selection to top
void browser_toggle(browser_t *b);

// Keyboard input while panel is open
// Returns action; for PREVIEW_ANSI and REPLAY_CAST path is written to out_path
brows_action_t browser_key(browser_t *b, brows_keykind_t kind, char ch,
                           char *out_path, size_t out_sz);

// Render panel into out
// Returns bytes written (0 if panel inactive or terminal too narrow)
// Draws at columns [term_w - BROWS_PANEL_W + 1, term_w]
int browser_render(const browser_t *b, int term_h, int term_w, int color,
                   char *out, size_t outsz);

// First column and number of rows (panel is drawn on rows 1..panel_h)
// Returns 0 when terminal is too small for it
int browser_panel_geom(int term_h, int term_w, int *place_col, int *panel_h);

// 1 when terminal cell (x, y), 1-based, belongs to open panel
int browser_panel_hit(int term_h, int term_w, int x, int y);

// Tab strip: 0 when terminal is too small for it
// Tab under cell (x, y) of strip, or -1
// Open = panel is showing (strip is next to it, otherwise at screen edge)
int browser_strip_tab_at(int term_h, int term_w, int open, int x, int y);

// Draw strip with tab highlighted (only when panel is open)
// Return bytes written, 0 when it does not fit
int browser_strip_render(char *out, size_t outsz, int term_h, int term_w,
                         int open, int tab, int color);

// Clear panel's screen area after close
// Called once when active goes 1 -> 0
void browser_clear_area(int term_h, int term_w);

// Take over screen to preview / replay
// Returns 0 on success, -1 on error (file missing etc)
// *aborted = 1 if user pressed a key to bail
// On return caller is expected to redraw live UI
int browser_preview_ansi(const char *path, int *aborted);
int browser_replay_cast(const char *path, int *aborted);

// Standalone playback (asciicam --play): speed 1.0 = recorded pace, loop
// restarts at end
// Keys: q quits, space pauses, + / - change speed
// Needs terminal in raw mode (non-blocking stdin)
// *events = events written
// Returns 0, or -1 when file cannot be opened
int browser_play_cast(const char *path, double speed, int loop, int *events);

// Terminal size recorded in header of a .cast (0, 0 when unknown)
void browser_cast_size(const char *path, int *w, int *h);

#endif
