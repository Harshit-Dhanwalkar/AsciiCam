#ifndef CALL_UI_H
#define CALL_UI_H

/*
 * call_ui - `asciicam --call` and `asciicam --room`: parts that touch terminal,
 * camera and clock
 */

#include "ascii.h"

// Run relay on "[host:]port" until keep_running is cleared
// Exit status
int room_main(const char *addr);

// Caption lines for sender side of call are read from this file or fifo
// NULL: no captions
void call_ui_set_captions(const char *path);

// Captions from speech recognition of --listen audio
// NULL or no listen source: none
struct voice_opts_s;
void call_ui_set_listen(const struct voice_opts_s *o, const char *xdg,
                        const char *home);

// Join room at "[host:]port" as name, sending our camera as tile_w x tile_h
// cells rendered with opts (24-bit colour when opts->color)
// Exit status
int call_main(const char *addr, const char *name, const char *device, int cap_w,
              int cap_h, int fps, int tile_w, int tile_h,
              const ascii_opts_t *opts);

#endif
