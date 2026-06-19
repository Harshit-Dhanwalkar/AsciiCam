#ifndef ASCII_H
#define ASCII_H

#include <stddef.h>
#include <stdint.h>

#define ASCII_CHARS_DEFAULT " .:-=+*#%@"
#define MAX_CHARSETS 16
#define CHARSET_NAME_LEN 32
#define CHARSET_RAMP_LEN 128

// Render mode
typedef enum {
  RENDER_BRAILLE = 0,
  RENDER_BLOCKS,
  RENDER_ASCII_RAMP,
  RENDER_HALF_BLOCK,
  RENDER_DOTS,
  RENDER_MODE_COUNT
} render_mode_t;

// Edge detection mode
typedef enum {
  EDGE_OFF = 0,
  EDGE_SOBEL,
  EDGE_SOBEL_DIR,
  EDGE_LAPLACIAN,
  EDGE_MODE_COUNT
} edge_mode_t;

typedef struct {
  char name[CHARSET_NAME_LEN];
  char ramp[CHARSET_RAMP_LEN];
} charset_entry_t;

// Registry of loaded charsets, hot-reloadable from directory of .txt files.
typedef struct {
  charset_entry_t sets[MAX_CHARSETS];
  int count;
  int active;
  int inotify_fd;
  int inotify_wd;
  char dir_path[256];
} charset_registry_t;

int charset_registry_init(charset_registry_t *reg, const char *dir);
void charset_registry_scan(
    charset_registry_t *reg); // (re)loads all files in dir
void charset_registry_check_reload(
    charset_registry_t *reg); // inotify poll, call once per frame
void charset_registry_cleanup(charset_registry_t *reg);
const char *charset_registry_active_ramp(const charset_registry_t *reg);

typedef struct {
  int brightness;      /* additive offset: -128..128       */
  int contrast;        /* percent, 100 = no change         */
  int invert;          /* flip brightness->charset mapping */
  int color;           /* ANSI truecolor output            */
  edge_mode_t edges;   /* edge detection mode              */
  int dither;          /* Floyd-Steinberg dithering        */
  int threshold_val;   /* Binarization limit               */
  const char *charset; /* NULL = ASCII_CHARS_DEFAULT; active ramp for
                          RENDER_ASCII_RAMP */
  render_mode_t render_mode;

  // TESTING:
  /* Pseudo-3D "pop out" parallax effect.
   * depth_pop:  0 = off, >0 = effect strength (1..100 suggested range)
   * depth_invert: 0 = brighter is "closer" (pops toward viewer), 1 = darker is
   * closer
   */
  int depth_pop;
  int depth_invert;
} ascii_opts_t;

// Convert YUYV raw data to grayscale
void yuyv_to_gray(const uint8_t *yuyv, uint8_t *gray, int width, int height);
void yuyv_to_gray_simd(const uint8_t *yuyv, uint8_t *gray, int width,
                       int height);
void yuyv_to_rgb(const uint8_t *yuyv, uint8_t *rgb, int width, int height);

// Output buffer sizing
size_t ascii_out_size(int dst_w, int dst_h, int color);
size_t ascii_out_size_for_mode(int dst_w, int dst_h, int color,
                               render_mode_t mode);

// grayscale to ASCII output grid
int grayscale_to_ascii(const uint8_t *gray, const uint8_t *rgb, int src_w,
                       int src_h, int dst_w, int dst_h, char *out,
                       size_t out_size, const ascii_opts_t *opts);

// Overlay FPS box
void overlay_fps_box(int dst_w, double fps, int color_enabled);

// Render mode name for UI display
const char *render_mode_name(render_mode_t m);
const char *edge_mode_name(edge_mode_t m);

#endif
