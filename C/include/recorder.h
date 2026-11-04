#ifndef RECORDER_H
#define RECORDER_H

/*
 * Screenshots and screen recordings, no encoder library
 *
 * Recording: asciinema "asciicast v2" file (.cast), plain text: a JSON header
 * line, then one [seconds, "o", "<bytes terminal received>"] line per frame
 * It is exact (colours, braille, positions), tiny, and plays with `asciinema
 * play`, in a browser player, or converts to GIF with agg
 * Frames that did not change are skipped, time stays right
 *
 * Screenshot: two files with one timestamp,
 *   .png   camera picture after plugins (colour or grey), written by an own PNG
 *          encoder (stored deflate, CRC-32, Adler-32)
 *   .ansi  ASCII frame as shown; `cat` it in a terminal to see it again
 *
 * NOTE: Files go to <dir>, see rec_default_dir(). Names carry a UTC timestamp:
 * asciicam-20261102T153045Z.cast / .png / .ansi
 */

#include "nl_types.h"

#define REC_PATH_LEN 256

typedef struct {
  int fd;     // -1 when not recording
  int active; // 1 between rec_start and rec_stop
  uint64_t t0_ns;
  uint32_t frames;   // events written
  uint32_t skipped;  // identical frames not written
  uint64_t bytes;    // file size so far
  uint32_t last_crc; // of previous frame, to skip repeats
  size_t last_len;
  char path[REC_PATH_LEN];
} recorder_t;

// <xdg_data>/asciicam/captures, else <home>/.local/share/asciicam/captures,
// else ./captures
// 0 on success, -1 if result does not fit
int rec_default_dir(char *out, size_t outsz, const char *home,
                    const char *xdg_data);

// mkdir -p. 0 on success (also when it already exists), -1 on failure
int rec_make_dirs(const char *path);

// "20261102T153045Z" (17 bytes with NUL) from unix seconds, UTC
void rec_timestamp(char out[17], int64_t unix_secs);

// Current wall-clock seconds and monotonic nanoseconds
int64_t rec_unix_now(void);
uint64_t rec_mono_ns(void);

void rec_init(recorder_t *r);

// Start a .cast in dir for a term_w x term_h terminal
// 0 on success; -1 with r->path unchanged on failure
// Never overwrites an existing file
int rec_start(recorder_t *r, const char *dir, int term_w, int term_h,
              int64_t unix_secs, uint64_t now_ns);

// Append one frame (exact bytes sent to terminal)
// 0 on success, 1 if skipped because identical to last, -1 on write error
// (recording is stopped then)
int rec_frame(recorder_t *r, uint64_t now_ns, const char *data, size_t len);

// Finish: closes file
// Returns number of frames written, -1 if not recording. *seconds gets length
// (may be NULL)
int rec_stop(recorder_t *r, uint64_t now_ns, double *seconds);

// PNG with channels 1 (grey) or 3 (RGB), 8 bit
// 0 on success. Writes to path.tmp then renames, so a crash never leaves half a
// picture
int png_write(const char *path, int w, int h, int channels,
              const uint8_t *pixels);

// Save <dir>/asciicam-<ts>.png and .ansi. pixels may be NULL (then only  .ansi)
// png_path/ansi_path get paths written, "" for a skipped one
// 0 when everything asked for was written, -1 otherwise
int shot_save(const char *dir, int64_t unix_secs, const uint8_t *pixels, int w,
              int h, int channels, const char *ansi, size_t ansi_len,
              int term_h, char *png_path, size_t png_sz, char *ansi_path,
              size_t ansi_sz);

// "1:05" / "12:34" style clock for panel
void rec_clock(char out[16], double seconds);

#endif
