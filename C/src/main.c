#include "nolibc.h"

#include "ascii.h"
#include "capture.h"
#include "plugins.h"
#include "thread_sharing.h"
#include "timing.h"

// Defaults
#define DEFAULT_ASCII_WIDTH 80
#define DEFAULT_ASCII_HEIGHT 40
#define DEFAULT_CAPTURE_WIDTH 640
#define DEFAULT_CAPTURE_HEIGHT 480
#define DEFAULT_FPS 20
#define MAX_PLUGINS 8

#define DEFAULT_CHARSET_DIR "./charsets"
#define DEFAULT_CONFIG_PATH ".asciicamrc"
#define CONFIG_MAX_BYTES 4096

#define PANEL_ROWS 4
#define MIN_ASCII_W 10
#define MIN_ASCII_H 5

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
void handle_signal(int sig) {
  (void)sig;

  keep_running = 0;
}

// SIGWINCH: terminal resized
volatile sig_atomic_t term_resized = 0;
volatile sig_atomic_t winch_count = 0;
void handle_winch(int sig) {
  (void)sig;

  term_resized = 1;
  winch_count++;
}

static struct termios orig_terminal;

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
      "  -p <path>     filter plugin .so path                          \n"
      "  -m <mode>     render mode: braille|blocks|ascii|halfblock|dots\n"
      "  -k <dir>      charset directory (hot-reloadable .txt ramps)   \n"
      "\n"
      "Image adjustments:\n"
      "  -b <val>      brightness offset        -128..128  (default: 0)\n"
      "  -c <val>      contrast in percent      >0; 100=none (default: 100)\n"
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
      "  e / E         hw exposure down / up        (V4L2, Linux only) \n"
      "  w / W         hw white-balance down / up   (V4L2, Linux only) \n"
      "  c / C         hw contrast down / up        (V4L2, Linux only) \n"
      "  up/down       select plugin    [ ] +-1   { } +-10   r reset   \n"
      "  q             quit                                            \n",
      prog, DEFAULT_CAPTURE_WIDTH, DEFAULT_CAPTURE_HEIGHT, DEFAULT_FPS,
      DEFAULT_ASCII_WIDTH, DEFAULT_ASCII_HEIGHT, ASCII_CHARS_DEFAULT);
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
        char *val = _cfg_skip_ws(eq + 1);
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
}

void term_restore(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_terminal); }

static void overlay_panel(int ascii_h, double fps, plugin_loader_t *plugins,
                          int *plugin_params, int count, int selected,
                          int color, const ascii_opts_t *opts,
                          const charset_registry_t *charsets, int hw_exposure,
                          int hw_contrast, int hw_wb) {
  char buf[1024];
  int n, base_row = ascii_h + 1; // 1-indexed panel row

  // FPS + hint bar
  char fpsbuf[10];
  nl_fmt_fps(fpsbuf, sizeof(fpsbuf), fps);
  if (color) {
    n = nl_snprintf(buf, sizeof(buf),
                    "\033[%d;1H\033[38;2;0;220;0m\033[48;2;18;18;18m"
                    " FPS: %s  │  ↑↓ select  [ ] ±1  { } ±10  r reset  q quit "
                    "\033[0m\033[K",
                    base_row, fpsbuf);
  } else {
    n = nl_snprintf(buf, sizeof(buf),
                    "\033[%d;1H FPS: %s  |  up/dn select  [ ] +-1  { } +-10  r "
                    "reset  q quit\033[K",
                    base_row, fpsbuf);
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
    n = nl_snprintf(buf, sizeof(buf),
                    "\033[38;2;0;180;220m mode: %s (m/M)  edges: %s (x/X)  "
                    "charset: %s (n/N)  depth-pop: %d%s (+/-, v)\033[0m\033[K",
                    render_mode_name(opts->render_mode),
                    edge_mode_name(opts->edges), cset_name, opts->depth_pop,
                    opts->depth_invert ? " [inv]" : "");
  } else {
    n = nl_snprintf(buf, sizeof(buf),
                    " mode: %s  edges: %s  charset: %s  depth-pop: %d%s\033[K",
                    render_mode_name(opts->render_mode),
                    edge_mode_name(opts->edges), cset_name, opts->depth_pop,
                    opts->depth_invert ? " [inv]" : "");
  }

  if (n > 0 && n < (int)sizeof(buf)) {
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }

  // Hardware (V4L2) camera control row -- "n/a" fields when unsupported
  // (macOS/Windows, or a driver that doesn't expose that control).
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

    (void)write(STDOUT_FILENO, msg, strlen(msg));

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

// Returns >0 if the ASCII dimensions changed, 0 otherwise.
// Reallocates *out_buf and updates *out_size.
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

  int new_w = ws.ws_col;
  int new_h = ws.ws_row - PANEL_ROWS; // reserve rows for overlay panel
  if (new_h < MIN_ASCII_H) {
    new_h = MIN_ASCII_H;
  }
  if (new_w < MIN_ASCII_W) {
    new_w = MIN_ASCII_W;
  }
  if (new_w == *ascii_w && new_h == *ascii_h) {
    return 0;
  }

  *ascii_w = new_w;
  *ascii_h = new_h;

  // Recompute output buffer size for the new dimensions (max over all modes)
  size_t need = 0;
  for (render_mode_t rm = 0; rm < RENDER_MODE_COUNT; rm++) {
    size_t s = ascii_out_size_for_mode(new_w * 2, new_h * 4, color, rm);
    if (s > need) {
      need = s;
    }
  }

  char dbg[96];
  int dn = nl_snprintf(dbg, sizeof(dbg), "[resize] ws_row=%u ws_col=%u\n",
                       (unsigned)ws.ws_row, (unsigned)ws.ws_col);
  if (dn > 0) {
    write(2, dbg, (size_t)dn);
  }

  char *nb = malloc(need);
  if (!nb) {
    return 0; // keep old buffer, dimensions updated
  }

  free(*out_buf);
  *out_buf = nb;
  *out_size = need;

  return 1;
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
  while ((opt = nl_getopt(argc, argv, "d:W:H:w:h:f:b:c:iCD2s:p:m:E:k:P:")) !=
         -1)
    switch (opt) {
    case 'd':
      device = optarg;
      break;
    case 'W':
      ascii_w = my_atoi(optarg);
      if (ascii_w <= 0)
        ascii_w = DEFAULT_ASCII_WIDTH;
      break;
    case 'H':
      ascii_h = my_atoi(optarg);
      if (ascii_h <= 0)
        ascii_h = DEFAULT_ASCII_HEIGHT;
      break;
    case 'w':
      cap_w = my_atoi(optarg);
      if (cap_w <= 0)
        cap_w = DEFAULT_CAPTURE_WIDTH;
      break;
    case 'h':
      cap_h = my_atoi(optarg);
      if (cap_h <= 0)
        cap_h = DEFAULT_CAPTURE_HEIGHT;
      break;
    case 'f':
      fps = my_atoi(optarg);
      if (fps <= 0)
        fps = DEFAULT_FPS;
      break;
    case 'b':
      opts.brightness = my_atoi(optarg);
      break;
    case 'c':
      opts.contrast = my_atoi(optarg);
      if (opts.contrast <= 0)
        opts.contrast = 100;
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
      if (opts.depth_pop < 0)
        opts.depth_pop = 0;
      if (opts.depth_pop > 100)
        opts.depth_pop = 100;
      break;
    case 'D':
      opts.dither = 1;
      break;
    case 's':
      opts.charset = optarg;
      break;
    case 'p':
      if (plugin_path_count < MAX_PLUGINS)
        plugin_paths[plugin_path_count++] = optarg;
      else
        fprintf(stderr, "Warning: max %d plugins, ignoring %s\n", MAX_PLUGINS,
                optarg);
      break;
    default:
      print_usage(argv[0]);

      return 1;
    }

  timing_init(fps);

  // Initialize plugins
  plugin_loader_t plugins[MAX_PLUGINS];
  int plugin_params[MAX_PLUGINS];
  int plugin_count = 0;

  for (int i = 0; i < plugin_path_count; i++) {
    memset(&plugins[i], 0, sizeof(plugin_loader_t));
    plugins[i].inotify_fd = -1;
    plugin_params[i] = 128; // default

    if (plugin_load(&plugins[i], plugin_paths[i]) == 0) {
      plugin_watch_init(&plugins[i], plugin_paths[i]);
      plugin_count++;
    } else {
      fprintf(stderr, "Failed to load plugin: %s\n", plugin_paths[i]);
    }
  }

  int selected = 0;

  // Open webcam
  webcam_t cam = {.fd = -1, .buffer = MAP_FAILED};
  if (webcam_init(&cam, device, cap_w, cap_h) < 0) {
    perror("webcam_init");

    return 1;
  }

  fprintf(stderr,
          "Device: %s | capture %dx%d | ASCII %dx%d | %d fps | %d "
          "plugin(s) | mode: %s%s%s%s\n",
          device, cam.width, cam.height, ascii_w, ascii_h, fps, plugin_count,
          render_mode_name(opts.render_mode), opts.color ? " | color" : "",
          opts.edges != EDGE_OFF ? " | edges" : "",
          opts.dither ? " | dither" : "");

  int hw_exposure = -1;
  int hw_contrast = -1;
  int hw_wb = -1;
  webcam_get_exposure(&cam, &hw_exposure);
  webcam_get_contrast(&cam, &hw_contrast);
  webcam_get_white_balance(&cam, &hw_wb);

  // Pixel buffers allocation
  int cam_pixels = cam.width * cam.height;
  uint8_t *gray = malloc(cam_pixels);
  uint8_t *rgb = opts.color ? malloc(cam_pixels * 3) : NULL;

  if (!gray || (opts.color && !rgb)) {
    perror("malloc pixel buffers");
    free(gray);
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
    perror("malloc out_buf");
    free(gray);
    free(rgb);
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
    if (term_resized) {
      term_resized = 0;
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

    {
      static sig_atomic_t last = 0;
      if (winch_count != last) {
        last = winch_count;
        char b[64];
        int n = nl_snprintf(b, sizeof(b), "[winch #%d]\n", (int)winch_count);
        if (n > 0) {
          write(2, b, (size_t)n);
        }
      }
    }

    // Keypress handling
    char ch;
    while (read(STDIN_FILENO, &ch, 1) == 1) {
      if (ch == '\033') {
        char seq[2] = {0, 0};
        if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
          if (read(STDIN_FILENO, &seq[1], 1) == 1) {
            switch (seq[1]) {
            case 'A': // up arrow key, previous plugin
              if (plugin_count > 0)
                selected = (selected - 1 + plugin_count) % plugin_count;
              break;
            case 'B': // down arrow key, next plugin
              if (plugin_count > 0)
                selected = (selected + 1) % plugin_count;
              break;
            }
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
        if (pp && *pp < 255)
          (*pp)++;
        break;
      case '[':
        if (pp && *pp > 0)
          (*pp)--;
        break;
      case '}':
        if (pp)
          *pp = (*pp + 10 > 255) ? 255 : *pp + 10;
        break;
      case '{':
        if (pp)
          *pp = (*pp - 10 < 0) ? 0 : *pp - 10;
        break;
      case 'r':
      case 'R':
        if (pp)
          *pp = 128;
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
      case '+':
        opts.depth_pop = (opts.depth_pop + 5 > 100) ? 100 : opts.depth_pop + 5;
        break;
      case '-':
        opts.depth_pop = (opts.depth_pop - 5 < 0) ? 0 : opts.depth_pop - 5;
        break;
      case 'v':
        opts.depth_invert = !opts.depth_invert;
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
      perror("capture_frame");
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
    // Color mode is therefore a Linux-only feature for now.
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
                    hw_contrast, hw_wb);
    }

    if (webcam_requeue_buffer(&cam) < 0) {
      perror("requeue_buffer");
      break;
    }

    timing_sleep(&frame_start);
  }

  // Cleanup
  term_restore();
  // \033[2J = erase screen, \033[H = cursor home, \033[?25h = show cursor
  static const char CLEANUP[] = "\033[2J\033[H\033[0m\033[?25h";
  (void)write(STDOUT_FILENO, CLEANUP, sizeof(CLEANUP) - 1);

  fprintf(stderr, "Stopped.\n");

  free(gray);
  free(rgb);
  free(out_buf);
  for (int i = 0; i < plugin_count; i++) {
    plugin_cleanup(&plugins[i]);
  }
  charset_registry_cleanup(&charsets);
  webcam_cleanup(&cam);

  return 0;
}
