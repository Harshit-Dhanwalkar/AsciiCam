#ifndef NETENC_H
#define NETENC_H

/*
 * netenc - delta encoder for slow links (ssh over bad wifi, serial, ...)
 *
 * Renderer repaints every cell of every frame, each with its own colour escape:
 * about 4 KB per frame in plain ASCII, 60 KB in truecolor, 120 KB in
 * half-blocks. netenc sits between renderer and write(2): it reads frame as
 * renderer produced it, remembers what terminal is already showing, and sends
 * only what changed:
 *
 *   - cell level diff (unchanged cells cost nothing)
 *   - cursor moves only where changes are not contiguous
 *   - one colour escape per run of equal colours, not one per cell
 *   - optional hysteresis: colour changes smaller than min_score are noise
 *   - optional byte budget per frame: most visible changes go first, rest stay
 *     pending and are sent in later frames, so picture builds up gracefully
 *     instead of link falling behind
 *
 * Cells encoder has not been able to send yet are simply "dirty" and are
 * retried, so every frame converges on real picture. netenc_invalidate() marks
 * everything dirty (resize, an overlay was drawn over picture, ...)
 *
 * Input is escape subset grayscale_to_ascii produces: CSI row;colH, CSI H, CSI
 * m with 0 / 38;2 / 38;5 / 48;2 / 48;5 / 39 / 49, LF, and UTF-8 glyphs one cell
 * wide
 * Anything else makes netenc_frame() return -1 and caller sends frame untouched
 */

#include "nl_types.h"

typedef struct {
  uint32_t glyph; // up to 4 UTF-8 bytes, first byte lowest
  uint32_t fg;    // kind << 24 | colour; kind 0 default, 1 rgb, 2 palette
  uint32_t bg;
} netcell_t;

typedef struct {
  // configuration
  int min_score; // changes scoring below this are ignored (0 = lossless)
  int budget;    // approx. bytes per frame (0 = unlimited)

  // state
  netcell_t *prev; // what terminal shows; fg == NETCELL_UNKNOWN if not
  netcell_t *cur;  // scratch: frame being encoded
  uint8_t *seen;   // scratch: cells frame wrote
  int cap_cells;
  int rows, cols;

  // statistics
  uint64_t frames;
  uint64_t in_bytes;
  uint64_t out_bytes;
  uint64_t cells_sent;
  int pending; // dirty cells left unsent by last frame
} netenc_t;

#define NETCELL_UNKNOWN 0xFF000000u
#define NETENC_MAX_CELLS 65536

void netenc_init(netenc_t *e, int min_score, int budget);
void netenc_free(netenc_t *e);

// Forget what terminal shows: next frames repaint everything
void netenc_invalidate(netenc_t *e);

// Encode one frame. Returns number of bytes written to out (at most cap), or -1
// when input cannot be encoded (unsupported escape, frame too big, out of
// memory); caller should then write original frame and call netenc_invalidate()
int netenc_frame(netenc_t *e, const char *in, size_t n, char *out, size_t cap);

// Cells a frame draws, for other users of same escape subset (video calls)
// netenc_extent: rows and columns frame reaches (0, 0 = nothing drawn)
// netenc_parse: fill grid (rows x cols cells, row by row) with what frame
// draws; seen[i] is 1 for cells it wrote, others are left as they were
// Return -1 on unsupported input or when frame reaches outside rows x cols
int netenc_extent(const char *in, size_t n, int *rows, int *cols);
int netenc_parse(const char *in, size_t n, netcell_t *grid, uint8_t *seen,
                 int rows, int cols);

#endif /* NETENC_H */
