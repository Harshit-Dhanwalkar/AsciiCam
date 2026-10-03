#include "nolibc.h"

#include "ascii.h"
#include "capture.h"
#include "mouse.h"
#include "plugin_catalog.h"
#include "plugin_picker.h"
#include "plugins.h"
// #include "thread_sharing.h"
#include "timing.h"

// Defaults
#define DEFAULT_ASCII_WIDTH 80
#define DEFAULT_ASCII_HEIGHT 40
#define DEFAULT_CAPTURE_WIDTH 640
#define DEFAULT_CAPTURE_HEIGHT 480
#define DEFAULT_FPS 20
#define MAX_PLUGINS 8

#define DEFAULT_CONFIG_PATH ".asciicamrc"
#define DEFAULT_CHARSET_DIR "./charsets"
#define DEFAULT_PLUGIN_DIR "./filters"          // plugin sources to browse
#define DEFAULT_PLUGIN_CACHE "./.cache/plugins" // where runtime builds land
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

static const char *g_plugin_dir = DEFAULT_PLUGIN_DIR;
static const char *g_plugin_cache = DEFAULT_PLUGIN_CACHE;

static volatile sig_atomic_t raw_mode_active = 0;
static struct termios orig_terminal;

static void emergency_terminal_restore(void) {
  if (raw_mode_active) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_terminal);
  }

  // otherwise shell keeps receiving mouse reports as garbage text
  mouse_disable();

  static const char SHOW_CURSOR[] = "\033[?25h\033[0m\n";
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
      "  -d <device>   video device             (default: /dev/video0)\n"
      "  -w <width>    capture width            (default: %d)         \n"
      "  -h <height>   capture height           (default: %d)         \n"
      "  -f <fps>      target framerate         (default: %d)         \n"
      "\n"
      "Output options:\n"
      "  -W <width>    ASCII output columns     (default: %d)          \n"
      "  -H <height>   ASCII output rows        (default: %d)          \n"
      "  -s <chars>    custom charset string    (default: \"%s\")      \n"
      "  -p <path>     filter plugin .so path (repeatable)             \n"
      "  -L <dir>      plugin source dir for live picker (default: %s) \n"
      "  -m <mode>     render mode: braille|blocks|ascii|halfblock|dots\n"
      "  -k <dir>      charset directory (hot-reloadable .txt ramps)   \n"
      "\n"
      "Image adjustments:\n"
      "  -b <val>      brightness offset        -128..128  (default: 0)\n"
      "  -c <val>      contrast in percent      >0; 100=none (default: 100)\n"
      "  -g <val>      gamma percent          10..400; 100=none (default: "
      "100)\n"
      "  -i            invert mapping                                  \n"
      "  -E <mode>     edge mode                off|sobel|sobel-dir|laplacian\n"
      "  -C            ANSI truecolor output                           \n"
      "  -2            ANSI-256 color output (fallback for terminals   \n"
      "                without truecolor support, implies color on)    \n"
      "  -D            Floyd-Steinberg dithering                       \n"
      "  -P <0-100>    depth-pop 3D parallax strength (0=off)          \n"
      "\n"
      "Config file:\n"
      "  ./.asciicamrc, key=value per line (# comments), auto-loaded   \n"
      "  if present. CLI flags above always override it.               \n"
      "\n"
      "Live keybindings:\n"
      "  m / M         cycle render mode forward / backward            \n"
      "  x / X         cycle edge detection mode forward / backward    \n"
      "  n / N         cycle loaded charset forward / backward         \n"
      "  p / o         increase / decrease depth-pop strength          \n"
      "  g / G         decrease / increase gamma                        \n"
      "  e / E         hw exposure down / up        (V4L2, Linux only) \n"
      "  w / W         hw white-balance down / up   (V4L2, Linux only) \n"
      "  c / C         hw contrast down / up        (V4L2, Linux only) \n"
      "  up/down       select plugin    [ ] +-1   { } +-10   r reset   \n"
      "  /             search plugin sources, enter builds + loads it  \n"
      "                (up/down move, esc closes; compiles in background)\n"
      "  u             unload the selected plugin                      \n"
      "  mouse         drag bottom-right corner to resize ASCII        \n"
      "                frame; applied on release                       \n"
      "  a / A         return frame to terminal auto-fit               \n"
      "  q             quit                                            \n",
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

static void overlay_panel(int ascii_h, double fps, plugin_loader_t *plugins,
                          const int *plugin_params, int count, int selected,
                          int color, const ascii_opts_t *opts,
                          const charset_registry_t *charsets, int hw_exposure,
                          int hw_contrast, int hw_wb, int cap_w, int cap_h,
                          int preview_w, int preview_h, int ascii_w_now,
                          int ascii_size_manual) {
  char buf[1024];
  int n;
  int base_row = ascii_h + 1; // 1-indexed panel row

  // FPS + hint bar
  char fpsbuf[10];
  nl_fmt_fps(fpsbuf, sizeof(fpsbuf), fps);
  if (preview_w > 0) {
    // mouse drag in progress: preview of the pending frame size
    if (color) {
      n = nl_snprintf(buf, sizeof(buf),
                      "\033[%d;1H\033[38;2;255;60;60m\033[48;2;18;18;18m"
                      " RESIZING frame %dx%d -> %dx%d  │  release to apply"
                      "\033[0m\033[K",
                      base_row, ascii_w_now, ascii_h, preview_w, preview_h);
    } else {
      n = nl_snprintf(buf, sizeof(buf),
                      "\033[%d;1H RESIZING frame %dx%d -> %dx%d  |  release "
                      "to apply\033[K",
                      base_row, ascii_w_now, ascii_h, preview_w, preview_h);
    }
  } else if (color) {
    n = nl_snprintf(buf, sizeof(buf),
                    "\033[%d;1H\033[38;2;0;220;0m\033[48;2;18;18;18m"
                    " FPS: %s  │  ↑↓ select  [ ] ±1  { } ±10  r reset  q quit "
                    " │  frame: %dx%d%s  cap: %dx%d  ◢ drag"
                    "\033[0m\033[K",
                    base_row, fpsbuf, ascii_w_now, ascii_h,
                    ascii_size_manual ? " [manual]" : "", cap_w, cap_h);
  } else {
    n = nl_snprintf(buf, sizeof(buf),
                    "\033[%d;1H FPS: %s  |  up/dn select  [ ] +-1  { } +-10  "
                    "r reset  q quit  |  frame: %dx%d%s  cap: %dx%d  drag +"
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

// Active-plugin list. plugins[]/params[]/cat[] stay packed, so adding and
// removing at runtime is just append / shift-down. cat[i] is catalog entry a
// slot was loaded from, or -1 for plugins given with -p
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

// Load a built catalog entry into the filter chain. msg gets the toast text
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

// Main
int main(int argc, char *argv[]) {
  for (int i = 1; i < argc; i++) {
    if (nl_strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);

      return 0;
    }
  }

  nl_signal(SIGINT, handle_signal);
  nl_signal(SIGTERM, handle_signal);
  nl_signal(SIGHUP, handle_signal);
  nl_signal(SIGQUIT, handle_signal);
  nl_signal(SIGABRT, handle_signal);
  nl_signal(SIGSEGV, handle_signal);
  nl_signal(SIGWINCH, handle_winch);

  // Config
  char *device = "/dev/video0";
  int ascii_w = DEFAULT_ASCII_WIDTH;
  int ascii_h = DEFAULT_ASCII_HEIGHT;
  int cap_w = DEFAULT_CAPTURE_WIDTH;
  int cap_h = DEFAULT_CAPTURE_HEIGHT;
  int fps = DEFAULT_FPS;
  const char *charset_dir = DEFAULT_CHARSET_DIR;

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
                          "d:W:H:w:h:f:b:c:g:iCD2s:p:L:m:E:k:P:")) != -1)
    switch (opt) {
    case 'd':
      device = optarg;

      break;
    case 'W':
      ascii_w = my_atoi(optarg);
      if (ascii_w <= 0) {
        ascii_w = DEFAULT_ASCII_WIDTH;
      }

      break;
    case 'H':
      ascii_h = my_atoi(optarg);
      if (ascii_h <= 0) {
        ascii_h = DEFAULT_ASCII_HEIGHT;
      }

      break;
    case 'w':
      cap_w = my_atoi(optarg);
      if (cap_w <= 0) {
        cap_w = DEFAULT_CAPTURE_WIDTH;
      }

      break;
    case 'h':
      cap_h = my_atoi(optarg);
      if (cap_h <= 0) {
        cap_h = DEFAULT_CAPTURE_HEIGHT;
      }

      break;
    case 'f':
      fps = my_atoi(optarg);
      if (fps <= 0) {
        fps = DEFAULT_FPS;
      }

      break;
    case 'b':
      opts.brightness = my_atoi(optarg);

      break;
    case 'c':
      opts.contrast = my_atoi(optarg);
      if (opts.contrast <= 0) {
        opts.contrast = 100;
      }

      break;
    case 'i':
      opts.invert = 1;

      break;
    case 'C':
      opts.color = 1;
      opts.color_mode = COLOR_TRUECOLOR;

      break;
    case '2':
      opts.color = 1;
      opts.color_mode = COLOR_256;

      break;
    case 'g':
      opts.gamma = my_atoi(optarg);
      if (opts.gamma < 10) {
        opts.gamma = 10;
      }
      if (opts.gamma > 400) {
        opts.gamma = 400;
      }

      break;
    case 'E':
      opts.edges = parse_edge_mode(optarg);

      break;
    case 'm':
      opts.render_mode = parse_render_mode(optarg);

      break;
    case 'k':
      charset_dir = optarg;

      break;
    case 'P':
      opts.depth_pop = my_atoi(optarg);
      if (opts.depth_pop < 0) {
        opts.depth_pop = 0;
      }
      if (opts.depth_pop > 100) {
        opts.depth_pop = 100;
      }

      break;
    case 'D':
      opts.dither = 1;

      break;
    case 's':
      opts.charset = optarg;

      break;
    case 'L':
      g_plugin_dir = optarg;

      break;
    case 'p':
      if (plugin_path_count < MAX_PLUGINS) {
        plugin_paths[plugin_path_count++] = optarg;
      } else {
        fprintf(stderr, "Warning: max %d plugins, ignoring %s\n", MAX_PLUGINS,
                optarg);
      }

      break;

    default:
      print_usage(argv[0]);

      return 1;
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

  // From here on TUI owns terminal: load problems are reported in UI
  // (status_msg), not on stderr
  plugin_log_stderr = 0;

  // Catalog of plugin sources that can be built and loaded while running
  plugin_catalog_t catalog;
  catalog_init(&catalog, g_plugin_dir, g_plugin_cache,
#ifdef __LINUX_NOLIBC__
               argv + argc + 1 // envp follows argv NULL terminator
#else
               (char **)0
#endif
  );
  catalog_scan(&catalog);

  picker_t picker;
  nl_memset(&picker, 0, sizeof(picker));
  char toast_msg[128];
  toast_msg[0] = '\0';
  int toast_frames = 0;

#define TOAST(...)                                                             \
  do {                                                                         \
    nl_snprintf(toast_msg, sizeof(toast_msg), __VA_ARGS__);                    \
                                                                               \
    toast_frames = fps * TOAST_SECONDS;                                        \
  } while (0)

  int selected = 0;

  // Open webcam
  webcam_t cam = {.fd = -1, .buffer = MAP_FAILED};
  if (webcam_init(&cam, device, cap_w, cap_h) < 0) {
    nl_perror("webcam_init");

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
  uint8_t *gray = nl_malloc(cam_pixels);
  uint8_t *rgb = opts.color ? malloc(cam_pixels * 3) : NULL;

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

  // // Thead sharing
  // shared_frame_t sf = {0};
  // sf.buf[0] = malloc(cam_pixels);
  // sf.buf[1] = malloc(cam_pixels);
  // sf.width  = cam.width;  sf.height = cam.height;
  // sf.ascii_w = ascii_w;   sf.ascii_h = ascii_h;
  // sf.opts   = opts;
  // pthread_mutex_init(&sf.lock, NULL);
  // pthread_cond_init(&sf.cond, NULL);
  //
  // pthread_t tid_cap, tid_render;
  // pthread_create(&tid_cap,    NULL, capture_thread, &sf);
  // pthread_create(&tid_render, NULL, render_thread,  &sf);
  //
  // sf.stop = 1;
  // pthread_cond_broadcast(&sf.cond);
  // pthread_join(tid_cap,    NULL);
  // pthread_join(tid_render, NULL);

  // Initial screen setup
  (void)write(STDOUT_FILENO, "\033[2J\033[H\033[?25l", 13);

  term_raw_mode();
  mouse_enable();

  mouse_parser_t mouse_parser = {0, {0, 0, 0}, 0};
  mouse_drag_t mouse_drag = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  int preview_w = 0, preview_h = 0; // pending frame size while dragging
  // 0 = frame follows terminal width (SIGWINCH); 1 = user dragged it
  int ascii_size_manual = 0;

  struct timespec frame_start, last_frame_time;
  clock_gettime(CLOCK_MONOTONIC, &frame_start);
  last_frame_time = frame_start;

  // Main loop
  while (keep_running) {
    clock_gettime(CLOCK_MONOTONIC, &frame_start);

    long frame_diff_ns =
        (frame_start.tv_sec - last_frame_time.tv_sec) * 1000000000L + // Seconds
        (frame_start.tv_nsec - last_frame_time.tv_nsec); // Nano seconds

    if (frame_diff_ns > 0) {
      fps_push(&fps_calc, frame_diff_ns);
    }

    last_frame_time = frame_start;
    double current_fps = fps_get(&fps_calc);

    // Apply any pending terminal resize (SIGWINCH) before drawing this frame.
    // If user has manually sized frame with mouse, terminal auto-fit is
    // suppressed until press 'a'
    if (term_resized) {
      term_resized = 0;

      // a mid-drag SIGWINCH would corrupt drag's frame-base snapshot
      if (mouse_drag.dragging) {
        mouse_drag.dragging = 0;
        preview_w = preview_h = 0;
      }

      if (!ascii_size_manual) {
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
    //     int n = nl_snprintf(b, sizeof(b), "[winch #%d]\n", (int)winch_count);
    //     if (n > 0) {
    //       write(2, b, (size_t)n);
    //     }
    //   }
    // }

    // Keypress handling
    char ch;
    while (read(STDIN_FILENO, &ch, 1) == 1) {
      // Inside an SGR mouse report (ESC [ < b ; x ; y M|m): bytes must not
      // reach key handler, 'M' would cycle render mode
      if (mouse_parser_active(&mouse_parser)) {
        mouse_event_t mev;
        int prc = mouse_parser_feed(&mouse_parser, ch, &mev);
        if (prc == MOUSE_PARSE_MORE) {
          continue;
        }

        if (prc == MOUSE_PARSE_DONE) {
          int nw = 0;
          int nh = 0;
          // Drag resizes the ASCII frame 1:1 in cells. Capture resolution is
          // controlled only by -w/-h at startup and is not touched here, so
          // frame's aspect and camera's aspect stay independent
          int act =
              mouse_drag_event(&mouse_drag, &mev, ascii_w, ascii_h, ascii_w,
                               ascii_h, MIN_ASCII_W, MIN_ASCII_H,
                               DRAG_MAX_ASCII_W, DRAG_MAX_ASCII_H, &nw, &nh);
          if (act == MOUSE_GRAB || act == MOUSE_RESIZE) {
            preview_w = nw; // shown in panel; buffer untouched
            preview_h = nh;
          } else if (act == MOUSE_RELEASE) {
            preview_w = preview_h = 0;
          } else if (act == MOUSE_APPLY) {
            preview_w = preview_h = 0;
            ascii_size_manual = 1;
            if (apply_ascii_size(nw, nh, &ascii_w, &ascii_h, &out_buf,
                                 &out_size, opts.color) > 0) {
              (void)write(STDOUT_FILENO, "\033[2J", 4);
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
        char seq[2] = {0, 0};
        if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
          if (read(STDIN_FILENO, &seq[1], 1) == 1) {
            if (seq[1] == '<') {
              mouse_parser_begin(&mouse_parser);

              continue;
            }

            if (seq[1] == 'M') {
              // Legacy X10 report (ESC [ M b x y) from a terminal that has no
              // SGR mode: swallow 3 payload bytes, they are not keys (a
              // coordinate byte can equal 'q')
              char junk;
              for (int k = 0; k < 3; k++) {
                if (read(STDIN_FILENO, &junk, 1) != 1) {
                  break;
                }
              }

              continue;
            }

            switch (seq[1]) {
            case 'A': // up arrow key, previous plugin / picker row
              if (picker.active) {
                picker_key(&picker, &catalog, PK_KEY_UP, NULL);
              } else if (plugin_count > 0) {
                selected = (selected - 1 + plugin_count) % plugin_count;
              }

              break;
            case 'B': // down arrow key, next plugin / picker row
              if (picker.active) {
                picker_key(&picker, &catalog, PK_KEY_DOWN, NULL);
              } else if (plugin_count > 0) {
                selected = (selected + 1) % plugin_count;
              }

              break;
            }
          }
        } else if (picker.active) {
          // a lone ESC closes the picker
          picker_key(&picker, &catalog, PK_KEY_ESC, NULL);
        }

        continue;
      }

      // Picker open: it owns keyboard, so typing a plugin name can't trigger
      // hotkeys below (w/e/c would all fire on "water")
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
      case 'Q':
        keep_running = 0;

        break;
      case ']':
        if (pp && *pp < 255) {
          (*pp)++;
        }

        break;
      case '[':
        if (pp && *pp > 0) {
          (*pp)--;
        }

        break;
      case '}':
        if (pp) {
          *pp = (*pp + 10 > 255) ? 255 : *pp + 10;
        }

        break;
      case '{':
        if (pp) {
          *pp = (*pp - 10 < 0) ? 0 : *pp - 10;
        }

        break;
      case 'r':
      case 'R':
        if (pp) {
          *pp = 128;
        }

        break;
      case 'm':
        opts.render_mode = (opts.render_mode + 1) % RENDER_MODE_COUNT;

        break;
      case 'M':
        opts.render_mode =
            (opts.render_mode - 1 + RENDER_MODE_COUNT) % RENDER_MODE_COUNT;

        break;
      case 'x':
        opts.edges = (opts.edges + 1) % EDGE_MODE_COUNT;

        break;
      case 'X':
        opts.edges = (opts.edges - 1 + EDGE_MODE_COUNT) % EDGE_MODE_COUNT;

        break;
      case 'n':
        if (charsets.count > 0) {
          charsets.active = (charsets.active + 1) % charsets.count;
          opts.charset = charset_registry_active_ramp(&charsets);
        }

        break;
      case 'N':
        if (charsets.count > 0) {
          charsets.active =
              (charsets.active - 1 + charsets.count) % charsets.count;
          opts.charset = charset_registry_active_ramp(&charsets);
        }

        break;
      case 'g':
        opts.gamma = (opts.gamma - 10 < 10) ? 10 : opts.gamma - 10;

        break;
      case 'G':
        opts.gamma = (opts.gamma + 10 > 400) ? 400 : opts.gamma + 10;

        break;
      case '+':
        opts.depth_pop = (opts.depth_pop + 5 > 100) ? 100 : opts.depth_pop + 5;

        break;
      case '-':
        opts.depth_pop = (opts.depth_pop - 5 < 0) ? 0 : opts.depth_pop - 5;

        break;
      case 'v':
        opts.depth_invert = !opts.depth_invert;

        break;
      case 'u':
      case 'U':
        if (plugin_count > 0) {
          const char *uname =
              plugins[selected].plugin ? plugins[selected].plugin->name : "?";
          TOAST("unloaded %s", uname);

          plugin_slot_remove(plugins, plugin_params, plugin_cat, &plugin_count,
                             &selected, selected, &catalog);
        }

        break;
      case 'a':
      case 'A':
        // hand control of frame size back to SIGWINCH; the next loop
        // iteration picks up the current terminal size
        ascii_size_manual = 0;
        term_resized = 1;

        break;
      case 'e':
        webcam_adjust_exposure(&cam, -10, &hw_exposure);

        break;
      case 'E':
        webcam_adjust_exposure(&cam, 10, &hw_exposure);

        break;
      case 'w':
        webcam_adjust_white_balance(&cam, -100, &hw_wb);

        break;
      case 'W':
        webcam_adjust_white_balance(&cam, 100, &hw_wb);

        break;
      case 'c':
        webcam_adjust_contrast(&cam, -5, &hw_contrast);

        break;
      case 'C':
        webcam_adjust_contrast(&cam, 5, &hw_contrast);

        break;
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

    // Frame capture
    if (webcam_wait_frame(&cam, 1000) < 0) {
      continue; // timeout, retry
    }

    if (webcam_capture_frame(&cam, gray) < 0) {
      nl_perror("capture_frame");

      break;
    }

    // Run all plugins in order
    for (int i = 0; i < plugin_count; i++) {
      if (plugins[i].plugin) {
        plugins[i].plugin->process(gray, cam.width, cam.height,
                                   &plugin_params[i]);
      }
    }

    // NOTE: cam.buffer is the V4L2 mmap region (Linux only)
    // On macOS, capture_macos.c delivers luma only; cam.buffer is NULL
    // TODO: Add color support for macOS
    // Color mode is therefore a Linux-only feature for now
    if (opts.color && rgb && cam.buffer && cam.buffer != MAP_FAILED) {
      yuyv_to_rgb((const uint8_t *)cam.buffer, rgb, cam.width, cam.height);
    }

    // Calculate proper subpixel dimensions
    int subpixel_w = ascii_w;
    int subpixel_h = ascii_h;

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

    int len = grayscale_to_ascii(gray, rgb, cam.width, cam.height, subpixel_w,
                                 subpixel_h, out_buf, out_size, &opts);
    if (len > (int)out_size) {
      fprintf(stderr, "WARNING: len=%d > out_size=%zu\n", len, out_size);
    }

    if (len > 0) {
      (void)write(STDOUT_FILENO, out_buf, (size_t)len);

      overlay_panel(ascii_h, current_fps, plugins, plugin_params, plugin_count,
                    selected, opts.color, &opts, &charsets, hw_exposure,
                    hw_contrast, hw_wb, cam.width, cam.height, preview_w,
                    preview_h, ascii_w, ascii_size_manual);
      draw_corner_indicator(ascii_w, ascii_h, opts.color, &mouse_drag);

      // Picker box, or a toast / build indicator, over bottom frame rows
      char ovl[2048];
      int on = 0;
      if (picker.active) {
        on = picker_render(&picker, &catalog, ascii_h, opts.color, ovl,
                           sizeof(ovl));
      } else if (toast_frames > 0) {
        toast_frames--;
        on = toast_render(toast_msg, ascii_h, opts.color, ovl, sizeof(ovl));
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
          on = toast_render(bmsg, ascii_h, opts.color, ovl, sizeof(ovl));
        }
      }

      if (on > 0) {
        (void)write(STDOUT_FILENO, ovl, (size_t)on);
      }
    }

    if (webcam_requeue_buffer(&cam) < 0) {
      nl_perror("requeue_buffer");

      break;
    }

    timing_sleep(&frame_start);
  }

  // Cleanup
  mouse_disable();
  term_restore();
  // \033[2J = erase screen, \033[H = cursor home, \033[?25h = show cursor
  static const char CLEANUP[] = "\033[2J\033[H\033[0m\033[?25h";
  (void)write(STDOUT_FILENO, CLEANUP, sizeof(CLEANUP) - 1);

  fprintf(stderr, "Stopped.\n");

  nl_free(gray);
  nl_free(rgb);
  nl_free(out_buf);
  for (int i = 0; i < plugin_count; i++) {
    plugin_cleanup(&plugins[i]);
  }
  catalog_cleanup(&catalog);
  charset_registry_cleanup(&charsets);
  webcam_cleanup(&cam);

  return 0;
}
