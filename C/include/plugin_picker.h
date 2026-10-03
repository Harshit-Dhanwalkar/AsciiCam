#ifndef PLUGIN_PICKER_H
#define PLUGIN_PICKER_H

/*
 * Search-as-you-type plugin picker and a one-line toast, both rendered as an
 * overlay on bottom rows of ASCII frame. Logic: key handling works on integers
 * and rendering fills a caller buffer, so none of it needs a terminal to test
 */

#include "plugin_catalog.h"

#define PICKER_QUERY_MAX 32
#define PICKER_MAX_ROWS 8

#define PK_KEY_UP (-1)
#define PK_KEY_DOWN (-2)
#define PK_KEY_ESC (-3)

typedef struct {
  int active;
  char query[PICKER_QUERY_MAX];
  int qlen;
  int sel; // index into the current match list
} picker_t;

typedef enum {
  PK_NONE = 0, // picker closed, key was not for us
  PK_CONSUMED, // picker handled the key, nothing for the caller to do
  PK_LOAD,     // user pressed Enter on entry *load_idx
  PK_CLOSED    // picker was closed (Esc, or Enter with no match)
} picker_action_t;

void picker_open(picker_t *p);
void picker_close(picker_t *p);

// Case-insensitive substring match; an empty query matches everything
int picker_name_match(const char *name, const char *query);

// Fill idx_out with catalog indices matching the query. Returns the count
int picker_matches(const picker_t *p, const plugin_catalog_t *c, int *idx_out,
                   int max);

// Feed one key: printable ASCII, 8/127 backspace, 10/13 enter, or PK_KEY_*
picker_action_t picker_key(picker_t *p, const plugin_catalog_t *c, int key,
                           int *load_idx);

// Render the picker box over the bottom rows of an ascii_h-row frame. Returns
// bytes written to buf (0 if there is no room)
int picker_render(const picker_t *p, const plugin_catalog_t *c, int ascii_h,
                  int color, char *buf, size_t size);

// Render a one-line message on the last frame row
int toast_render(const char *msg, int ascii_h, int color, char *buf,
                 size_t size);

#endif
