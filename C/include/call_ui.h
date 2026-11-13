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

// Join room at "[host:]port" as name, sending our camera as tile_w x tile_h
// cells rendered with opts (24-bit colour when opts->color)
// Exit status
int call_main(const char *addr, const char *name, const char *device, int cap_w,
              int cap_h, int fps, int tile_w, int tile_h,
              const ascii_opts_t *opts);

#endif
