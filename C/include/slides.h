#ifndef SLIDES_H
#define SLIDES_H

/*
 * Slideshow: `--image <folder>` shows pictures of a folder one after another
 *
 * List of picture names (png, jpg, jpeg, pgm, ppm, pnm) sorted so img2 comes
 * before img10, a current one, and a timer
 * List is read once and never changes afterwards. Capture side (slides_tick,
 * slides_mark_bad) and UI side (slides_request, slides_take_changed) may be
 * different threads: they meet only in atomics
 */

#include "nl_types.h"

#define SLIDES_MAX 512
#define SLIDES_DEFAULT_SECS 5

// Has name a picture extension, case does not matter
int slides_is_picture(const char *name);

// Order for file names: letters without case, digit runs by value
int slides_natural_cmp(const char *a, const char *b);

// Read folder
// Number of pictures (0 = none), -1 when dir is not a folder 
// Open again with same dir keeps list and position (capture resize)
int slides_open(const char *dir);

int slides_active(void);
int slides_count(void);
int slides_index(void);

// File name of picture i, NULL outside list
const char *slides_name(int i);

// Full path of current picture into out
// Return 0 on success
int slides_path(char *out, size_t cap);

// Seconds per picture (default SLIDES_DEFAULT_SECS)
// Return 0 = only on request
// Survives slides_open and slides_close
void slides_set_secs(int secs);

// Ask to move by delta pictures (wraps), takes effect on next slides_tick
void slides_request(int delta);

// Apply requests and timer at time now_ms
// Return 1 when current picture changed
int slides_tick(long now_ms);

// Current picture could not be loaded: skip it for good, move on
// Return 1 when another picture is left
int slides_mark_bad(long now_ms);

// Return 1 once after each change
int slides_take_changed(void);

// Close everything (tests)
void slides_close(void);

#endif
