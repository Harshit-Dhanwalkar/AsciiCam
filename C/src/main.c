#include "nolibc.h"

#include "ascii.h"
#include "capture.h"
#include "plugins.h"
#include "thread_sharing.h"
#include "timing.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

// Defaults
#define DEFAULT_ASCII_WIDTH 80
#define DEFAULT_ASCII_HEIGHT 40
#define DEFAULT_CAPTURE_WIDTH 640
#define DEFAULT_CAPTURE_HEIGHT 480
#define DEFAULT_FPS 20
#define MAX_PLUGINS 8
#define DEFAULT_CHARSET_DIR "./charsets"

// Signal handling
volatile sig_atomic_t keep_running = 1;
void handle_signal(int sig) {
  (void)sig;
  keep_running = 0;
}

static struct termios orig_terminal;

static int my_atoi(const char *s) {
  int n = 0, neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  }
  while (*s >= '0' && *s <= '9')
    n = n * 10 + (*s++ - '0');
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
      "  -D            Floyd-Steinberg dithering                       \n"
      "  -P <0-100>    depth-pop 3D parallax strength (0=off)          \n"
      "\n"
      "Live keybindings:\n"
      "  m / M         cycle render mode forward / backward            \n"
      "  x / X         cycle edge detection mode forward / backward    \n"
      "  n / N         cycle loaded charset forward / backward         \n"
      "  p / o         increase / decrease depth-pop strength          \n"
      "  up/down       select plugin    [ ] +-1   { } +-10   r reset   \n"
      "  q             quit                                            \n",
      prog, DEFAULT_CAPTURE_WIDTH, DEFAULT_CAPTURE_HEIGHT, DEFAULT_FPS,
      DEFAULT_ASCII_WIDTH, DEFAULT_ASCII_HEIGHT, ASCII_CHARS_DEFAULT);
}

static render_mode_t parse_render_mode(const char *s) {
  if (nl_strcmp(s, "braille") == 0)
    return RENDER_BRAILLE;
  if (nl_strcmp(s, "blocks") == 0)
    return RENDER_BLOCKS;
  if (nl_strcmp(s, "ascii") == 0)
    return RENDER_ASCII_RAMP;
  if (nl_strcmp(s, "halfblock") == 0)
    return RENDER_HALF_BLOCK;
  if (nl_strcmp(s, "dots") == 0)
    return RENDER_DOTS;
  return RENDER_BRAILLE;
}

static edge_mode_t parse_edge_mode(const char *s) {
  if (nl_strcmp(s, "off") == 0)
    return EDGE_OFF;
  if (nl_strcmp(s, "sobel") == 0)
    return EDGE_SOBEL;
  if (nl_strcmp(s, "sobel-dir") == 0)
    return EDGE_SOBEL_DIR;
  if (nl_strcmp(s, "laplacian") == 0)
    return EDGE_LAPLACIAN;
  return EDGE_OFF;
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
                          const charset_registry_t *charsets) {
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
  if (n > 0 && n < (int)sizeof(buf))
    (void)write(STDOUT_FILENO, buf, (size_t)n);

  // Mode/edge/charset/depth-pop status row
  const char *cset_name = (charsets && charsets->count > 0 &&
                           opts->render_mode == RENDER_ASCII_RAMP)
                              ? charsets->sets[charsets->active].name
                              : "-";
  n = nl_snprintf(buf, sizeof(buf), "\033[%d;1H\033[K", base_row + 1);
  if (n > 0)
    (void)write(STDOUT_FILENO, buf, (size_t)n);
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
  if (n > 0 && n < (int)sizeof(buf))
    (void)write(STDOUT_FILENO, buf, (size_t)n);

  n = nl_snprintf(buf, sizeof(buf), "\033[%d;1H\033[K", base_row + 2);
  if (n > 0)
    (void)write(STDOUT_FILENO, buf, (size_t)n);

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
    if (n > 0 && n < (int)sizeof(buf))
      (void)write(STDOUT_FILENO, buf, (size_t)n);
  }
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
      .render_mode = RENDER_BRAILLE,
      .depth_pop = 0,
      .depth_invert = 0,
  };

  // Plugins
  const char *plugin_paths[MAX_PLUGINS];
  int plugin_path_count = 0;

  // CLI parsing
  int opt;
  while ((opt = nl_getopt(argc, argv, "d:W:H:w:h:f:b:c:iCDs:p:m:E:k:P:")) != -1)
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
    if (s > out_size)
      out_size = s;
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

    if (frame_diff_ns > 0)
      fps_push(&fps_calc, frame_diff_ns);
    last_frame_time = frame_start;
    double current_fps = fps_get(&fps_calc);

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
      }
    }
    if (!keep_running)
      break;

    // Hot-reload check for all plugins
    for (int i = 0; i < plugin_count; i++)
      plugin_check_reload(&plugins[i]);

    // Hot-reload check for charset ramps
    charset_registry_check_reload(&charsets);
    opts.charset = charset_registry_active_ramp(&charsets);

    // Frame capture
    if (webcam_wait_frame(&cam, 1000) < 0)
      continue; // timeout, retry

    if (webcam_capture_frame(&cam, gray) < 0) {
      perror("capture_frame");
      break;
    }

    // Run all plugins in order
    for (int i = 0; i < plugin_count; i++) {
      if (plugins[i].plugin)
        plugins[i].plugin->process(gray, cam.width, cam.height,
                                   &plugin_params[i]);
    }

    // NOTE: cam.buffer is the V4L2 mmap region (Linux only)
    // On macOS, capture_macos.c delivers luma only; cam.buffer is NULL
    // TODO: Add color support for macOS
    // Color mode is therefore a Linux-only feature for now.
    if (opts.color && rgb && cam.buffer && cam.buffer != MAP_FAILED)
      yuyv_to_rgb((const uint8_t *)cam.buffer, rgb, cam.width, cam.height);

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

    size_t out_size = ascii_out_size_for_mode(subpixel_w, subpixel_h,
                                              opts.color, opts.render_mode);
    char *out_buf = malloc(out_size);
    if (!out_buf) {
      perror("Failed to allocate text output buffer");
      break;
    }

    // Process frame mapping using dynamically calculated bounds
    int len = grayscale_to_ascii(gray, rgb, cam.width, cam.height, subpixel_w,
                                 subpixel_h, out_buf, out_size, &opts);
    if (len > (int)out_size) {
      fprintf(stderr, "WARNING: len=%d > out_size=%zu\n", len, out_size);
    }

    if (len > 0) {
      (void)write(STDOUT_FILENO, out_buf, (size_t)len);
      overlay_panel(ascii_h, current_fps, plugins, plugin_params, plugin_count,
                    selected, opts.color, &opts, &charsets);
    }

    free(out_buf);

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
  for (int i = 0; i < plugin_count; i++)
    plugin_cleanup(&plugins[i]);
  charset_registry_cleanup(&charsets);
  webcam_cleanup(&cam);
  return 0;
}
