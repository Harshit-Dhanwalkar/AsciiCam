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

#endif
