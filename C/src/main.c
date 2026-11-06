#include "nolibc.h"

#include "app_paths.h"
#include "ascii.h"
#include "browser.h"
#include "capture.h"
#include "curve.h"
#include "curve_ui.h"
#include "demo.h"
#include "gfx.h"
#include "image.h"
#include "mouse.h"
#include "netenc.h"
#include "plugin_catalog.h"
#include "plugin_picker.h"
#include "plugins.h"
#include "recorder.h"
#include "slides.h"
#include "thread_sharing.h"
#include "timing.h"
#include "vcam.h"

// Defaults
#define DEFAULT_ASCII_WIDTH 80
#define DEFAULT_ASCII_HEIGHT 40
#define DEFAULT_CAPTURE_WIDTH 640
#define DEFAULT_CAPTURE_HEIGHT 480
#define DEFAULT_FPS 20
#define MAX_PLUGINS 8

#define DEFAULT_CONFIG_PATH ".asciicamrc"
#define DEFAULT_CHARSET_DIR "./charsets"
#define DEFAULT_PLUGIN_DIR "<sdk>/filters"      // plugin sources to browse
#define DEFAULT_PLUGIN_CACHE "./.cache/plugins" // where runtime builds land
// Plugin sources, build cache, prebuilt libraries and charsets are located
// relative to binary as well as current directory (app_paths.h)

#define TOAST_SECONDS 3
#define CONFIG_MAX_BYTES 4096

#define PANEL_ROWS 4
#define MIN_ASCII_W 10
#define MIN_ASCII_H 5
#define MIN_CAPTURE_W 160 // sanity limits for capture_reinit
#define MIN_CAPTURE_H 120
#define MAX_CAPTURE_W 1280
#define MAX_CAPTURE_H 720
#define DRAG_MAX_ASCII_W 500 // upper bound mouse can drag frame to
#define DRAG_MAX_ASCII_H 200

#ifndef PLATFORM_MACOS

#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#endif

#ifndef SIGWINCH
#define SIGWINCH 28
#endif

struct winsize {
  unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};

#endif

// Signal handling
volatile sig_atomic_t keep_running = 1;
// void handle_signal(int sig) {
//   (void)sig;
//
//   keep_running = 0;
// }

static char g_curve_cfg[256];             // curve= from .asciicamrc
static const char *g_plugin_dir = NULL;   // NULL: chosen by app_paths_resolve
static const char *g_plugin_cache = NULL; // NULL: chosen by app_paths_resolve

static volatile sig_atomic_t raw_mode_active = 0;
static struct termios orig_terminal;

// write(2) may take only part of a big frame (a signal such as SIGWINCH lands
// mid-write, or pty is busy). A cut-off frame leaves stale rows and, with
// colour, half an escape sequence: loop until everything is out
static void write_all(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }

      return;
    }

    p += w;
    n -= (size_t)w;
  }
}

// Synchronized output (DEC private mode 2026): terminals that support it show
// whole frame at once instead of repainting half-drawn rows; others ignore
// sequence
#define SYNC_BEGIN "\033[?2026h"
#define SYNC_END "\033[?2026l"

// invert is a blend: at generic default of 128 (~50%) it flattens image to
// mid-grey, so it starts fully on (as it does when run without a param)
static int plugin_default_param(const filter_plugin_t *p) {
  if (p && p->name && nl_strcmp(p->name, "invert") == 0) {
    return 255;
  }

  return 128;
}

static void emergency_terminal_restore(void) {
  if (raw_mode_active) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_terminal);
  }

  // otherwise shell keeps receiving mouse reports as garbage text
  mouse_disable();

  static const char SHOW_CURSOR[] = SYNC_END "\033[?25h\033[0m\n";
  (void)write(STDOUT_FILENO, SHOW_CURSOR, sizeof(SHOW_CURSOR) - 1);
}

void handle_signal(int sig) {
  switch (sig) {
  case SIGSEGV:
  case SIGABRT:
  case SIGQUIT:
  case SIGHUP:
    emergency_terminal_restore();
    nl_exit(128 + sig);

    break;

  default:
    keep_running = 0;
  }
}

// SIGWINCH: terminal resized
volatile sig_atomic_t term_resized = 0;
volatile sig_atomic_t winch_count = 0;
void handle_winch(int sig) {
  (void)sig;

  term_resized = 1;
  winch_count++;
}

static int my_atoi(const char *s) {
  int n = 0;
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  }

  while (*s >= '0' && *s <= '9') {
    n = n * 10 + (*s++ - '0');
  }

  return neg ? -n : n;
}

// Usage
static void print_usage(const char *prog) {
  fprintf(
      stderr,
      "Usage: %s [options]\n"
      "\n"
      "Capture options:\n"
      "  -d <device>   video device             (default: /dev/video0)       \n"
      "  --demo        built-in animated picture, no camera needed           \n"
      "                (same as -d demo)                                     \n"
      "  --image <f>   a picture as the camera: PNG, JPEG or PGM/PPM, kept   \n"
      "                in proportion; a folder is a slideshow, . and , step  \n"
      "                through it (same as -d image:<f>)                     \n"
      "  --slide <s>   seconds per picture of a slideshow (default 5, 0 =    \n"
      "                only on key)                                          \n"
      "  -w <width>    capture width            (default: %d)                \n"
      "  -h <height>   capture height           (default: %d)                \n"
      "  -r <fps>      target framerate         (default: %d)                \n"
      "  -T            capture in main loop     (Linux default: a separate   \n"
      "                capture thread keeps UI responsive)                   \n"
      "  --play <file> play a saved .cast recording, no camera needed (also  \n"
      "                over ssh); keys: q quit, space pause, + / - speed     \n"
      "  --speed <x>   playback speed for --play (default 1, e.g. 0.5, 2)    \n"
      "  --loop        repeat --play until quit                              \n"
      "  --curve <s>   tone curve: points \"0,0 64,40 255,255\", a preset    \n"
      "                name (s-curve asinh sqrt invert threshold solarize) or\n"
      "                @file                                                 \n"
      "  --net <kbps>  slow-link mode: send only changed pixels,             \n"
      "                <kbps> kbit/s (0 = lossless changes only)             \n"
      "  --vcam <dev>  also send the picture to a virtual camera, for Meet,  \n"
      "                Teams, Zoom (v4l2loopback device, /dev/video10)       \n"
      "  --vcam-size <WxH>  virtual camera size (default 1280x720)           \n"
      "\n"
      "Output options:\n"
      "  -W <width>    ASCII output columns     (default: %d)                \n"
      "  -H <height>   ASCII output rows        (default: %d)                \n"
      "  -s <chars>    custom charset string    (default: \"%s\")            \n"
      "  -p <path>     filter plugin .so path   (repeatable)                 \n"
      "  -L <dir>      plugin source dir for live picker (default: %s)       \n"
      "  -m <mode>     render mode: braille|blocks|ascii|halfblock|dots      \n"
      "  -k <dir>      charset directory (hot-reloadable .txt ramps)         \n"
      "  --debug-input <file>  log raw input bytes and mouse events there    \n"
      "  --gfx <type>  pixels instead of characters: kitty | sixel | auto    \n"
      "                (kitty, WezTerm, Ghostty / foot, mlterm; auto looks   \n"
      "                at terminal, off if it has no pictures)               \n"
      "\n"
      "Image adjustments:\n"
      "  -b <val>      brightness offset        -128..128  (default: 0)      \n"
      "  -c <val>      contrast in percent      >0; 100=none (default: 100)  \n"
      "  -g <val>      gamma percent            10..400; 100=none (default: "
      "100)\n"
      "  -i            invert mapping                                        \n"
      "  -E <mode>     edge mode                off|sobel|sobel-dir|laplacian\n"
      "  -C            ANSI truecolor output                                 \n"
      "  -2            ANSI-256 color output (fallback for terminals         \n"
      "                without truecolor support, implies color on)          \n"
      "  -D            Floyd-Steinberg dithering                             \n"
      "  -P <0-100>    depth-pop 3D parallax strength (0=off)                \n"
      "  -f            mirror/flip frame horizontally                        \n"
      "\n"
      "Config file:                                                          \n"
      "  ./.asciicamrc, key=value per line (# comments), auto-loaded         \n"
      "  if present. CLI flags above always override it.                     \n"
      "\n"
      "Live keybindings:                                                     \n"
      "  m / M         cycle render mode forward / backward                  \n"
      "  x / X         cycle edge detection mode forward / backward          \n"
      "  n / N         cycle loaded charset forward / backward               \n"
      "  p / o         increase / decrease depth-pop strength                \n"
      "  g / G         decrease / increase gamma                             \n"
      "  e / E         hw exposure down / up        (V4L2, Linux only)       \n"
      "  w / W         hw white-balance down / up   (V4L2, Linux only)       \n"
      "  c / C         hw contrast down / up        (V4L2, Linux only)       \n"
      "  up/down       select plugin    [ ] +-1   { } +-10   r reset         \n"
      "  /             search plugin sources, enter builds + loads it        \n"
      "                (up/down move, esc closes; compiles in background)    \n"
      "  u             unload the selected plugin                            \n"
      "  mouse         drag any corner to resize frame, drag frame           \n"
      "                itself to move it anywhere; applied on release        \n"
      "  a / A         return frame to terminal auto-fit (top-left)          \n"
      "  s             screenshot: .png + .ansi in ~/Pictures/asciicam       \n"
      "  t             start / stop recording (.cast, asciinema v2); a       \n"
      "                red REC clock shows in panel while it runs            \n"
      "                .cast plays with `asciinema play file.cast`;          \n"
      "                convert to mp4/gif with `agg file.cast out.gif`       \n"
      "                then `ffmpeg -i out.gif out.mp4`                      \n"
      "  b             toggle side panel: browse / replay saved              \n"
      "                captures. ↑↓ or jk move, enter opens (.ansi           \n"
      "                previews, .cast replays with timing), esc closes      \n"
      "  k             toggle side panel on the tone curve tab: drag points, \n"
      "                click to add, right-click deletes, [ ] presets,       \n"
      "                x bypass, w saves curve.txt. tab switches tabs        \n"
      "  q             quit                                                  "
      "\n",
      prog, DEFAULT_CAPTURE_WIDTH, DEFAULT_CAPTURE_HEIGHT, DEFAULT_FPS,
      DEFAULT_ASCII_WIDTH, DEFAULT_ASCII_HEIGHT, ASCII_CHARS_DEFAULT,
      DEFAULT_PLUGIN_DIR);
}

static render_mode_t parse_render_mode(const char *s) {
  if (nl_strcmp(s, "braille") == 0) {
    return RENDER_BRAILLE;
  }
  if (nl_strcmp(s, "blocks") == 0) {
    return RENDER_BLOCKS;
  }
  if (nl_strcmp(s, "ascii") == 0) {
    return RENDER_ASCII_RAMP;
  }
  if (nl_strcmp(s, "halfblock") == 0) {
    return RENDER_HALF_BLOCK;
  }
  if (nl_strcmp(s, "dots") == 0) {
    return RENDER_DOTS;
  }

  return RENDER_BRAILLE;
}

static edge_mode_t parse_edge_mode(const char *s) {
  if (nl_strcmp(s, "off") == 0) {
    return EDGE_OFF;
  }
  if (nl_strcmp(s, "sobel") == 0) {
    return EDGE_SOBEL;
  }
  if (nl_strcmp(s, "sobel-dir") == 0) {
    return EDGE_SOBEL_DIR;
  }
  if (nl_strcmp(s, "laplacian") == 0) {
    return EDGE_LAPLACIAN;
  }

  return EDGE_OFF;
}

// Config file support
static void _cfg_trim(char *s) {
  for (char *p = s; *p; p++) {
    if (*p == '\n' || *p == '\r') {
      *p = '\0';

      break;
    }
  }

  size_t len = nl_strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
    s[--len] = '\0';
  }
}

static char *_cfg_skip_ws(char *s) {
  while (*s == ' ' || *s == '\t') {
    s++;
  }

  return s;
}

// Write curve as one line into <dir>/curve.txt (for --curve @file)
static int save_curve(const curve_t *c, const char *dir) {
  char path[REC_PATH_LEN + 16];
  nl_snprintf(path, sizeof(path), "%s/curve.txt", dir);
  char spec[160];
  int len = curve_format(c, spec, sizeof(spec) - 1);
  spec[len++] = '\n';
  int fd = open(
      path, (int)((unsigned)O_WRONLY | (unsigned)O_CREAT | (unsigned)O_TRUNC),
      0644);
  if (fd < 0) {
    return -1;
  }

  ssize_t w = write(fd, spec, (size_t)len);
  close(fd);

  return w == len ? 0 : -1;
}

// Contents of a small text file, trailing whitespace cut. Length or -1
static int read_small_file(const char *path, char *buf, size_t sz) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  ssize_t n = read(fd, buf, sz - 1);
  close(fd);
  if (n < 0) {
    return -1;
  }

  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                   buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
    n--;
  }

  buf[n] = '\0';

  return (int)n;
}

static void load_config_file(const char *path, char **device, int *ascii_w,
                             int *ascii_h, int *cap_w, int *cap_h, int *fps,
                             ascii_opts_t *opts, const char **charset_dir,
                             const char *plugin_paths[],
                             int *plugin_path_count) {
  // Static storage
  static char buf[CONFIG_MAX_BYTES];
  static char cfg_device[256];
  static char cfg_charset[CHARSET_RAMP_LEN];
  static char cfg_charset_dir[256];
  static char cfg_plugins[MAX_PLUGINS][256];
  static char cfg_plugin_dir[256];
  static char cfg_plugin_cache[256];

  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    return; // no config file present, use CLI flags defaults
  }

  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    return;
  }

  buf[n] = '\0';

  char *line = buf;
  while (line && *line) {
    char *next = line;
    while (*next && *next != '\n') {
      next++;
    }

    char had_nl = *next;
    if (had_nl) {
      *next = '\0';
    }

    char *l = _cfg_skip_ws(line);
    _cfg_trim(l);

    if (l[0] != '\0' && l[0] != '#') {
      char *eq = l;
      while (*eq && *eq != '=') {
        eq++;
      }

      if (*eq != '=') {
        fprintf(stderr, "[config] malformed line (missing '='): %s\n", l);
      } else {
        *eq = '\0';
        char *key = l;
        const char *val = _cfg_skip_ws(eq + 1);
        _cfg_trim(key); // strip whitespace left between key and '='

        if (nl_strcmp(key, "device") == 0) {
          nl_strncpy_safe(cfg_device, val, sizeof(cfg_device));
          *device = cfg_device;
        } else if (nl_strcmp(key, "capture_width") == 0) {
          *cap_w = my_atoi(val);
        } else if (nl_strcmp(key, "capture_height") == 0) {
          *cap_h = my_atoi(val);
        } else if (nl_strcmp(key, "ascii_width") == 0) {
          *ascii_w = my_atoi(val);
        } else if (nl_strcmp(key, "ascii_height") == 0) {
          *ascii_h = my_atoi(val);
        } else if (nl_strcmp(key, "fps") == 0) {
          *fps = my_atoi(val);
        } else if (nl_strcmp(key, "brightness") == 0) {
          opts->brightness = my_atoi(val);
        } else if (nl_strcmp(key, "contrast") == 0) {
          opts->contrast = my_atoi(val);
        } else if (nl_strcmp(key, "invert") == 0) {
          opts->invert = my_atoi(val) != 0;
        } else if (nl_strcmp(key, "color") == 0) {
          opts->color = my_atoi(val) != 0;
        } else if (nl_strcmp(key, "color_mode") == 0) {
          opts->color_mode =
              (nl_strcmp(val, "256") == 0) ? COLOR_256 : COLOR_TRUECOLOR;
        } else if (nl_strcmp(key, "dither") == 0) {
          opts->dither = my_atoi(val) != 0;
        } else if (nl_strcmp(key, "threshold") == 0) {
          opts->threshold_val = my_atoi(val);
        } else if (nl_strcmp(key, "depth_pop") == 0) {
          opts->depth_pop = my_atoi(val);
        } else if (nl_strcmp(key, "depth_invert") == 0) {
          opts->depth_invert = my_atoi(val) != 0;
        } else if (nl_strcmp(key, "gamma") == 0) {
          opts->gamma = my_atoi(val);
          if (opts->gamma < 10) {
            opts->gamma = 10;
          }
          if (opts->gamma > 400) {
            opts->gamma = 400;
          }
        } else if (nl_strcmp(key, "curve") == 0) {
          nl_strncpy_safe(g_curve_cfg, val, sizeof(g_curve_cfg));
        } else if (nl_strcmp(key, "render_mode") == 0) {
          opts->render_mode = parse_render_mode(val);
        } else if (nl_strcmp(key, "edge_mode") == 0) {
          opts->edges = parse_edge_mode(val);
        } else if (nl_strcmp(key, "charset") == 0) {
          nl_strncpy_safe(cfg_charset, val, sizeof(cfg_charset));
          opts->charset = cfg_charset;
        } else if (nl_strcmp(key, "charset_dir") == 0) {
          nl_strncpy_safe(cfg_charset_dir, val, sizeof(cfg_charset_dir));
          *charset_dir = cfg_charset_dir;
        } else if (nl_strcmp(key, "plugin_dir") == 0) {
          nl_strncpy_safe(cfg_plugin_dir, val, sizeof(cfg_plugin_dir));
          g_plugin_dir = cfg_plugin_dir;
        } else if (nl_strcmp(key, "plugin_cache") == 0) {
          nl_strncpy_safe(cfg_plugin_cache, val, sizeof(cfg_plugin_cache));
          g_plugin_cache = cfg_plugin_cache;
        } else if (nl_strcmp(key, "plugin") == 0) {
          if (*plugin_path_count < MAX_PLUGINS) {
            nl_strncpy_safe(cfg_plugins[*plugin_path_count], val,
                            sizeof(cfg_plugins[0]));
            plugin_paths[*plugin_path_count] = cfg_plugins[*plugin_path_count];
            (*plugin_path_count)++;
          } else {
            fprintf(stderr, "[config] max %d plugins, ignoring extra: %s\n",
                    MAX_PLUGINS, val);
          }
        } else {
          fprintf(stderr, "[config] unknown key '%s', ignoring\n", key);
        }
      }
    }

    line = had_nl ? next + 1 : NULL;
  }
}

// termios
void term_raw_mode(void) {
  tcgetattr(STDIN_FILENO, &orig_terminal); // save stdin state

  struct termios raw = orig_terminal;
  raw.c_lflag &= ~(ICANON | ECHO); // no line buffering or no echo
  raw.c_cc[VMIN] = 0;              // non-blocking read
  raw.c_cc[VTIME] = 0;

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  raw_mode_active = 1;
}

void term_restore(void) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_terminal);
  raw_mode_active = 0;
}

// panel_row: first of PANEL_ROWS rows (1-based) status panel is drawn on
// drag     : current mouse drag, shown instead of hint bar while one is active
// Last REC_TOAST_W bytes of a path with "..." in front when it was cut: toast
// box is only 60 columns wide and file name is part that matters
#define REC_TOAST_W 50

static const char *path_tail(const char *path, char *buf, size_t bufsz) {
  size_t n = nl_strlen(path);
  if (n <= REC_TOAST_W || bufsz < REC_TOAST_W + 4) {
    return path;
  }

  nl_snprintf(buf, bufsz, "...%s", path + n - REC_TOAST_W);

  return buf;
}

// Red blinking dot, "REC" and elapsed time at right end of panel's first row
static void draw_rec_indicator(int panel_row, int term_w, const recorder_t *r,
                               uint64_t now_ns, int color) {
  if (!r->active || panel_row < 1) {
    return;
  }

  double secs = now_ns > r->t0_ns ? (double)(now_ns - r->t0_ns) / 1e9 : 0.0;
  char clk[16];
  rec_clock(clk, secs);
  int lit = ((int)(secs * 2.0) & 1) == 0; // half-second blink
  int col = term_w > 14 ? term_w - 13 : 1;
  char buf[160];
  int n;
  if (color) {
    n = nl_snprintf(buf, sizeof(buf),
                    "\033[%d;%dH\033[48;2;18;18;18m\033[1;38;2;255;40;40m%s"
                    "\033[38;2;255;255;255m REC %s \033[0m",
                    panel_row, col, lit ? "●" : " ", clk);
  } else {
    n = nl_snprintf(buf, sizeof(buf), "\033[%d;%dH\033[7m %s REC %s \033[0m",
                    panel_row, col, lit ? "(o)" : "   ", clk);
  }

  if (n > 0 && n < (int)sizeof(buf)) {
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }
}

static void overlay_panel(int panel_row, const frame_rect_t *frame,
                          const mouse_drag_t *drag, double fps,
                          plugin_loader_t *plugins, const int *plugin_params,
                          int count, int selected, int color,
                          const ascii_opts_t *opts,
                          const charset_registry_t *charsets, int hw_exposure,
                          int hw_contrast, int hw_wb, int cap_w, int cap_h,
                          int ascii_size_manual) {
  char buf[1024];
  int n;
  int base_row = panel_row;
  int ascii_h = frame->h;
  int ascii_w_now = frame->w;

  // FPS + hint bar
  char fpsbuf[10];
  nl_fmt_fps(fpsbuf, sizeof(fpsbuf), fps);
  if (drag && drag->dragging) {
    // mouse drag in progress: preview of pending frame
    char pv[96];
    if (drag->dragging == GRAB_MOVE) {
      nl_snprintf(pv, sizeof(pv), "MOVING frame (%d,%d) -> (%d,%d)",
                  drag->base.x, drag->base.y, drag->pv.x, drag->pv.y);
    } else {
      nl_snprintf(pv, sizeof(pv), "RESIZING frame %dx%d -> %dx%d", drag->base.w,
                  drag->base.h, drag->pv.w, drag->pv.h);
    }

    if (color) {
      n = nl_snprintf(buf, sizeof(buf),
                      "\033[%d;1H\033[38;2;255;60;60m\033[48;2;18;18;18m"
                      " %s  │  release to apply\033[0m\033[K",
                      base_row, pv);
    } else {
      n = nl_snprintf(buf, sizeof(buf),
                      "\033[%d;1H %s  |  release to apply\033[K", base_row, pv);
    }
  } else if (color) {
    n = nl_snprintf(buf, sizeof(buf),
                    "\033[%d;1H\033[38;2;0;220;0m\033[48;2;18;18;18m"
                    " FPS: %s  │  / catalog  ↑↓ select  [ ] ±1  { } ±10  "
                    "r reset  s shot  t rec  b browse  q quit  │  frame: "
                    "%dx%d%s  cap: %dx%d  "
                    "◤◥◣◢ drag"
                    "\033[0m\033[K",
                    base_row, fpsbuf, ascii_w_now, ascii_h,
                    ascii_size_manual ? " [manual]" : "", cap_w, cap_h);
  } else {
    n = nl_snprintf(
        buf, sizeof(buf),
        "\033[%d;1H FPS: %s  |  / catalog  up/dn select  "
        "[ ] +-1  { } +-10  r reset  s shot  t rec  b browse  q quit  |  "
        "frame: %dx%d%s  cap: %dx%d  drag +"
        "\033[K",
        base_row, fpsbuf, ascii_w_now, ascii_h,
        ascii_size_manual ? " [manual]" : "", cap_w, cap_h);
  }

  if (n > 0 && n < (int)sizeof(buf)) {
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }

  // Mode/edge/charset/depth-pop status row
  const char *cset_name = (charsets && charsets->count > 0 &&
                           opts->render_mode == RENDER_ASCII_RAMP)
                              ? charsets->sets[charsets->active].name
                              : "-";
  n = nl_snprintf(buf, sizeof(buf), "\033[%d;1H\033[K", base_row + 1);
  if (n > 0) {
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }

  if (color) {
    n = nl_snprintf(
        buf, sizeof(buf),
        "\033[38;2;0;180;220m mode: %s (m/M)  edges: %s (x/X)  "
        "charset: %s (n/N)  depth-pop: %d%s (+/-, v)  gamma: %d (g/G)"
        "\033[0m\033[K",
        render_mode_name(opts->render_mode), edge_mode_name(opts->edges),
        cset_name, opts->depth_pop, opts->depth_invert ? " [inv]" : "",
        opts->gamma);
  } else {
    n = nl_snprintf(
        buf, sizeof(buf),
        " mode: %s  edges: %s  charset: %s  depth-pop: %d%s  gamma: %d\033[K",
        render_mode_name(opts->render_mode), edge_mode_name(opts->edges),
        cset_name, opts->depth_pop, opts->depth_invert ? " [inv]" : "",
        opts->gamma);
  }

  if (n > 0 && n < (int)sizeof(buf)) {
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }

  // Hardware (V4L2) camera control row - "n/a" fields when unsupported
  // (macOS/Windows, or a driver that doesn't expose that control)
  n = nl_snprintf(buf, sizeof(buf), "\033[%d;1H\033[K", base_row + 2);
  if (n > 0) {
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }

  char exp_buf[16];
  char con_buf[16];
  char wb_buf[16];
  if (hw_exposure >= 0) {
    nl_snprintf(exp_buf, sizeof(exp_buf), "%d", hw_exposure);
  } else {
    nl_snprintf(exp_buf, sizeof(exp_buf), "n/a");
  }
  if (hw_contrast >= 0) {
    nl_snprintf(con_buf, sizeof(con_buf), "%d", hw_contrast);
  } else {
    nl_snprintf(con_buf, sizeof(con_buf), "n/a");
  }
  if (hw_wb >= 0) {
    nl_snprintf(wb_buf, sizeof(wb_buf), "%dK", hw_wb);
  } else {
    nl_snprintf(wb_buf, sizeof(wb_buf), "n/a");
  }

  if (color) {
    n = nl_snprintf(buf, sizeof(buf),
                    "\033[38;2;220;160;0m hw exposure: %s (e/E)  hw contrast: "
                    "%s (c/C)  hw white-balance: %s (w/W)\033[0m\033[K",
                    exp_buf, con_buf, wb_buf);
  } else {
    n = nl_snprintf(buf, sizeof(buf),
                    " hw exposure: %s  hw contrast: %s  hw white-balance: "
                    "%s\033[K",
                    exp_buf, con_buf, wb_buf);
  }
  if (n > 0 && n < (int)sizeof(buf)) {
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }

  n = nl_snprintf(buf, sizeof(buf), "\033[%d;1H\033[K", base_row + 3);
  if (n > 0) {
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }

  // Plugin cells
  if (count == 0) {
    const char *msg = color ? "\033[38;2;120;120;120m no plugins loaded \033[0m"
                            : " no plugins loaded";

    (void)write(STDOUT_FILENO, msg, nl_strlen(msg));

    return;
  }

  for (int i = 0; i < count; i++) {
    const char *name = plugins[i].plugin ? plugins[i].plugin->name : "???";
    int param = plugin_params[i];
    int is_sel = (i == selected);

    if (color) {
      // Selected: bright yellow text on dark blue bg; others: dim
      if (is_sel) {
        n = nl_snprintf(buf, sizeof(buf),
                        "\033[38;2;255;220;0m\033[48;2;0;40;80m"
                        " ▶ %s [%3d] \033[0m ",
                        name, param);
      } else {
        n = nl_snprintf(buf, sizeof(buf),
                        "\033[38;2;140;140;140m\033[48;2;18;18;18m"
                        "   %s [%3d] \033[0m ",
                        name, param);
      }
    } else {
      n = nl_snprintf(buf, sizeof(buf), is_sel ? " *%s[%3d]  " : "  %s[%3d]  ",
                      name, param);
    }

    if (n > 0 && n < (int)sizeof(buf)) {
      (void)write(STDOUT_FILENO, buf, (size_t)n);
    }
  }
}

// Grow/shrink ASCII frame to (new_w, new_h). Clamps to MIN_ASCII_W/H,
// reallocates *out_buf, updates *out_size
//
// Returns >0 if size changed, 0 if unchanged or OOM (in which case old buffer
// is kept and *ascii_w / *ascii_h are left at their previous values so caller
// is not left with a size that has no buffer to match)
static int apply_ascii_size(int new_w, int new_h, int *ascii_w, int *ascii_h,
                            char **out_buf, size_t *out_size, int color) {
  if (new_h < MIN_ASCII_H) {
    new_h = MIN_ASCII_H;
  }
  if (new_w < MIN_ASCII_W) {
    new_w = MIN_ASCII_W;
  }
  if (new_w == *ascii_w && new_h == *ascii_h) {
    return 0;
  }

  // Recompute output buffer size for new dimensions (max over all modes)
  size_t need = 0;
  for (render_mode_t rm = 0; rm < RENDER_MODE_COUNT; rm++) {
    size_t s = ascii_out_size_for_mode(new_w * 2, new_h * 4, color, rm);
    if (s > need) {
      need = s;
    }
  }

  char *nb = nl_malloc(need);
  if (!nb) {
    return 0;
  }

  nl_free(*out_buf);
  *out_buf = nb;
  *out_size = need;
  *ascii_w = new_w;
  *ascii_h = new_h;

  return 1;
}

// Returns >0 if ASCII dimensions changed, 0 otherwise
// Reads terminal size and delegates to apply_ascii_size
int handle_term_resize(int *ascii_w, int *ascii_h, char **out_buf,
                       size_t *out_size, int color) {
  // Query terminal size via ioctl(TIOCGWINSZ)
  struct winsize ws;
  nl_memset(&ws, 0, sizeof(ws));
  if (nl_ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) {
    return 0;
  }

  if (ws.ws_col == 0 || ws.ws_row == 0) {
    return 0;
  }

  int new_h = ws.ws_row - PANEL_ROWS; // reserve rows for overlay panel

  char dbg[96];
  int dn = nl_snprintf(dbg, sizeof(dbg), "[resize] ws_row=%u ws_col=%u\n",
                       (unsigned)ws.ws_row, (unsigned)ws.ws_col);
  if (dn > 0) {
    write(2, dbg, (size_t)dn);
  }

  return apply_ascii_size(ws.ws_col, new_h, ascii_w, ascii_h, out_buf, out_size,
                          color);
}

// Terminal size in cells; left unchanged when it cannot be read
static void query_term_size(int *w, int *h) {
  struct winsize ws;
  nl_memset(&ws, 0, sizeof(ws));
  if (nl_ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
      ws.ws_row > 0) {
    *w = ws.ws_col;
    *h = ws.ws_row;
  }
}

// --debug-input log (-1 = off)
static int input_log_fd = -1;

static void input_log(const char *fmt, int a, int b, int c, int d) {
  if (input_log_fd < 0) {
    return;
  }

  char line[160];
  int n = nl_snprintf(line, sizeof(line), fmt, a, b, c, d);
  if (n > 0) {
    (void)write(input_log_fd, line, (size_t)n);
  }
}

// Next byte of an escape sequence
// A terminal writes a whole sequence at once but it may arrive in pieces; give
// rest up to ~25 ms to show up
// Returns 1 and stores byte, or 0 when nothing came (a lone ESC key)
static int read_escape_byte(char *c) {
  for (int i = 0; i < 25; i++) {
    if (read(STDIN_FILENO, c, 1) == 1) {
      input_log("  esc byte 0x%02x after %d ms\n", (unsigned char)*c, i, 0, 0);

      return 1;
    }

    nl_usleep(1000);
  }

  input_log("  esc: nothing more after 25 ms\n", 0, 0, 0, 0);

  return 0;
}

// Pixel size of one terminal cell; 8x16 when terminal does not say
static void query_cell_px(int *cw, int *ch) {
  struct winsize ws;
  nl_memset(&ws, 0, sizeof(ws));
  *cw = 8;
  *ch = 16;
  if (nl_ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
      ws.ws_row > 0 && ws.ws_xpixel >= ws.ws_col && ws.ws_ypixel >= ws.ws_row) {
    *cw = ws.ws_xpixel / ws.ws_col;
    *ch = ws.ws_ypixel / ws.ws_row;
  }
}

// Row status panel starts on: bottom of terminal, so it stays put wherever
// frame is
// Without a known size it goes under frame as before
static int panel_row_for(int term_h, int ascii_h) {
  if (term_h <= 0) {
    return ascii_h + 1;
  }

  return term_h - PANEL_ROWS + 1 < 1 ? 1 : term_h - PANEL_ROWS + 1;
}

// Active-plugin list. plugins[]/params[]/cat[] stay packed, so adding and
// removing at runtime is just append / shift-down
// cat[i] is catalog entry a was loaded from, or -1 for plugins given with -p
static int plugin_slot_add(plugin_loader_t *plugins, int *params, int *cat,
                           int *count, const char *path, int cat_idx, char *err,
                           size_t errsz) {
  if (*count >= MAX_PLUGINS) {
    nl_snprintf(err, errsz, "plugin list full (%d)", MAX_PLUGINS);

    return -1;
  }

  int i = *count;
  nl_memset(&plugins[i], 0, sizeof(plugin_loader_t));
  plugins[i].inotify_fd = -1;
  params[i] = 128; // default
  cat[i] = cat_idx;

  if (plugin_load(&plugins[i], path) != 0) {
    nl_snprintf(err, errsz, "%s", plugins[i].status_msg);

    plugin_cleanup(&plugins[i]);

    return -1;
  }

  plugin_watch_init(&plugins[i], path);
  (*count)++;

  return i;
}

static void plugin_slot_remove(plugin_loader_t *plugins, int *params, int *cat,
                               int *count, int *selected, int idx,
                               plugin_catalog_t *cc) {
  if (idx < 0 || idx >= *count) {
    return;
  }

  if (cat[idx] >= 0 && cat[idx] < cc->count) {
    cc->e[cat[idx]].state = PC_READY;
  }

  plugin_cleanup(&plugins[idx]);
  for (int i = idx; i + 1 < *count; i++) {
    nl_memcpy(&plugins[i], &plugins[i + 1], sizeof(plugin_loader_t));
    params[i] = params[i + 1];
    cat[i] = cat[i + 1];
  }

  (*count)--;

  if (*selected >= *count) {
    *selected = *count > 0 ? *count - 1 : 0;
  }
}

// Load a built catalog entry into filter chain. msg gets toast text
static int plugin_activate(plugin_catalog_t *cc, int ci,
                           plugin_loader_t *plugins, int *params, int *cat,
                           int *count, char *msg, size_t msgsz) {
  catalog_entry_t *e = &cc->e[ci];
  char err[CATALOG_ERR_LEN];

  if (plugin_slot_add(plugins, params, cat, count, e->so_path, ci, err,
                      sizeof(err)) < 0) {
    e->state = PC_FAILED;
    nl_snprintf(e->err, sizeof(e->err), "%s", err);
    nl_snprintf(msg, msgsz, "%s: %s", e->name, err);

    return -1;
  }

  e->state = PC_ACTIVE;
  nl_snprintf(msg, msgsz, "loaded %s", e->name);

  return 0;
}

fps_counter_t fps_calc = {0};

// "2", "0.5", "1.25" -> 2.0 ... ; -1.0 when not a positive number up to 16
static double parse_speed(const char *s) {
  if (!s || *s == '\0') {
    return -1.0;
  }

  double v = 0.0;
  double frac = 0.1;
  int dot = 0;
  int digits = 0;
  for (; *s; s++) {
    if (*s >= '0' && *s <= '9') {
      if (dot) {
        v += (*s - '0') * frac;
        frac /= 10.0;
      } else {
        v = v * 10.0 + (*s - '0');
      }

      digits++;
    } else if (*s == '.' && !dot) {
      dot = 1;
    } else {
      return -1.0;
    }
  }

  return digits > 0 && v >= 0.0625 && v <= 16.0 ? v : -1.0;
}

// Alternate screen on / off (cursor hidden while on)
static const char SCREEN_ENTER[] = "\033[?1049h\033[2J\033[H\033[?25l";
static const char SCREEN_LEAVE[] = "\033[0m\033[?25h\033[?1049l";

// Main
// A non-negative decimal number not larger than max, or -1
static long parse_count(const char *s, long max) {
  if (!s || *s == '\0') {
    return -1;
  }

  long v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') {
      return -1;
    }

    v = v * 10 + (*s - '0');
    if (v > max) {
      return -1;
    }
  }

  return v;
}

int main(int argc, char *argv[]) {
  char **envp_base = argv + argc + 1;
  for (int i = 1; i < argc; i++) {
    if (nl_strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);

      return 0;
    }
  }

  // --gfx <type>: taken out of argv here, resolved once environment is known
  // (auto looks at TERM)
  const char *gfx_name = NULL;
  for (int i = 1; i < argc; i++) {
    if (nl_strcmp(argv[i], "--gfx") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--gfx needs kitty, sixel, auto or off\n");

        return 2;
      }

      gfx_name = argv[i + 1];
      for (int j = i; j + 2 <= argc; j++) {
        argv[j] = argv[j + 2];
      }

      argc -= 2;
      i--;
    }
  }

  // getopt only knows short options: --demo is -d demo
  for (int i = 1; i < argc; i++) {
    if (nl_strcmp(argv[i], "--demo") == 0) {
      argv[i] = (char *)"-d" DEMO_DEVICE;
    }
  }

  // --image <file> is -d image:<file>
  static char image_arg[600];
  for (int i = 1; i < argc; i++) {
    if (nl_strcmp(argv[i], "--image") != 0) {
      continue;
    }

    if (i + 1 >= argc || nl_strlen(argv[i + 1]) > 500) {
      fprintf(stderr, "--image needs a picture file or a folder of them\n");

      return 2;
    }

    nl_snprintf(image_arg, sizeof(image_arg), "-d" IMAGE_DEVICE_PREFIX "%s",
                argv[i + 1]);
    argv[i] = image_arg;
    for (int k = i + 1; k + 1 < argc; k++) {
      argv[k] = argv[k + 1];
    }

    argc--;
    argv[argc] = NULL;
  }

  // --slide <seconds>: time per picture of a slideshow
  for (int i = 1; i < argc; i++) {
    if (nl_strcmp(argv[i], "--slide") != 0) {
      continue;
    }

    long v = i + 1 < argc ? parse_count(argv[i + 1], 86400) : -1;
    if (v < 0) {
      fprintf(stderr, "--slide needs a number of seconds (0 = keys only)\n");

      return 2;
    }

    slides_set_secs((int)v);
    for (int k = i; k + 2 < argc; k++) {
      argv[k] = argv[k + 2];
    }

    argc -= 2;
    argv[argc] = NULL;
    i--;
  }

  // --net <kbps>: slow-link mode (getopt only knows short options, so it is
  // taken out of argv here)
  // -1 = off
  int net_kbps = -1;
  for (int i = 1; i < argc; i++) {
    if (nl_strcmp(argv[i], "--net") != 0) {
      continue;
    }

    long v = i + 1 < argc ? parse_count(argv[i + 1], 1000000) : -1;
    if (v < 0) {
      fprintf(stderr, "--net needs a number of kbit/s (0 = lossless)\n");

      return 2;
    }

    net_kbps = (int)v;
    for (int k = i; k + 2 < argc; k++) {
      argv[k] = argv[k + 2];
    }

    argc -= 2;
    argv[argc] = NULL;
    i--;
  }

  // --curve <points | preset | @file>: tone curve
  char cli_curve[256];
  cli_curve[0] = '\0';
  for (int i = 1; i < argc; i++) {
    if (nl_strcmp(argv[i], "--curve") != 0) {
      continue;
    }

    if (i + 1 >= argc) {
      fprintf(stderr, "--curve needs points like \"0,0 64,40 255,255\", a "
                      "preset name or @file\n");

      return 2;
    }

    nl_strncpy_safe(cli_curve, argv[i + 1], sizeof(cli_curve));
    for (int k = i; k + 2 < argc; k++) {
      argv[k] = argv[k + 2];
    }

    argc -= 2;
    argv[argc] = NULL;
    i--;
  }

  // --debug-input <file>: write every input byte and every decoded mouse event
  for (int i = 1; i < argc; i++) {
    if (nl_strcmp(argv[i], "--debug-input") != 0) {
      continue;
    }

    if (i + 1 >= argc) {
      fprintf(stderr, "--debug-input needs a file name\n");

      return 2;
    }

    input_log_fd =
        open(argv[i + 1],
             (int)((unsigned)O_WRONLY | (unsigned)O_CREAT | (unsigned)O_TRUNC),
             0600);
    if (input_log_fd < 0) {
      fprintf(stderr, "--debug-input: cannot write %s\n", argv[i + 1]);

      return 2;
    }

    for (int k = i; k + 2 < argc; k++) {
      argv[k] = argv[k + 2];
    }

    argc -= 2;
    argv[argc] = NULL;
    i--;
  }

  // --vcam <device> [--vcam-size WxH]: picture as a virtual webcam
  vcam_t vcam;
  int vcam_on = 0;
  const char *vcam_path = NULL;
  int vcam_w = VCAM_DEFAULT_W, vcam_h = VCAM_DEFAULT_H;
  for (int i = 1; i < argc; i++) {
    int is_dev = nl_strcmp(argv[i], "--vcam") == 0;
    if (!is_dev && nl_strcmp(argv[i], "--vcam-size") != 0) {
      continue;
    }

    if (i + 1 >= argc) {
      fprintf(stderr, "%s needs a value\n", argv[i]);

      return 2;
    }

    if (is_dev) {
      vcam_path = argv[i + 1];
    } else {
      const char *z = argv[i + 1];
      long a = 0, b = 0;
      while (*z >= '0' && *z <= '9' && a < 100000) {
        a = a * 10 + (*z++ - '0');
      }

      if (*z != 'x' || z[1] < '0' || z[1] > '9') {
        a = 0;
      } else {
        z++;
        while (*z >= '0' && *z <= '9' && b < 100000) {
          b = b * 10 + (*z++ - '0');
        }
      }

      if (a < VCAM_CELL_W || b < VCAM_CELL_H || a > 4096 || b > 4096 ||
          a % 2 != 0 || *z) {
        fprintf(stderr, "--vcam-size needs WxH, W even, e.g. 1280x720\n");

        return 2;
      }

      vcam_w = (int)a;
      vcam_h = (int)b;
    }

    for (int k = i; k + 2 < argc; k++) {
      argv[k] = argv[k + 2];
    }

    argc -= 2;
    argv[argc] = NULL;
    i--;
  }

  if (vcam_path) {
    if (vcam_init(&vcam, vcam_w, vcam_h) < 0) {
      fprintf(stderr, "--vcam: cannot set up a %dx%d canvas\n", vcam_w, vcam_h);

      return 2;
    }

    if (vcam_open(&vcam, vcam_path) < 0) {
      fprintf(stderr, "--vcam: cannot open %s\n", vcam_path);
      vcam_close(&vcam);

      return 2;
    }

    vcam_on = 1;
#if defined(__LINUX_NOLIBC__)
    nl_signal(13,
              (void (*)(int))1); // SIGPIPE: a closed reader is a write error
#endif
  }

  // --play <file> [--speed <x>] [--loop]: replay a recording
  const char *play_path = NULL;
  double play_speed = 1.0;
  int play_loop = 0;
  for (int i = 1; i < argc; i++) {
    int takes_value =
        nl_strcmp(argv[i], "--play") == 0 || nl_strcmp(argv[i], "--speed") == 0;
    int is_loop = nl_strcmp(argv[i], "--loop") == 0;
    if (!takes_value && !is_loop) {
      continue;
    }

    int width = takes_value ? 2 : 1;
    if (takes_value && i + 1 >= argc) {
      fprintf(stderr, "%s needs a value\n", argv[i]);

      return 2;
    }

    if (is_loop) {
      play_loop = 1;
    } else if (nl_strcmp(argv[i], "--play") == 0) {
      play_path = argv[i + 1];
    } else {
      play_speed = parse_speed(argv[i + 1]);
      if (play_speed < 0.0) {
        fprintf(stderr, "--speed needs a number from 0.0625 to 16\n");

        return 2;
      }
    }

    for (int k = i; k + width < argc; k++) {
      argv[k] = argv[k + width];
    }

    argc -= width;
    argv[argc] = NULL;
    i--;
  }

  nl_signal(SIGINT, handle_signal);
  nl_signal(SIGTERM, handle_signal);
  nl_signal(SIGHUP, handle_signal);
  nl_signal(SIGQUIT, handle_signal);
  nl_signal(SIGABRT, handle_signal);
  nl_signal(SIGSEGV, handle_signal);
  nl_signal(SIGWINCH, handle_winch);

  if (play_path) {
    int probe = open(play_path, O_RDONLY, 0);
    if (probe < 0) {
      fprintf(stderr, "cannot read %s\n", play_path);

      return 1;
    }

    close(probe);

    int rw = 0;
    int rh = 0;
    browser_cast_size(play_path, &rw, &rh);

    int tw = 0;
    int th = 0;
    query_term_size(&tw, &th);

    if (rw > 0 && ((tw > 0 && tw < rw) || (th > 0 && th < rh))) {
      fprintf(stderr,
              "note: recorded at %dx%d, this terminal is %dx%d: the picture "
              "will wrap; enlarge the window first\n",
              rw, rh, tw, th);
      nl_usleep(1500000);
    }

    (void)write(STDOUT_FILENO, SCREEN_ENTER, sizeof(SCREEN_ENTER) - 1);
    term_raw_mode();

    int played = 0;
    int rc = browser_play_cast(play_path, play_speed, play_loop, &played);
    term_restore();

    (void)write(STDOUT_FILENO, SCREEN_LEAVE, sizeof(SCREEN_LEAVE) - 1);
    if (rc < 0) {
      fprintf(stderr, "cannot read %s\n", play_path);

      return 1;
    }

    return 0;
  }

  // Config
  char *device = "/dev/video0";
  int ascii_w = DEFAULT_ASCII_WIDTH;
  int ascii_h = DEFAULT_ASCII_HEIGHT;
  int cap_w = DEFAULT_CAPTURE_WIDTH;
  int cap_h = DEFAULT_CAPTURE_HEIGHT;
  int fps = DEFAULT_FPS;
  const char *charset_dir = NULL;

  ascii_opts_t opts = {
      .brightness = 0,
      .contrast = 100,
      .invert = 0,
      .color = 0,
      .gamma = 100,
      .edges = EDGE_OFF,
      .dither = 0,
      .threshold_val = 35,
      .charset = NULL,
      .render_mode = RENDER_DOTS, // RENDER_BRAILLE,
      .depth_pop = 0,
      .depth_invert = 0,
  };

  // Linux captures on its own thread unless -T; other platforms capture inline
  int capture_threaded = 1;

  // Plugins
  const char *plugin_paths[MAX_PLUGINS];
  int plugin_path_count = 0;

  // Seed defaults from .asciicamrc (cwd) if present
  load_config_file(DEFAULT_CONFIG_PATH, &device, &ascii_w, &ascii_h, &cap_w,
                   &cap_h, &fps, &opts, &charset_dir, plugin_paths,
                   &plugin_path_count);

  // CLI parsing
  int opt;
  while ((opt = nl_getopt(argc, argv,
                          "d:W:H:w:h:r:b:c:g:iCD2s:p:L:m:E:k:P:Tf")) != -1)
    switch (opt) {
    case 'd': {
      device = optarg;

      break;
    }
    case 'W': {
      ascii_w = my_atoi(optarg);
      if (ascii_w <= 0) {
        ascii_w = DEFAULT_ASCII_WIDTH;
      }

      break;
    }
    case 'H': {
      ascii_h = my_atoi(optarg);
      if (ascii_h <= 0) {
        ascii_h = DEFAULT_ASCII_HEIGHT;
      }

      break;
    }
    case 'w': {
      cap_w = my_atoi(optarg);
      if (cap_w <= 0) {
        cap_w = DEFAULT_CAPTURE_WIDTH;
      }

      break;
    }
    case 'h': {
      cap_h = my_atoi(optarg);
      if (cap_h <= 0) {
        cap_h = DEFAULT_CAPTURE_HEIGHT;
      }

      break;
    }
    case 'r': {
      fps = my_atoi(optarg);
      if (fps <= 0) {
        fps = DEFAULT_FPS;
      }

      break;
    }
    case 'b': {
      opts.brightness = my_atoi(optarg);

      break;
    }
    case 'c': {
      opts.contrast = my_atoi(optarg);
      if (opts.contrast <= 0) {
        opts.contrast = 100;
      }

      break;
    }
    case 'i': {
      opts.invert = 1;

      break;
    }
    case 'C': {
      opts.color = 1;
      opts.color_mode = COLOR_TRUECOLOR;

      break;
    }
    case '2': {
      opts.color = 1;
      opts.color_mode = COLOR_256;

      break;
    }
    case 'g': {
      opts.gamma = my_atoi(optarg);
      if (opts.gamma < 10) {
        opts.gamma = 10;
      }
      if (opts.gamma > 400) {
        opts.gamma = 400;
      }

      break;
    }
    case 'E': {
      opts.edges = parse_edge_mode(optarg);

      break;
    }
    case 'm': {
      opts.render_mode = parse_render_mode(optarg);

      break;
    }
    case 'k': {
      charset_dir = optarg;

      break;
    }
    case 'P': {
      opts.depth_pop = my_atoi(optarg);
      if (opts.depth_pop < 0) {
        opts.depth_pop = 0;
      }
      if (opts.depth_pop > 100) {
        opts.depth_pop = 100;
      }

      break;
    }
    case 'D': {
      opts.dither = 1;

      break;
    }
    case 'T': {
      capture_threaded = 0;

      break;
    }
    case 's': {
      opts.charset = optarg;

      break;
    }
    case 'L': {
      g_plugin_dir = optarg;

      break;
    }
    case 'p': {
      if (plugin_path_count < MAX_PLUGINS) {
        plugin_paths[plugin_path_count++] = optarg;
      } else {
        fprintf(stderr, "Warning: max %d plugins, ignoring %s\n", MAX_PLUGINS,
                optarg);
      }

      break;
    }
    case 'f':
    case 'F': {
      opts.mirror = !opts.mirror;
      break;
    }

    default: {
      print_usage(argv[0]);

      return 1;
    }
    }

  timing_init(fps);

  // Initialize plugins
  plugin_loader_t plugins[MAX_PLUGINS];
  int plugin_params[MAX_PLUGINS];
  int plugin_cat[MAX_PLUGINS];
  int plugin_count = 0;

  for (int i = 0; i < plugin_path_count; i++) {
    // nl_memset(&plugins[i], 0, sizeof(plugin_loader_t));
    // plugins[i].inotify_fd = -1;
    // plugin_params[i] = 128; // default
    //
    // if (plugin_load(&plugins[i], plugin_paths[i]) == 0) {
    //   plugin_params[i] = plugin_default_param(plugins[i].plugin);
    //   plugin_watch_init(&plugins[i], plugin_paths[i]);
    //   plugin_count++;
    // } else {
    //   fprintf(stderr, "Failed to load plugin: %s\n", plugin_paths[i]);
    // }
    char perr[CATALOG_ERR_LEN];
    if (plugin_slot_add(plugins, plugin_params, plugin_cat, &plugin_count,
                        plugin_paths[i], -1, perr, sizeof(perr)) < 0) {
      fprintf(stderr, "Failed to load plugin: %s (%s)\n", plugin_paths[i],
              perr);
    }
  }

  // Tone curve: --curve beats curve= in .asciicamrc; "@file" reads spec
  curve_t curve;
  curve_reset(&curve);
  curve_ui_t curve_ui;
  curve_ui_init(&curve_ui);
  {
    const char *spec = cli_curve[0] ? cli_curve : g_curve_cfg;
    char from_file[256];
    if (spec[0] == '@') {
      if (read_small_file(spec + 1, from_file, sizeof(from_file)) < 0) {
        fprintf(stderr, "--curve: cannot read %s\n", spec + 1);

        return 2;
      }

      spec = from_file;
    }

    char cerr[96];
    if (spec[0] && curve_parse(&curve, spec, cerr, sizeof(cerr)) < 0) {
      fprintf(stderr, "%s\n", cerr);

      return 2;
    }
  }

  // From here on TUI owns terminal: load problems are reported in UI
  // (status_msg)
  plugin_log_stderr = 0;

  // Catalog of plugin sources that can be built and loaded while running
  char **envp =
#ifdef __LINUX_NOLIBC__
      envp_base; // envp follows original argv NULL terminator
#else
      (char **)0;
#endif

  gfx_mode_t gfx_mode = GFX_OFF;
  if (gfx_name) {
    int gm = gfx_parse(gfx_name, app_getenv(envp, "TERM"),
                       app_getenv(envp, "TERM_PROGRAM"),
                       app_getenv(envp, "KITTY_WINDOW_ID") != NULL);
    if (gm < 0) {
      fprintf(stderr, "--gfx require kitty, sixel, auto or off (got \"%s\")\n",
              gfx_name);

      return 2;
    }

    gfx_mode = (gfx_mode_t)gm;
    if (gfx_mode == GFX_OFF && nl_strcmp(gfx_name, "auto") == 0) {
      fprintf(stderr, "--gfx auto: this terminal does not support pictures, "
                      "using characters\n");
    }
  }

  // Everything lives: prebuilt plugins beside binary or under install prefix,
  // sources + build cache only if a plugin SDK is around
  char exe_dir[AP_LEN];
  app_env_t app_env = {
      .exe_dir = app_exe_dir(exe_dir, sizeof(exe_dir)) == 0 ? exe_dir : NULL,
      .home = app_getenv(envp, "HOME"),
      .xdg_cache = app_getenv(envp, "XDG_CACHE_HOME"),
      .exists = NULL,
  };
  static app_paths_t paths;
  app_paths_resolve(&paths, &app_env, g_plugin_dir, g_plugin_cache,
                    charset_dir);
  charset_dir = paths.charset_dir;

  plugin_catalog_t catalog;
  catalog_init(&catalog, paths.src_dir, paths.cache_dir, envp);
  // "." is directory make already runs in; -C is only for a remote SDK
  if (paths.sdk_dir[0] && nl_strcmp(paths.sdk_dir, ".") != 0) {
    catalog_set_sdk(&catalog, paths.sdk_dir);
  }

  catalog_scan(&catalog);

  for (int i = 0; i < paths.prebuilt_count; i++) {
    catalog_add_prebuilt(&catalog, paths.prebuilt_dir[i]);
  }

  picker_t picker;
  nl_memset(&picker, 0, sizeof(picker));
  char toast_msg[160];
  toast_msg[0] = '\0';
  int toast_frames = 0;
  int browser_was_open = 0;

#define TOAST(...)                                                             \
  do {                                                                         \
    nl_snprintf(toast_msg, sizeof(toast_msg), __VA_ARGS__);                    \
                                                                               \
    toast_frames = fps * TOAST_SECONDS;                                        \
  } while (0)

// Curve editor asked for
#define CURVE_ACT(ca)                                                          \
  do {                                                                         \
    if ((ca) == CUI_CLOSE) {                                                   \
      browser.active = 0;                                                      \
      curve_ui.grab = 0;                                                       \
      browser_clear_area(term_h, term_w);                                      \
    } else if ((ca) == CUI_TAB_FILES) {                                        \
      browser.tab = BROWS_TAB_FILES;                                           \
      curve_ui.grab = 0;                                                       \
    } else if ((ca) == CUI_SAVE) {                                             \
      char tb_[64];                                                            \
      if (save_curve(&curve, cap_dir) == 0) {                                  \
        TOAST("curve saved: %s/curve.txt (load with --curve @file)",           \
              path_tail(cap_dir, tb_, sizeof(tb_)));                           \
      } else {                                                                 \
        TOAST("cannot save the curve into %s",                                 \
              path_tail(cap_dir, tb_, sizeof(tb_)));                           \
      }                                                                        \
    }                                                                          \
  } while (0)

  int selected = 0;

  // NOTE: built-in picture is bright everywhere: default dots mode would
  // fill screen, unless a mode was chosen, show it as an ASCII ramp
  if (demo_is_device(device) && opts.render_mode == RENDER_DOTS) {
    opts.render_mode = RENDER_ASCII_RAMP;
  }

  // Open webcam
  webcam_t cam = {.fd = -1, .buffer = MAP_FAILED};
  if (webcam_init(&cam, device, cap_w, cap_h) < 0) {
    nl_perror("webcam_init");
    if (!demo_is_device(device)) {
      fprintf(stderr,
              "no usable camera at %s. To try AsciiCam without one: "
              "%s --demo\n",
              device, argv[0]);
    }

    return 1;
  }

  fprintf(stderr,
          "Device: %s | capture %dx%d | ASCII %dx%d | %d fps | %d "
          "plugin(s) | mode: %s%s%s%s | gamma: %d\n",
          device, cam.width, cam.height, ascii_w, ascii_h, fps, plugin_count,
          render_mode_name(opts.render_mode), opts.color ? " | color" : "",
          opts.edges != EDGE_OFF ? " | edges" : "",
          opts.dither ? " | dither" : "", opts.gamma);

  int hw_exposure = -1;
  int hw_contrast = -1;
  int hw_wb = -1;
  webcam_get_exposure(&cam, &hw_exposure);
  webcam_get_contrast(&cam, &hw_contrast);
  webcam_get_white_balance(&cam, &hw_wb);

  // Pixel buffers allocation
  int cam_pixels = cam.width * cam.height;
  uint8_t *gray = nl_malloc(cam_pixels + 16); // + canary
  uint8_t *rgb = opts.color ? nl_malloc(cam_pixels * 3) : NULL;

  if (!gray || (opts.color && !rgb)) {
    nl_perror("malloc pixel buffers");
    nl_free(gray);
    webcam_cleanup(&cam);

    return 1;
  }

  // Allocate output string buffer
  size_t out_size = 0;
  for (render_mode_t rm = 0; rm < RENDER_MODE_COUNT; rm++) {
    size_t s =
        ascii_out_size_for_mode(ascii_w * 2, ascii_h * 4, opts.color, rm);
    if (s > out_size) {
      out_size = s;
    }
  }

  // Slow-link mode (--net): delta encoder state
  // net_dirty = terminal no longer shows what encoder thinks (screen cleared,
  // overlay removed)
  netenc_t net;
  char *net_buf = NULL;
  size_t net_buf_cap = 0;
  int net_dirty = 0;
  int net_age = 0;
  int net_prev_overlay = 0;
  int net_warned = 0;
  if (net_kbps >= 0) {
    // below ~24 levels of colour difference a change is noise, not picture
    netenc_init(&net, net_kbps > 0 ? 24 : 0, 0);
  }

  char *out_buf = malloc(out_size);
  if (!out_buf) {
    nl_perror("malloc out_buf");
    nl_free(gray);
    nl_free(rgb);
    webcam_cleanup(&cam);

    return 1;
  }

  // Charset registry, hot-reloadable ramps from charset_dir
  charset_registry_t charsets;
  charset_registry_init(&charsets, charset_dir);
  opts.charset = charset_registry_active_ramp(&charsets);

  // Capture thread (Linux). If memory or thread cannot be had, fall back to
  // capturing inline, exactly as with -T
  capture_pipe_t cap_pipe = {0};
  int pipe_on = 0;
#ifdef PLATFORM_LINUX
  if (capture_threaded) {
    if (capture_pipe_start(&cap_pipe, &cam, rgb != NULL) == 0) {
      pipe_on = 1;

      // frames now arrive in pipe's own slots
      nl_free(gray);
      gray = NULL;
    } else {
      fprintf(stderr, "capture thread unavailable, capturing inline\n");
    }
  }
#else
  (void)capture_threaded;
#endif

  // Initial screen setup
  (void)write(STDOUT_FILENO, SCREEN_ENTER, sizeof(SCREEN_ENTER) - 1);

  term_raw_mode();
  mouse_enable();

  mouse_parser_t mouse_parser = {0, {0, 0, 0}, 0};
  mouse_drag_t mouse_drag = {0};
  // Frame placement: top-left cell, and terminal it has to stay inside
  int frame_x = 1;
  int frame_y = 1;
  int term_w = 0;
  int term_h = 0;
  query_term_size(&term_w, &term_h);
  // Outline of pending rectangle while a drag is in progress
  frame_rect_t outline = {0, 0, 0, 0};
  int outline_on = 0;

  // 0 = frame follows terminal width (SIGWINCH); 1 = user dragged it
  int ascii_size_manual = 0;

  // Recorder / screenshots: output files paths, saved in this session
  // Declared at function scope because they are used by keyhandler ('s'
  // screenshot, 't' toggle-rec hotkeys), per-frame shot/rec write
  recorder_t rec;
  rec_init(&rec);

  // Save screenshots and recordings under user's Pictures folder so they
  // are visible alongside other image files, instead of buried in XDG_DATA
  // WARN: Probe existing casing first so it does not create a duplicate
  // ~/Pictures next to lowercase ~/pictures some systems/distros use;
  // default to 'Pictures' (XDG user-dirs standard) when neither exists
  char cap_dir[REC_PATH_LEN];
  int got_dir = 0;
  if (app_env.home && app_env.home[0]) {
    const char *pics = "Pictures"; // XDG user-dirs default capitalisation
    char probe[REC_PATH_LEN];
    if (nl_snprintf(probe, sizeof(probe), "%s/Pictures", app_env.home) > 0) {
      int pfd = open(probe, O_RDONLY, 0);
      if (pfd < 0) {
        // ~/Pictures doesn't exist: try ~/pictures before creating one
        if (nl_snprintf(probe, sizeof(probe), "%s/pictures", app_env.home) >
            0) {
          int pfd2 = open(probe, O_RDONLY, 0);
          if (pfd2 >= 0) {
            close(pfd2);
            pics = "pictures";
          }
        }
      } else {
        close(pfd);
      }
    }

    if (nl_snprintf(cap_dir, sizeof(cap_dir), "%s/%s/asciicam", app_env.home,
                    pics) > 0 &&
        rec_make_dirs(cap_dir) == 0) {
      got_dir = 1;
    }
  }
  if (!got_dir) {
    // Last-resort fallback: captures dir in current working directory
    nl_snprintf(cap_dir, sizeof(cap_dir), "captures");
    rec_make_dirs(cap_dir);
  }

  int want_shot = 0;

  // Picture output (--gfx): scaled copy of frame and its encoded form
  uint8_t *gfx_img = NULL;
  char *gfx_out = NULL;
  size_t gfx_img_cap = 0;
  size_t gfx_out_cap = 0;
  if (gfx_mode == GFX_SIXEL) {
    (void)write(STDOUT_FILENO, "\033[?80h", 6); // do not scroll at bottom
  }

  char saved_cast[REC_PATH_LEN] = "";
  char saved_shot[REC_PATH_LEN] = "";

  // Captures browser: retractable side panel that lists screenshots and
  // recordings saved to cap_dir, with in-terminal preview (.ansi) and
  // replay (.cast)
  // Toggled with 'b' while main loop is running
  browser_t browser;
  browser_init(&browser);
  browser_scan(&browser, cap_dir);

  struct timespec frame_start = {0};
  struct timespec last_frame_time = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &frame_start) != 0) {
    nl_perror("clock_gettime");

    return 1;
  }

  last_frame_time = frame_start;

  // Main loop
  while (keep_running) {
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      // Skip this frame; timing would be garbage
      continue;
    }

    frame_start = now;

    long frame_diff_ns =
        (frame_start.tv_sec - last_frame_time.tv_sec) * 1000000000L + // Seconds
        (frame_start.tv_nsec - last_frame_time.tv_nsec); // Nano seconds

    if (frame_diff_ns > 0) {
      fps_push(&fps_calc, frame_diff_ns);
    }

    last_frame_time = frame_start;
    double current_fps = fps_get(&fps_calc);

    // Apply any pending terminal resize (SIGWINCH) before drawing this
    // frame. If user has manually sized frame with mouse, terminal auto-fit
    // is suppressed until press 'a'
    if (term_resized) {
      term_resized = 0;

      query_term_size(&term_w, &term_h);

      // a mid-drag SIGWINCH would corrupt drag's frame-base snapshot
      if (mouse_drag.dragging) {
        mouse_drag.dragging = 0;
      }

      outline_on = 0;
      (void)write(STDOUT_FILENO, "\033[2J", 4);
      net_dirty = 1;

      if (ascii_size_manual) {
        // keep chosen frame, but not outside (smaller) terminal
        int avail_h = term_h - PANEL_ROWS;
        int fw = (term_w > 0 && ascii_w > term_w) ? term_w : ascii_w;
        int fh = (term_h > 0 && ascii_h > avail_h) ? avail_h : ascii_h;
        apply_ascii_size(fw, fh, &ascii_w, &ascii_h, &out_buf, &out_size,
                         opts.color);
        if (term_w > 0 && frame_x + ascii_w - 1 > term_w) {
          frame_x = term_w - ascii_w + 1;
        }

        if (term_h > 0 && frame_y + ascii_h - 1 > avail_h) {
          frame_y = avail_h - ascii_h + 1;
        }

        frame_x = frame_x < 1 ? 1 : frame_x;
        frame_y = frame_y < 1 ? 1 : frame_y;
      } else {
        frame_x = frame_y = 1;
        int rc = handle_term_resize(&ascii_w, &ascii_h, &out_buf, &out_size,
                                    opts.color);
        if (rc > 0) {
          char _b[96];
          int _n = nl_snprintf(_b, sizeof(_b),
                               "Resized: ASCII %dx%d (terminal changed)\n",
                               ascii_w, ascii_h);
          if (_n > 0) {
            write(2, _b, (size_t)_n);
          }
        }
      }
    }

    // DEBUG:
    // {
    //   static sig_atomic_t last = 0;
    //   if (winch_count != last) {
    //     last = winch_count;
    //     char b[64];
    //     int n = nl_snprintf(b, sizeof(b), "[winch #%d]\n",
    //     (int)winch_count); if (n > 0) {
    //       write(2, b, (size_t)n);
    //     }
    //   }
    // }

    // Keypress handling
    char ch;
    while (read(STDIN_FILENO, &ch, 1) == 1) {
      input_log("byte 0x%02x\n", (unsigned char)ch, 0, 0, 0);

      // Inside an SGR mouse report (ESC [ < b ; x ; y M|m): bytes must not
      // reach key handler, 'M' would cycle render mode
      if (mouse_parser_active(&mouse_parser)) {
        mouse_event_t mev;
        int prc = mouse_parser_feed(&mouse_parser, ch, &mev);
        if (prc == MOUSE_PARSE_MORE) {
          continue;
        }

        if (prc == MOUSE_PARSE_DONE) {
          // Corners resize ASCII frame 1:1 in cells, dragging body moves it
          // Capture resolution is controlled only by -w/-h at startup and
          // is not touched here, so frame's aspect and camera's aspect stay
          // independent. Nothing changes until release: meanwhile an
          // outline shows where frame will go
          // Side panel: its tab strip (always on screen) opens, switches and
          // retracts it. Inside panel mouse belongs to panel (and to a curve
          // point being dragged wherever pointer goes), except while frame is
          // being dragged: that drag must see its release
          if (!mouse_drag.dragging) {
            int grabbing = browser.active && browser.tab == BROWS_TAB_CURVE &&
                           curve_ui_grabbing(&curve_ui);
            if (!grabbing && mev.pressed && !mev.motion && !mev.wheel &&
                mev.button == 0) {
              int tab = browser_strip_tab_at(term_h, term_w, browser.active,
                                             mev.x, mev.y);
              if (tab >= 0) {
                if (browser.active && browser.tab == tab) {
                  browser.active = 0; // open tab was clicked: retract
                  browser_clear_area(term_h, term_w);
                } else {
                  if (!browser.active) {
                    browser_toggle(&browser);
                  }

                  browser.tab = tab;
                  if (tab == BROWS_TAB_FILES) {
                    browser_scan(&browser, cap_dir);
                  }
                }

                curve_ui.grab = 0;

                continue;
              }
            }

            if (browser.active &&
                (grabbing || browser_panel_hit(term_h, term_w, mev.x, mev.y))) {
              if (browser.tab == BROWS_TAB_CURVE) {
                curve_act_t ca =
                    curve_ui_mouse(&curve_ui, &curve, &mev, term_h, term_w);
                CURVE_ACT(ca);
              }

              continue;
            }
          }

          frame_rect_t cur = {frame_x, frame_y, ascii_w, ascii_h};
          mouse_limits_t lim = {term_w,          term_h,      PANEL_ROWS,
                                MIN_ASCII_W,     MIN_ASCII_H, DRAG_MAX_ASCII_W,
                                DRAG_MAX_ASCII_H};
          frame_rect_t nr = cur;
          int act = mouse_drag_event(&mouse_drag, &mev, &cur, &lim, &nr);
          input_log("mouse pressed=%d btn=%d x=%d y=%d", mev.pressed,
                    mev.button, mev.x, mev.y);
          input_log(" motion=%d wheel=%d -> action=%d dragging=%d\n",
                    mev.motion, mev.wheel, act, mouse_drag.dragging);
          input_log("  frame %d,%d %dx%d", cur.x, cur.y, cur.w, cur.h);
          input_log(" terminal %dx%d panel rows %d\n", term_w, term_h,
                    PANEL_ROWS, 0);

          if (act == MOUSE_RESIZE) {
            if (outline_on) {
              draw_drag_outline(&outline, opts.color, 1);
            }

            outline = nr;
            outline_on = 1;
            draw_drag_outline(&outline, opts.color, 0);
          } else if (act == MOUSE_RELEASE || act == MOUSE_APPLY) {
            if (outline_on) {
              draw_drag_outline(&outline, opts.color, 1);
              outline_on = 0;
            }

            if (act == MOUSE_APPLY) {
              ascii_size_manual = 1;
              apply_ascii_size(nr.w, nr.h, &ascii_w, &ascii_h, &out_buf,
                               &out_size, opts.color);
              if (ascii_w == nr.w && ascii_h == nr.h) { // else: out of memory
                frame_x = nr.x;
                frame_y = nr.y;
              }

              (void)write(STDOUT_FILENO, "\033[2J", 4);
              net_dirty = 1;
            }
          }

          if (!keep_running) {
            break;
          }

          continue;
        }
        // MOUSE_PARSE_FAIL: not a mouse report after all, handle byte below
        // like any other key
      }

      if (ch == '\033') {
        // Bytes of one escape sequence may reach us in separate reads (slow
        // terminal, ssh, tmux): wait briefly for each next byte before deciding
        // that this was a lone ESC
        // Otherwise rest of a mouse report lands in hotkey switch ('[', 'M',
        // digits ...)
        char seq[2] = {0, 0};
        if (read_escape_byte(&seq[0]) && seq[0] == 'O') {
          // SS3 (application cursor keys): ESC O A..D; not bound, but its
          // last byte must not be taken for a hotkey
          char ss3;
          (void)read_escape_byte(&ss3);

          continue;
        }

        if (seq[0] == '[') {
          if (read_escape_byte(&seq[1])) {
            if (seq[1] == '<') {
              mouse_parser_begin(&mouse_parser);

              continue;
            }

            if (seq[1] == 'M') {
              // Legacy X10 report (ESC [ M b x y) from a terminal that has
              // no SGR mode: swallow 3 payload bytes, they are not keys (a
              // coordinate byte can equal 'q')
              char junk;
              for (int k = 0; k < 3; k++) {
                if (!read_escape_byte(&junk)) {
                  break;
                }
              }

              continue;
            }

            switch (seq[1]) {
            case 'A': { // up arrow key
              if (browser.active && browser.tab == BROWS_TAB_CURVE) {
                curve_act_t ca = curve_ui_key(&curve_ui, &curve, BR_KEY_UP, 0);
                CURVE_ACT(ca);
              } else if (browser.active) {
                browser_key(&browser, BR_KEY_UP, 0, NULL, 0);
              } else if (picker.active) {
                picker_key(&picker, &catalog, PK_KEY_UP, NULL);
              } else if (plugin_count > 0) {
                selected = (selected - 1 + plugin_count) % plugin_count;
              }

              break;
            }
            case 'B': { // down arrow key
              if (browser.active && browser.tab == BROWS_TAB_CURVE) {
                curve_act_t ca =
                    curve_ui_key(&curve_ui, &curve, BR_KEY_DOWN, 0);
                CURVE_ACT(ca);
              } else if (browser.active) {
                browser_key(&browser, BR_KEY_DOWN, 0, NULL, 0);
              } else if (picker.active) {
                picker_key(&picker, &catalog, PK_KEY_DOWN, NULL);
              } else if (plugin_count > 0) {
                selected = (selected + 1) % plugin_count;
              }

              break;
            }
            case 'C':   // right arrow key
            case 'D': { // left arrow key
              if (browser.active && browser.tab == BROWS_TAB_CURVE) {
                curve_act_t ca =
                    curve_ui_key(&curve_ui, &curve,
                                 seq[1] == 'C' ? BR_KEY_RIGHT : BR_KEY_LEFT, 0);
                CURVE_ACT(ca);
              }

              break;
            }
            default: {
              // other CSI sequences (Home, End, F1..F12)
              // are not bound; fall through to outer continue
              break;
            }
            }
          }
        } else if (browser.active) {
          // a lone ESC closes browser panel
          browser_key(&browser, BR_KEY_ESC, 0, NULL, 0);
          browser.active = 0;
          browser_clear_area(term_h, term_w);
        } else if (picker.active) {
          // a lone ESC closes picker
          picker_key(&picker, &catalog, PK_KEY_ESC, NULL);
        }

        continue;
      }

      // Browser panel open: it owns keyboard, same convention as plugin
      // picker - typing a filename can't accidentally trigger hotkeys below
      // (b/q/j/k/g/G all have meaning to browser)
      if (browser.active && ch == '\t') {
        // Tab switches between panel's tabs
        browser.tab = (browser.tab + 1) % BROWS_TAB_COUNT;
        curve_ui.grab = 0;

        continue;
      }

      if (browser.active && browser.tab == BROWS_TAB_CURVE) {
        curve_act_t ca = curve_ui_key(
            &curve_ui, &curve,
            (ch == '\r' || ch == '\n') ? BR_KEY_ENTER : BR_KEY_CHAR, ch);
        CURVE_ACT(ca);

        continue;
      }

      if (browser.active) {
        char br_path[BROWS_PATH_LEN];
        brows_action_t ba;
        // Enter arrives as '\r' (or '\n' on some terminals) - map it to
        // dedicated BR_KEY_ENTER kind so browser_key can act on it
        // regardless of which char terminal sent
        if (ch == '\r' || ch == '\n') {
          ba = browser_key(&browser, BR_KEY_ENTER, 0, br_path, sizeof(br_path));
        } else {
          ba = browser_key(&browser, BR_KEY_CHAR, ch, br_path, sizeof(br_path));
        }
        switch (ba) {
        case BROWS_ACT_CLOSE: {
          browser.active = 0;
          browser_clear_area(term_h, term_w);
          break;
        }
        case BROWS_ACT_PREVIEW_ANSI: {
          int aborted = 0;
          // Hide panel during preview so it doesn't overlap screenshot
          browser_clear_area(term_h, term_w);
          browser_preview_ansi(br_path, &aborted);
          // Live UI will fully redraw on next frame
          break;
        }
        case BROWS_ACT_REPLAY_CAST: {
          int aborted = 0;
          // Refuse to replay while recording - recording thread would pause
          // with main loop and corrupt in-flight .cast
          if (rec.active) {
            TOAST("stop recording (t) before replaying a .cast");
            break;
          }
          browser_clear_area(term_h, term_w);
          browser_replay_cast(br_path, &aborted);
          break;
        }
        case BROWS_ACT_INFO: {
          TOAST("%s", browser.info_msg);
          break;
        }
        default: {
          break;
        }
        }

        continue;
      }

      // Picker open: it owns keyboard, so typing a plugin name can't
      // trigger hotkeys below (w/e/c would all fire on "water")
      if (picker.active) {
        int load_idx = -1;
        picker_action_t pa =
            picker_key(&picker, &catalog, (unsigned char)ch, &load_idx);
        if (pa == PK_LOAD) {
          catalog_entry_t *ce = &catalog.e[load_idx];
          if (ce->state == PC_ACTIVE) {
            TOAST("%s is already loaded", ce->name);
          } else if (ce->state == PC_COMPILING) {
            ce->want_load = 1;
            TOAST("%s is already compiling", ce->name);
          } else if (ce->state == PC_READY) {
            // Already built (or shipped prebuilt): load it, no compiler
            // needed
            char lmsg[128];
            plugin_activate(&catalog, load_idx, plugins, plugin_params,
                            plugin_cat, &plugin_count, lmsg, sizeof(lmsg));
            TOAST("%s", lmsg);
          } else {
            ce->want_load = 1;
            if (catalog_start_build(&catalog, load_idx) == 0) {
              TOAST("compiling %s...", ce->name);
            } else {
              ce->want_load = 0;
              TOAST("%s: %s", ce->name, ce->err);
            }
          }
        }

        continue;
      }

      if (ch == '/') {
        if (!catalog_supported()) {
          TOAST("runtime plugin build not supported on this platform yet");
        } else {
          catalog_scan(&catalog); // pick up sources added since startup
          if (catalog.count == 0) {
            TOAST("no plugin sources in %s", catalog.src_dir);
          } else {
            picker_open(&picker);
          }
        }

        continue;
      }

      // Adjust plugin param if selected
      int *pp = (plugin_count > 0) ? &plugin_params[selected] : NULL;
      switch (ch) {
      case 'q':
      case 'Q': {
        keep_running = 0;

        break;
      }
      case ']': {
        if (pp && *pp < 255) {
          (*pp)++;
        }

        break;
      }
      case '[': {
        if (pp && *pp > 0) {
          (*pp)--;
        }

        break;
      }
      case '}': {
        if (pp) {
          *pp = (*pp + 10 > 255) ? 255 : *pp + 10;
        }

        break;
      }
      case '{': {
        if (pp) {
          *pp = (*pp - 10 < 0) ? 0 : *pp - 10;
        }

        break;
      }
      case 'r':
      case 'R': {
        if (pp) {
          *pp = plugin_default_param(plugins[selected].plugin);
        }

        break;
      }
      case 'm': {
        opts.render_mode = (opts.render_mode + 1) % RENDER_MODE_COUNT;

        break;
      }
      case 'M': {
        opts.render_mode =
            (opts.render_mode - 1 + RENDER_MODE_COUNT) % RENDER_MODE_COUNT;

        break;
      }
      case 'x': {
        opts.edges = (opts.edges + 1) % EDGE_MODE_COUNT;

        break;
      }
      case 'X': {
        opts.edges = (opts.edges - 1 + EDGE_MODE_COUNT) % EDGE_MODE_COUNT;

        break;
      }
      case '.':
      case ',': {
        if (slides_active()) {
          slides_request(ch == '.' ? 1 : -1);
        }

        break;
      }
      case 'n': {
        if (charsets.count > 0) {
          charsets.active = (charsets.active + 1) % charsets.count;
          opts.charset = charset_registry_active_ramp(&charsets);
        }

        break;
      }
      case 'N': {
        if (charsets.count > 0) {
          charsets.active =
              (charsets.active - 1 + charsets.count) % charsets.count;
          opts.charset = charset_registry_active_ramp(&charsets);
        }

        break;
      }
      case 'g': {
        opts.gamma = (opts.gamma - 10 < 10) ? 10 : opts.gamma - 10;

        break;
      }
      case 'G': {
        opts.gamma = (opts.gamma + 10 > 400) ? 400 : opts.gamma + 10;

        break;
      }
      case '+': {
        opts.depth_pop = (opts.depth_pop + 5 > 100) ? 100 : opts.depth_pop + 5;

        break;
      }
      case '-': {
        opts.depth_pop = (opts.depth_pop - 5 < 0) ? 0 : opts.depth_pop - 5;

        break;
      }
      case 'v': {
        opts.depth_invert = !opts.depth_invert;

        break;
      }
      case 's':
      case 'S': {
        want_shot = 1; // taken from next frame that is rendered

        break;
      }
      case 'b':
      case 'B': {
        // Toggle captures browser panel. Re-scan on open so freshly saved
        // screenshots / recordings show up without a restart
        browser_toggle(&browser);
        if (browser.active) {
          browser.tab = BROWS_TAB_FILES;
          browser_scan(&browser, cap_dir);
        } else {
          browser_clear_area(term_h, term_w);
        }

        break;
      }
      case 'k':
      case 'K': {
        // Tone curve editor: second tab of the same side panel
        if (!browser.active) {
          browser_toggle(&browser);
        }

        browser.tab = BROWS_TAB_CURVE;
        curve_ui.grab = 0;

        break;
      }
      case 't':
      case 'T': {
        if (rec.active) {
          double secs = 0;
          char tb[REC_TOAST_W + 8];
          int frames = rec_stop(&rec, rec_mono_ns(), &secs);
          nl_snprintf(saved_cast, sizeof(saved_cast), "%s", rec.path);
          TOAST("REC stopped, %ds: %s", (int)secs,
                path_tail(rec.path, tb, sizeof(tb)));
          (void)frames;
        } else if (rec_start(&rec, cap_dir, term_w, term_h, rec_unix_now(),
                             rec_mono_ns()) == 0) {
          char tb[REC_TOAST_W + 8];
          TOAST("REC started: %s", path_tail(rec.path, tb, sizeof(tb)));
        } else {
          char tb[REC_TOAST_W + 8];
          TOAST("cannot record into %s", path_tail(cap_dir, tb, sizeof(tb)));
        }

        break;
      }
      case 'u':
      case 'U': {
        if (plugin_count > 0) {
          const char *uname =
              plugins[selected].plugin ? plugins[selected].plugin->name : "?";
          TOAST("unloaded %s", uname);

          plugin_slot_remove(plugins, plugin_params, plugin_cat, &plugin_count,
                             &selected, selected, &catalog);
        }

        break;
      }
      case 'a':
      case 'A': {
        // hand control of frame size back to SIGWINCH; next loop iteration
        // picks up current terminal size
        ascii_size_manual = 0;
        term_resized = 1;

        break;
      }
      case 'e': {
        webcam_adjust_exposure(&cam, -10, &hw_exposure);

        break;
      }
      case 'E': {
        webcam_adjust_exposure(&cam, 10, &hw_exposure);

        break;
      }
      case 'w': {
        webcam_adjust_white_balance(&cam, -100, &hw_wb);

        break;
      }
      case 'W': {
        webcam_adjust_white_balance(&cam, 100, &hw_wb);

        break;
      }
      case 'c': {
        webcam_adjust_contrast(&cam, -5, &hw_contrast);

        break;
      }
      case 'C': {
        webcam_adjust_contrast(&cam, 5, &hw_contrast);

        break;
      }
      default: {
        break;
      }
      }
    }

    if (!keep_running) {
      break;
    }

    // Collect finished background builds; load ones user asked for
    {
      int fi;
      while (catalog_poll(&catalog, &fi)) {
        catalog_entry_t *ce = &catalog.e[fi];
        if (ce->state == PC_READY && ce->want_load) {
          ce->want_load = 0;

          char lmsg[128];
          plugin_activate(&catalog, fi, plugins, plugin_params, plugin_cat,
                          &plugin_count, lmsg, sizeof(lmsg));

          TOAST("%s", lmsg);
        } else if (ce->state == PC_READY) {
          TOAST("%s built", ce->name);
        } else {
          ce->want_load = 0;

          TOAST("%s build failed: %s", ce->name, ce->err);
        }
      }
    }

    // Hot-reload check for all plugins
    for (int i = 0; i < plugin_count; i++) {
      plugin_check_reload(&plugins[i]);
    }

    // Hot-reload check for charset ramps
    charset_registry_check_reload(&charsets);
    opts.charset = charset_registry_active_ramp(&charsets);

    // Frame capture. `frame` is luma plugins and renderer work on; `raw` is
    // camera's YUYV for this frame (colour needs it), NULL if unavailable
    uint8_t *frame = gray;
    const uint8_t *raw = NULL;

    if (pipe_on) {
      capture_frame_t *cf = NULL;
      int got = capture_pipe_acquire(&cap_pipe, 100, &cf);
      if (got < 0) {
        fprintf(stderr, "capture thread failed\n");

        break;
      }

      if (got == 0) {
        continue; // no new frame yet; keys were handled above
      }

      frame = cf->gray; // exclusively ours until next acquire
      raw = cf->has_yuyv ? cf->yuyv : NULL;
    } else {
      if (webcam_wait_frame(&cam, 1000) < 0) {
        continue; // timeout, retry
      }

      int cap = webcam_capture_frame(&cam, gray);
      if (cap < 0) {
        nl_perror("capture_frame");

        break;
      }

      if (cap > 0) {
        continue; // corrupt frame dropped, its buffer is already requeued
      }

      // NOTE: cam.buffer is V4L2 mmap region (Linux only)
      // On macOS, capture_macos.c delivers luma only; cam.buffer is NULL
      // TODO: Add color support for macOS
      // Color mode is therefore a Linux-only feature for now
      if (cam.buffer && cam.buffer != MAP_FAILED) {
        raw = (const uint8_t *)cam.buffer;
      }
    }

    if (slides_take_changed()) {
      TOAST("%d/%d  %s", slides_index() + 1, slides_count(),
            slides_name(slides_index()));
    }

    // Mirror/Flip horizontally if requested
    if (opts.mirror) {
      int w = cam.width;
      int h = cam.height;

      // Flip luma (grayscale) frame in-place
      for (int y = 0; y < h; y++) {
        uint8_t *row = frame + y * w;
        for (int x = 0; x < w / 2; x++) {
          uint8_t tmp = row[x];
          row[x] = row[w - 1 - x];
          row[w - 1 - x] = tmp;
        }
      }
    }

    // Run all plugins in order
    for (int i = 0; i < plugin_count; i++) {
      if (plugins[i].plugin) {
        plugins[i].plugin->process(frame, cam.width, cam.height,
                                   &plugin_params[i]);
      }
    }

    // frame already went through plugins, so use it as Y: shapes and colour
    // then come from same image (U/V are still camera's)
    if (opts.color && rgb && raw) {
      if (opts.mirror) {
        // luma was flipped above: take U/V from the opposite end as well
        yuyv_to_rgb_mirror(raw, cam.stride, frame, rgb, cam.width, cam.height);
      } else {
        yuyv_to_rgb_ex(raw, cam.stride, frame, rgb, cam.width, cam.height);
      }

      // Colour hooks of plugins (thermal palette, sepia tint, ...)
      for (int i = 0; i < plugin_count; i++) {
        if (plugins[i].plugin && plugins[i].rgb_process) {
          plugins[i].rgb_process(rgb, cam.width, cam.height, &plugin_params[i]);
        }
      }
    }

    // Calculate proper subpixel dimensions
    int subpixel_w;
    int subpixel_h;

    switch (opts.render_mode) {
    case RENDER_BRAILLE:
      subpixel_w = ascii_w * 2;
      subpixel_h = ascii_h * 4;

      break;
    case RENDER_HALF_BLOCK:
      subpixel_w = ascii_w * 1;
      subpixel_h = ascii_h * 2;

      break;
    default:
      // RENDER_BLOCKS, RENDER_ASCII_RAMP, RENDER_DOTS are 1x1 per cell
      subpixel_w = ascii_w * 2;
      subpixel_h = ascii_h * 4;

      break;
    }

    opts.origin_x = frame_x;
    opts.origin_y = frame_y;
    opts.tone_lut = curve_active(&curve) ? curve.lut : NULL;

    if (browser.active && browser.tab == BROWS_TAB_CURVE) {
      nl_memset(curve_ui.hist, 0, sizeof(curve_ui.hist));
      opts.tone_hist = curve_ui.hist; // histogram behind curve
    } else {
      opts.tone_hist = NULL;
    }

    int len = grayscale_to_ascii(frame, rgb, cam.width, cam.height, subpixel_w,
                                 subpixel_h, out_buf, out_size, &opts);
    if (len > (int)out_size) {
      fprintf(stderr, "WARNING: len=%d > out_size=%zu\n", len, out_size);

      // Characters are only made when shown, recorded or saved
      int len = 0;
      if (gfx_mode == GFX_OFF || rec.active || want_shot) {
        len = grayscale_to_ascii(frame, rgb, cam.width, cam.height, subpixel_w,
                                 subpixel_h, out_buf, out_size, &opts);
        if (len > (int)out_size) {
          fprintf(stderr, "WARNING: len=%d > out_size=%zu\n", len, out_size);
        }
      }

      // Picture for --gfx: processed frame scaled to terminal to show in
      // frame's cells
      int gfx_len = 0;
      if (gfx_mode != GFX_OFF) {
        int pw = ascii_w * 4;
        int ph = ascii_h * 8;
        if (gfx_mode == GFX_SIXEL) {
          int cw, chh;
          query_cell_px(&cw, &chh);
          pw = ascii_w * cw;
          ph = ascii_h * chh;
          // keep picture inside one sixel costs on wire
          while (pw > 1024 || ph > 1024) {
            pw = pw * 3 / 4;
            ph = ph * 3 / 4;
          }
        } else {
          pw = pw > cam.width ? cam.width : pw;
          ph = ph > cam.height ? cam.height : ph;
        }

        pw = pw < 1 ? 1 : pw;
        ph = ph < 1 ? 1 : ph;

        size_t need_img = (size_t)pw * (size_t)ph * 3u;
        size_t need_out = gfx_mode == GFX_KITTY ? gfx_kitty_cap(pw, ph)
                                                : gfx_sixel_cap(pw, ph);
        if (need_img > gfx_img_cap) {
          nl_free(gfx_img);
          gfx_img = nl_malloc(need_img);
          gfx_img_cap = gfx_img ? need_img : 0;
        }

        if (need_out > gfx_out_cap) {
          nl_free(gfx_out);
          gfx_out = nl_malloc(need_out);
          gfx_out_cap = gfx_out ? need_out : 0;
        }

        if (gfx_img && gfx_out &&
            gfx_resample(rgb && opts.color && raw ? NULL : frame,
                         rgb && opts.color && raw ? rgb : NULL, cam.width,
                         cam.height, pw, ph, gfx_img) == 0) {
          char cup[24];
          int cn =
              nl_snprintf(cup, sizeof(cup), "\033[%d;%dH", frame_y, frame_x);
          int en =
              gfx_mode == GFX_KITTY
                  ? gfx_kitty_encode(gfx_img, pw, ph, ascii_w, ascii_h,
                                     gfx_out + cn, gfx_out_cap - (size_t)cn)
                  : gfx_sixel_encode(gfx_img, pw, ph,
                                     !(rgb && opts.color && raw), gfx_out + cn,
                                     gfx_out_cap - (size_t)cn);
          if (cn > 0 && en > 0) {
            nl_memcpy(gfx_out, cup, (size_t)cn);
            gfx_len = cn + en;
          }
        }
      }

      if (len > 0 || gfx_len > 0) {
        write_all(STDOUT_FILENO, SYNC_BEGIN, sizeof(SYNC_BEGIN) - 1);
        const char *wire = out_buf;
        size_t wire_len = (size_t)len;
        if (net_kbps >= 0) {
          // delta is never much bigger than frame itself and sizing it to frame
          // (not to out_size, worst case over all render modes) keeps it out of
          // allocator's big-block path
          size_t need = (size_t)len + (size_t)len / 4 + 4096;
          if (net_buf_cap < need) {
            nl_free(net_buf);
            net_buf = malloc(need);
            net_buf_cap = net_buf ? need : 0;
            if (!net_buf && !net_warned) {
              net_warned = 1;
              fprintf(stderr, "--net: out of memory, sending full frames\n");
            }
          }

          // repaint everything now and then so a stray character (a terminal
          // hiccup, a write from elsewhere) cannot stay on screen for long
          if (++net_age >= fps * 10) {
            net_dirty = 1;
          }

          if (net_buf) {
            if (net_dirty) {
              netenc_invalidate(&net);
              net_dirty = 0;
              net_age = 0;
            }

            net.budget =
                net_kbps > 0 ? net_kbps * 125 / (fps > 0 ? fps : 1) : 0;
            if (net.budget > 0 && net.budget < 300) {
              net.budget = 300;
            }

            int en =
                netenc_frame(&net, out_buf, (size_t)len, net_buf, net_buf_cap);
            if (en >= 0) {
              wire = net_buf;
              wire_len = (size_t)en;
            } else {
              netenc_invalidate(
                  &net); // unknown escapes: send as is, then resync
            }
          }
        }

        if (gfx_len > 0) {
          write_all(STDOUT_FILENO, gfx_out, (size_t)gfx_len);
        } else {
          write_all(STDOUT_FILENO, out_buf, (size_t)len);
        }

        if (vcam_on && vcam_frame(&vcam, out_buf, (size_t)len) < 0) {
          TOAST("virtual camera stopped: cannot write to %s", vcam_path);
          vcam_close(&vcam);
          vcam_on = 0;
        }

        uint64_t rec_now = rec_mono_ns();
        if (rec.active && len > 0 &&
            rec_frame(&rec, rec_now, out_buf, (size_t)len) < 0) {
          TOAST("REC stopped: cannot write %s", rec.path);
        }

        if (want_shot && len > 0) {
          want_shot = 0;
          int have_rgb = opts.color && rgb && raw;
          char png_p[REC_PATH_LEN], ansi_p[REC_PATH_LEN], tb[REC_TOAST_W + 8];
          int rc = shot_save(cap_dir, rec_unix_now(), have_rgb ? rgb : frame,
                             cam.width, cam.height, have_rgb ? 3 : 1, out_buf,
                             (size_t)len, term_h, png_p, sizeof(png_p), ansi_p,
                             sizeof(ansi_p));
          if (ansi_p[0]) {
            nl_snprintf(saved_shot, sizeof(saved_shot), "%s",
                        png_p[0] ? png_p : ansi_p);
          }

          if (rc == 0) {
            TOAST("saved %s", path_tail(png_p, tb, sizeof(tb)));
          } else if (ansi_p[0]) {
            TOAST("saved text only: %s", path_tail(ansi_p, tb, sizeof(tb)));
          } else {
            TOAST("cannot save into %s", path_tail(cap_dir, tb, sizeof(tb)));
          }
        }

        frame_rect_t shown = {frame_x, frame_y, ascii_w, ascii_h};
        overlay_panel(panel_row_for(term_h, ascii_h), &shown, &mouse_drag,
                      current_fps, plugins, plugin_params, plugin_count,
                      selected, opts.color, &opts, &charsets, hw_exposure,
                      hw_contrast, hw_wb, cam.width, cam.height,
                      ascii_size_manual);
        draw_corner_indicators(&shown, opts.color, &mouse_drag);
        draw_rec_indicator(panel_row_for(term_h, ascii_h), term_w, &rec,
                           rec_now, opts.color);

        if (outline_on && mouse_drag.dragging) {
          draw_drag_outline(&outline, opts.color, 0);
        }

        // Captures browser panel: drawn inside SYNC region so it appears
        // atomically with frame, no flicker while it's being scrolled
        if (browser.active) {
          char br_buf[16384];
          int br_n = browser.tab == BROWS_TAB_CURVE
                         ? curve_ui_render(&curve_ui, &curve, term_h, term_w,
                                           opts.color, br_buf, sizeof(br_buf))
                         : browser_render(&browser, term_h, term_w, opts.color,
                                          br_buf, sizeof(br_buf));
          if (br_n > 0) {
            write_all(STDOUT_FILENO, br_buf, (size_t)br_n);
          }
        }

        {
          // Tab strip: beside open panel, or at right edge when retracted
          char st_buf[1024];
          int st_n =
              browser_strip_render(st_buf, sizeof(st_buf), term_h, term_w,
                                   browser.active, browser.tab, opts.color);
          if (st_n > 0) {
            write_all(STDOUT_FILENO, st_buf, (size_t)st_n);
          }
        }

        write_all(STDOUT_FILENO, SYNC_END, sizeof(SYNC_END) - 1);

        // Picker box, or a toast / build indicator, over bottom frame rows
        char ovl[2048];
        int on = 0;
        picker_set_column(frame_x);
        int frame_bottom = frame_y + ascii_h - 1;
        if (picker.active) {
          on = picker_render(&picker, &catalog, frame_bottom, opts.color, ovl,
                             sizeof(ovl));
        } else if (toast_frames > 0) {
          toast_frames--;
          on = toast_render(toast_msg, frame_bottom, opts.color, ovl,
                            sizeof(ovl));
        } else {
          char bmsg[128];
          size_t bl = 0;
          for (int i = 0; i < catalog.count; i++) {
            if (catalog.e[i].state != PC_COMPILING) {
              continue;
            }
            if (bl == 0) {
              bl = (size_t)nl_snprintf(bmsg, sizeof(bmsg), "compiling:");
            }

            int w = nl_snprintf(bmsg + bl, sizeof(bmsg) - bl, " %s",
                                catalog.e[i].name);
            if (w > 0 && bl + (size_t)w < sizeof(bmsg)) {
              bl += (size_t)w;
            }
          }

          if (bl > 0) {
            on = toast_render(bmsg, frame_bottom, opts.color, ovl, sizeof(ovl));
          }
        }

        if (on > 0) {
          (void)write(STDOUT_FILENO, ovl, (size_t)on);
        }

        // An overlay that just went away leaves its cells wrong on screen
        int overlay_now = on > 0 || browser.active || mouse_drag.dragging;
        if (browser_was_open && !browser.active) {
          net_dirty = 1; // panel retracted: repaint what it covered
        }

        browser_was_open = browser.active;

        if (net_prev_overlay && !overlay_now) {
          net_dirty = 1;
        }

        net_prev_overlay = overlay_now;
      }

      if (!pipe_on && webcam_requeue_buffer(&cam) < 0) {
        nl_perror("requeue_buffer");

        break;
      }

      timing_sleep(&frame_start);
    }

    // Cleanup
    mouse_disable();
    term_restore();

    if (gfx_mode == GFX_KITTY) {
      char del[32];
      int dn = gfx_kitty_delete(del, sizeof(del));
      if (dn > 0) {
        (void)write(STDOUT_FILENO, del, (size_t)dn);
      }
    } else if (gfx_mode == GFX_SIXEL) {
      (void)write(STDOUT_FILENO, "\033[?80l", 6);
    }

    fprintf(stderr, "Stopped.\n");

    if (vcam_on) {
      fprintf(stderr, "Virtual camera: %u frames sent to %s\n",
              (unsigned)vcam.frames, vcam_path);
      vcam_close(&vcam);
    }

    if (rec.active) {
      double secs = 0;
      int frames = rec_stop(&rec, rec_mono_ns(), &secs);
      nl_snprintf(saved_cast, sizeof(saved_cast), "%s", rec.path);
      fprintf(stderr, "Recording ended: %d frames, %d s\n", frames, (int)secs);
    }

    if (saved_cast[0]) {
      fprintf(stderr, "Recording saved: %s\n", saved_cast);
    }

    if (saved_shot[0]) {
      fprintf(stderr, "Screenshot saved: %s\n", saved_shot);
    }

    // Restore main screen, reset attributes, and explicitly show cursor
    // \033[?1049l = erase screen, \033[0m = cursor home, \033[?25h = show
    // cursor
    // static const char CLEANUP[] = SYNC_END "\033[?1049l\033[0m\033[?25h";
    static const char CLEANUP[] = "\033[?1049l\033[0m\033[?25h";
    (void)write(STDOUT_FILENO, CLEANUP, sizeof(CLEANUP) - 1);

    nl_free(gray);
    nl_free(rgb);
    nl_free(out_buf);
    nl_free(gfx_img);
    nl_free(gfx_out);
    if (net_kbps >= 0) {
      netenc_free(&net);
      nl_free(net_buf);
    }

    for (int i = 0; i < plugin_count; i++) {
      plugin_cleanup(&plugins[i]);
    }

    catalog_cleanup(&catalog);
    charset_registry_cleanup(&charsets);
    capture_pipe_stop(&cap_pipe); // before camera it reads from is closed
    webcam_cleanup(&cam);

    return 0;
  }
