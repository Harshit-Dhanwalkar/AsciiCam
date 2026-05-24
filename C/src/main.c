#include "ascii.h"
#include "capture.h"
#include "thread_sharing.h"
#include "timing.h"

#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// Defaults
#define DEFAULT_ASCII_WIDTH    80
#define DEFAULT_ASCII_HEIGHT   40
#define DEFAULT_CAPTURE_WIDTH  160
#define DEFAULT_CAPTURE_HEIGHT 120
#define DEFAULT_FPS            20

// Signal handling
volatile sig_atomic_t keep_running = 1;
void handle_signal(int sig) {
  (void)sig;
  keep_running = 0;
}

static struct termios orig_terminal;

// Usage
static void print_usage(const char *prog) {
  fprintf(
      stderr,
      "Usage: %s [options]\n"
      "\n"
      "Capture options:\n"
      "  -d <device>   video device            (default: /dev/video0)\n"
      "  -w <width>    capture width            (default: %d)\n"
      "  -h <height>   capture height           (default: %d)\n"
      "  -f <fps>      target framerate         (default: %d)\n"
      "\n"
      "Output options:\n"
      "  -W <width>    ASCII output columns     (default: %d)\n"
      "  -H <height>   ASCII output rows        (default: %d)\n"
      "  -s <chars>    custom charset string    (default: \"%s\")\n"
      "\n"
      "Image adjustments:\n"
      "  -b <val>      brightness offset        -128..128  (default: 0)\n"
      "  -c <val>      contrast in percent      >0; 100=none (default: 100)\n"
      "  -i            invert brightness->charset mapping\n"
      "  -e            enable Sobel edge detection\n"
      "  -C            colour output (ANSI truecolor)\n"
      "  -D            Floyd-Steinberg dithering\n",
      prog, DEFAULT_CAPTURE_WIDTH, DEFAULT_CAPTURE_HEIGHT, DEFAULT_FPS,
      DEFAULT_ASCII_WIDTH, DEFAULT_ASCII_HEIGHT, ASCII_CHARS_DEFAULT); // FIX: undeclared identifier ASCII_CHARS_DEFAULT
}

// termios
void term_raw_mode(void) {
  tcgetattr(STDOUT_FILENO, &orig_terminal); // save stdin state
  struct termios raw = orig_terminal;
  raw.c_lflag &= ~(ICANON | ECHO);          // no line buffering or no echo
  raw.c_cc[VMIN]  = 0;                      // non-blocking read
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void term_restore(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_terminal); }

fps_counter_t fps_calc = {0};              // Global FPS counter // FIX:  

// Main
int main(int argc, char *argv[]) {
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  // Config
  char *device = "/dev/video0";
  int ascii_w  = DEFAULT_ASCII_WIDTH;
  int ascii_h  = DEFAULT_ASCII_HEIGHT;
  int cap_w    = DEFAULT_CAPTURE_WIDTH;
  int cap_h    = DEFAULT_CAPTURE_HEIGHT;
  int fps      = DEFAULT_FPS;

  ascii_opts_t opts = { // FIX: undcleared identifier 'ascii_opts_t'
      .brightness = 0,
      .contrast = 100,
      .invert = 0,
      .color = 0,
      .edges = 0,
      .dither = 0,
      .charset = NULL,
  };

  // CLI parsing
  int opt;
  while ((opt = getopt(argc, argv, "ed:W:H:w:h:f:b:c:iCDs:")) != -1) {
    switch (opt) {
    case 'd':
      device = optarg;
      break;
    case 'W':
      ascii_w = atoi(optarg);
      if (ascii_w <= 0)
        ascii_w = DEFAULT_ASCII_WIDTH;
      break;
    case 'H':
      ascii_h = atoi(optarg);
      if (ascii_h <= 0)
        ascii_h = DEFAULT_ASCII_HEIGHT;
      break;
    case 'w':
      cap_w = atoi(optarg);
      if (cap_w <= 0)
        cap_w = DEFAULT_CAPTURE_WIDTH;
      break;
    case 'h':
      cap_h = atoi(optarg);
      if (cap_h <= 0)
        cap_h = DEFAULT_CAPTURE_HEIGHT;
      break;
    case 'f':
      fps = atoi(optarg);
      if (fps <= 0)
        fps = DEFAULT_FPS;
      break;
    case 'b':
      opts.brightness = atoi(optarg);
      break;
    case 'c':
      opts.contrast = atoi(optarg);
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
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  timing_init(fps);

  // Open webcam
  webcam_t cam = {.fd = -1, .buffer = MAP_FAILED};
  if (webcam_init(&cam, device, cap_w, cap_h) < 0) {
    perror("webcam_init");
    return 1;
  }
  fprintf(stderr, "Device: %s | capture %dx%d | ASCII %dx%d | %d fps%s%s%s%s\n",
          device, cam.width, cam.height, ascii_w, ascii_h, fps,
          opts.color  ? " | color"   : "",
          opts.edges  ? " | edges"   : "",
          opts.dither ? " | dither"  : "",
          opts.invert ? " | inverted": "");

  // Allocate pixel buffers
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

  // Initial full clear
  write(STDOUT_FILENO, "\033[2J\033[H\033[?25l", 13);
  term_raw_mode();

  // Main loop
  struct timespec frame_start;
  clock_gettime(CLOCK_MONOTONIC, &frame_start);
  struct timespec last_frame_time = frame_start;
  char input_char;

  while (keep_running) {
    clock_gettime(CLOCK_MONOTONIC, &frame_start);

    long frame_diff_ns =
        (frame_start.tv_sec - last_frame_time.tv_sec) * 1000000000L +  // Seconds
        (frame_start.tv_nsec - last_frame_time.tv_nsec);               // Nano seconds

    if (frame_diff_ns > 0)
      fps_push(&fps_calc, frame_diff_ns);
    last_frame_time = frame_start;
    double current_fps = fps_get(&fps_calc);

    // Check for 'q' key non-blocking
    if (read(STDIN_FILENO, &input_char, 1) == 1) {
      if (input_char == 'q' || input_char == 'Q') {
        keep_running = 0;
        break;
      }
    }

    if (webcam_wait_frame(&cam, 1000) < 0)
      continue; // timeout, retry

    if (webcam_capture_frame(&cam, gray) < 0) {
      perror("capture_frame");
      break;
    }

    if (opts.color && rgb)
      yuyv_to_rgb((const uint8_t *)cam.buffer, rgb, cam.width, cam.height);

    int len = grayscale_to_ascii(gray, rgb, cam.width, cam.height, ascii_w,
                                 ascii_h, out_buf, out_size, &opts);

    if (len > 0) {
      write(STDOUT_FILENO, out_buf, (size_t)len);
      overlay_fps_box(ascii_w, current_fps, opts.color);
    }

    if (webcam_requeue_buffer(&cam) < 0) {
      perror("requeue_buffer");
      break;
    }

    timing_sleep(&frame_start);
  }

  // Cleanup
  term_restore();
  write(STDOUT_FILENO, "\033[0m\033[?25h\n", 11);
  fprintf(stderr, "Stopped.\n");

  free(gray);
  free(rgb);
  free(out_buf);
  webcam_cleanup(&cam);
  return 0;
}
