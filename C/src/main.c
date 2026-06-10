#include "ascii.h"
#include "capture.h"
#include "plugins.h"
#include "thread_sharing.h"
#include "timing.h"

#include "nolibc.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

// Defaults
#define DEFAULT_ASCII_WIDTH 80
#define DEFAULT_ASCII_HEIGHT 40
#define DEFAULT_CAPTURE_WIDTH 160
#define DEFAULT_CAPTURE_HEIGHT 120
#define DEFAULT_FPS 20
#define MAX_PLUGINS 8

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
      "\n"
      "Image adjustments:\n"
      "  -b <val>      brightness offset        -128..128  (default: 0)\n"
      "  -c <val>      contrast in percent      >0; 100=none (default: 100)\n"
      "  -i            invert mapping                                  \n"
      "  -e            enable Sobel edge detection                     \n"
      "  -C            ANSI truecolor output                           \n"
      "  -D            Floyd-Steinberg dithering                       \n",
      prog, DEFAULT_CAPTURE_WIDTH, DEFAULT_CAPTURE_HEIGHT, DEFAULT_FPS,
      DEFAULT_ASCII_WIDTH, DEFAULT_ASCII_HEIGHT, ASCII_CHARS_DEFAULT);
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
                          int color) {
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

  n = nl_snprintf(buf, sizeof(buf), "\033[%d;1H\033[K", base_row + 1);
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

  ascii_opts_t opts = {
      .brightness = 0,
      .contrast = 100,
      .invert = 0,
      .color = 0,
      .edges = 0,
      .dither = 0,
      .charset = NULL,
  };

  // Plugins
  const char *plugin_paths[MAX_PLUGINS];
  int plugin_path_count = 0;

  // CLI parsing
  int opt;
  while ((opt = nl_getopt(argc, argv, "d:W:H:w:h:f:b:c:iCDes:p:")) != -1)
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
    case 'e':
      opts.edges = 1;
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
          "plugin(s)%s%s%s%s\n",
          device, cam.width, cam.height, ascii_w, ascii_h, fps, plugin_count,
          opts.color ? " | color" : "", opts.edges ? " | edges" : "",
          opts.dither ? " | dither" : "", opts.invert ? " | inverted" : "");

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
  size_t out_size = ascii_out_size(ascii_w, ascii_h, opts.color);
  char *out_buf = malloc(out_size);

  if (!out_buf) {
    perror("malloc out_buf");
    free(gray);
    free(rgb);
    webcam_cleanup(&cam);
    return 1;
  }

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

      // adjust plugin param if selected
      int *p = (plugin_count > 0) ? &plugin_params[selected] : NULL;
      switch (ch) {
      case 'q':
      case 'Q':
        keep_running = 0;
        break;
      case ']':
        if (p && *p < 255)
          (*p)++;
        break;
      case '[':
        if (p && *p > 0)
          (*p)--;
        break;
      case '}':
        if (p)
          *p = (*p + 10 > 255) ? 255 : *p + 10;
        break;
      case '{':
        if (p)
          *p = (*p - 10 < 0) ? 0 : *p - 10;
        break;
      case 'r':
      case 'R':
        if (p)
          *p = 128;
        break;
      }
    }
    if (!keep_running)
      break;

    // Hot-reload check for all plugins
    for (int i = 0; i < plugin_count; i++)
      plugin_check_reload(&plugins[i]);

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

    if (opts.color && rgb)
      yuyv_to_rgb((const uint8_t *)cam.buffer, rgb, cam.width, cam.height);

    int len = grayscale_to_ascii(gray, rgb, cam.width, cam.height, ascii_w,
                                 ascii_h, out_buf, out_size, &opts);

    if (len > 0) {
      (void)write(STDOUT_FILENO, out_buf, (size_t)len);
      overlay_panel(ascii_h, current_fps, plugins, plugin_params, plugin_count,
                    selected, opts.color);
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
  static const char CLEANUP[] = "\033[2J\033[H\033[0m\033[?25h\n";
  (void)write(STDOUT_FILENO, CLEANUP, sizeof(CLEANUP) - 1);
  fprintf(stderr, "Stopped.\n");

  free(gray);
  free(rgb);
  free(out_buf);
  for (int i = 0; i < plugin_count; i++)
    plugin_cleanup(&plugins[i]);
  webcam_cleanup(&cam);
  return 0;
}
