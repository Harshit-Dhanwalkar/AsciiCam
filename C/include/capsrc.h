#ifndef CAPSRC_H
#define CAPSRC_H

/*
 * capsrc - caption lines from a speech recogniser
 *
 * `asciicam --call ... --captions <path>` reads lines of text from a file or
 * fifo, so any recogniser can feed it (libasciiasr's tools, whisper.cpp, a
 * script): one line is one caption. A line that starts with "~" is a hypothesis
 * recogniser may still change (partial), any other line is final
 * Blank lines are ignored; carriage returns are dropped; a line longer than
 * CAPSRC_LINE is cut
 */

#include "nl_types.h"

#define CAPSRC_LINE 200

typedef struct {
  int fd;                    // -1 when closed
  char buf[CAPSRC_LINE + 1]; // line being collected
  size_t n;
  int cut; // current line is already too long
} capsrc_t;

// Called for each line: final is 0 for "~" lines
typedef void (*capsrc_fn)(void *user, int final, const char *text);

void capsrc_init(capsrc_t *c);

// Take bytes as they come; calls fn for each whole line
void capsrc_feed(capsrc_t *c, const char *data, size_t n, capsrc_fn fn,
                 void *user);

// Open path for reading without blocking (a fifo is opened read-write so that
// it does not end when its writer does)
// Return 0 on success
int capsrc_open(capsrc_t *c, const char *path);

// Read what is there now, calls fn per line
// Number of bytes read
long capsrc_poll(capsrc_t *c, capsrc_fn fn, void *user);

void capsrc_close(capsrc_t *c);

/*
 * Captions on local screen (not in a call): last two lines, a partial one is
 * replaced by what comes next, they fade
 */

#define CAPVIEW_ROWS 2
#define CAPVIEW_FINAL_MS 6000
#define CAPVIEW_PARTIAL_MS 3000

typedef struct {
  char text[CAPVIEW_ROWS][CAPSRC_LINE + 1]; // oldest first; "" = unused
  int final[CAPVIEW_ROWS];
  long at[CAPVIEW_ROWS];
  int n;
} capview_t;

void capview_init(capview_t *v);

// A caption line arrives (a partial line replaces a partial line before it)
void capview_push(capview_t *v, int final, const char *text, long now_ms);

// Let old lines go. 1 when something went (screen needs repainting)
int capview_tick(capview_t *v, long now_ms);

// Number of lines to show
int capview_count(const capview_t *v);

// Escape sequences drawing lines in bottom_row-1 .. bottom_row rows inside
// width columns starting at col, bottom aligned (last line is newest)
// A long line shows its end
// Return bytes, 0 when nothing to show or room is too small
int capview_render(const capview_t *v, int bottom_row, int col, int width,
                   int color, char *buf, size_t size);

// Where "asciicam --captions" reads by default (key y without a path):
// $XDG_RUNTIME_DIR/asciicam-captions, else ~/.asciicam-captions. 0 on success,
// -1 when neither is known
int capsrc_default_path(char *out, size_t size, const char *xdg_runtime,
                        const char *home);

// Make a fifo at path (mode 0600)
// An existing file there is fine
// Return 0 or -1 and always -1 off Linux
int capsrc_make_fifo(const char *path);

#endif
