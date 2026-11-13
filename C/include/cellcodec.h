#ifndef CELLCODEC_H
#define CELLCODEC_H

/*
 * cellcodec - a picture as a grid of cells, in a compact wire form
 *
 * Video calls send what a renderer drew (glyph and colours per cell), not
 * pixels, and not terminal escapes: receiver places it wherever it wants, next
 * to other people, at its own position. A frame is either a key (every cell)
 * or a delta against sender's previous frame (runs of changed cells). Colours
 * are 24 bit or default; a frame without any colour costs one byte per ASCII
 * cell
 *
 * Wire form, big endian:
 *   byte 0   bit 0: key frame; bit 1: no colours at all
 *   u16      columns (1..CC_MAX_COLS)
 *   u16      rows (1..CC_MAX_ROWS)
 *   key:     columns * rows cell records, row by row
 *   delta:   pairs (varint skip, varint run, run cell records) until every cell
 *            is covered; a pair must make progress
 *   cell:    [tag: bit 0 fg r,g,b follows; bit 1 bg r,g,b follows] (absent in
 *            no-colour frames), UTF-8 glyph, then colour bytes
 * Decoder checks everything: a damaged or hostile frame is refused, never
 * trusted
 */

#include "netenc.h"
#include "nl_types.h"

#define CC_MAX_COLS 200
#define CC_MAX_ROWS 100

typedef struct {
  int cols, rows;
  netcell_t *cell; // cols * rows
} cc_grid_t;

// Colours in cells: 0 = terminal default, else 0x01000000 | 0xRRGGBB
#define CC_RGB(r, g, b)                                                        \
  (0x01000000u | ((uint32_t)(r) << 16u) | ((uint32_t)(g) << 8u) | (uint32_t)(b))

// Set size (cells blank: a space, default colours). 0 on success
int cc_grid_size(cc_grid_t *g, int cols, int rows);
void cc_grid_free(cc_grid_t *g);

// Cells a renderer frame (escape subset of netenc.h, positions from 1,1) draws
// Cells it does not write are blank
// Return -1 for anything unsupported, 256-colour palettes included, or a frame
// beyond size limits
int cc_grid_from_ansi(cc_grid_t *g, const char *ansi, size_t n);

// Encode cur. A delta against prev, or a key when key != 0, prev is NULL or
// size differs. Bytes written, 0 when it does not fit in cap
size_t cc_encode(const cc_grid_t *cur, const cc_grid_t *prev, int key,
                 uint8_t *out, size_t cap);

// Worst case size of encoded frame of this grid
size_t cc_max_encoded(int cols, int rows);

// Check encoded frame by a key (valid header only) 1 / 0 and -1 for a bad
// header
int cc_is_key(const uint8_t *in, size_t n);

// Apply an encoded frame to state. 0 on success; -1 malformed (state is left
// as it was); -2 delta that does not fit state (wrong size, or none yet):
// wait for a key
int cc_decode(cc_grid_t *state, const uint8_t *in, size_t n);

// Escapes that draw grid with its top-left cell at screen row y, column x (1
// based), clipped to maxw x maxh cells
// Bytes written, 0 when out of room
size_t cc_draw(const cc_grid_t *g, int x, int y, int maxw, int maxh, char *out,
               size_t cap);

// Room cc_draw may need
size_t cc_max_draw(int cols, int rows);

#endif
