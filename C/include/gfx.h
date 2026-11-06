#ifndef GFX_H
#define GFX_H

/* gfx - pixel output for terminals that can show images.
 *
 * Instead of characters, (plugin-processed, optionally coloured) frame is sent
 * as a real image:
 *
 *   kitty  Kitty graphics protocol (kitty, WezTerm, Ghostty, Konsole, ...):
 *          raw 24-bit RGB, base64, chunked; terminal scales it to cells it is
 * placed on, so picture stays small on wire
 *
 *   sixel  DEC sixel (foot, mlterm, xterm -ti vt340, Windows Terminal, ...):
 *          fixed palette (216-colour cube, or 64 greys for a luma picture),
 *          run-length coded; pixel size must match cells exactly
 *
 * Encoders are pure functions on caller buffers, no I/O */

#include "nl_types.h"

typedef enum { GFX_OFF = 0, GFX_KITTY = 1, GFX_SIXEL = 2 } gfx_mode_t;

#define GFX_MAX_W 2048 // widest picture encoders accept
#define GFX_MAX_H 2048

// "kitty", "sixel", "off" -> mode; "auto" picks from environment
// (KITTY_WINDOW_ID, TERM, TERM_PROGRAM)
// Returns -1 for anything else
int gfx_parse(const char *name, const char *term, const char *term_program,
              int have_kitty_window_id);

// Resample src (sw x sh) to dst (dw x dh) RGB by area averaging
// src is RGB (3 bytes per pixel) when rgb != NULL, else luma plane gray
// Sizes above GFX_MAX_* or below 1 return -1
int gfx_resample(const uint8_t *gray, const uint8_t *rgb, int sw, int sh,
                 int dw, int dh, uint8_t *dst);

// Largest output of each encoder for a w x h picture (buffer size to pass)
size_t gfx_kitty_cap(int w, int h);
size_t gfx_sixel_cap(int w, int h);

// Encode an RGB picture (w x h, 3 bytes per pixel)
// Image is placed with its top-left corner at current cursor and scaled over
// cols x rows cells (kitty only; sixel ignores them)
// Kitty images keep id 1 and replace previous frame; z = -1 keeps text drawn on
// top of picture
// mono != 0 tells sixel encoder picture is grey (R == G == B) so it uses a
// 64-step grey palette

// Return number of bytes written, or -1 when it does not fit in cap
int gfx_kitty_encode(const uint8_t *rgb, int w, int h, int cols, int rows,
                     char *out, size_t cap);
int gfx_sixel_encode(const uint8_t *rgb, int w, int h, int mono, char *out,
                     size_t cap);

// Remove kitty image (call on exit)
int gfx_kitty_delete(char *out, size_t cap);

#endif /* GFX_H */
