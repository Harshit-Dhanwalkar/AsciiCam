#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Default configuration values
#define DEFAULT_ASCII_WIDTH 80
#define DEFAULT_ASCII_HEIGHT 40
#define DEFAULT_CAPTURE_WIDTH 160
#define DEFAULT_CAPTURE_HEIGHT 120
#define DEFAULT_FPS 20

#define ASCII_CHARS " .:-=+*#%@"
#define CLEAR_SCREEN "\033[2J\033[H"

volatile sig_atomic_t keep_running = 1;

void handle_signal(int sig) { keep_running = 0; }

void print_usage(char *prog_name) {
  fprintf(stderr,
          "Usage: %s [-d <device>] [-W <width>] [-H <height>] [-f <fps>]\n",
          prog_name);
  fprintf(stderr, "  -d <device> : Video device path (default: /dev/video0)\n");
  fprintf(stderr, "  -W <width>  : ASCII output width (default: %d)\n",
          DEFAULT_ASCII_WIDTH);
  fprintf(stderr, "  -H <height> : ASCII output height (default: %d)\n",
          DEFAULT_ASCII_HEIGHT);
  fprintf(stderr, "  -f <fps>    : Target framerate (default: %d)\n",
          DEFAULT_FPS);
}

int main(int argc, char *argv[]) {
  signal(SIGINT, handle_signal);

  // Configuration Variables
  char *device_path = "/dev/video0";
  int ascii_width = DEFAULT_ASCII_WIDTH;
  int ascii_height = DEFAULT_ASCII_HEIGHT;
  int target_fps = DEFAULT_FPS;
  long frame_duration_ns = 1000000000L / target_fps;
  int capture_width = DEFAULT_CAPTURE_WIDTH;
  int capture_height = DEFAULT_CAPTURE_HEIGHT;

  // Resource Tracking for Cleanup
  int fd = -1;
  void *buffer = MAP_FAILED;
  unsigned char *gray_buffer = NULL;
  struct v4l2_buffer buf = {0};
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  // Command-Line Argument Parsing
  int c;
  while ((c = getopt(argc, argv, "d:W:H:f:")) != -1) {
    switch (c) {
    case 'd':
      device_path = optarg;
      break;
    case 'W':
      ascii_width = atoi(optarg);
      if (ascii_width <= 0)
        ascii_width = DEFAULT_ASCII_WIDTH;
      break;
    case 'H':
      ascii_height = atoi(optarg);
      if (ascii_height <= 0)
        ascii_height = DEFAULT_ASCII_HEIGHT;
      break;
    case 'f':
      target_fps = atoi(optarg);
      if (target_fps <= 0)
        target_fps = DEFAULT_FPS;
      frame_duration_ns = 1000000000L / target_fps;
      break;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  // V4L2 Setup
  fd = open(device_path, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    perror("Error opening video device");
    goto cleanup;
  }
  printf("Webcam opened successfully: %s\n", device_path);

  // Check capabilities (omitted for brevity, assume capture capability)

  // Set format
  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = capture_width;
  fmt.fmt.pix.height = capture_height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    perror("Setting format failed");
    goto cleanup;
  }

  // Format Negotiation (Read back the actual accepted size)
  capture_width = fmt.fmt.pix.width;
  capture_height = fmt.fmt.pix.height;
  printf("Capture resolution set to: %dx%d\n", capture_width, capture_height);

  // Request buffers
  struct v4l2_requestbuffers req = {0};
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
    perror("Requesting buffers failed");
    goto cleanup;
  }

  // Map the buffer
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = 0;

  if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
    perror("Querying buffer failed");
    goto cleanup;
  }

  buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                buf.m.offset);
  if (buffer == MAP_FAILED) {
    perror("Memory mapping failed");
    goto cleanup;
  }

  // Queue the buffer
  if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
    perror("Queue buffer failed");
    goto cleanup;
  }

  // Start streaming
  if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
    perror("Start streaming failed");
    goto cleanup;
  }

  printf("Starting ASCII webcam stream... Press Ctrl+C to stop\n");

  // Create buffer for grayscale conversion
  gray_buffer = malloc(capture_width * capture_height);
  if (gray_buffer == NULL) {
    perror("Memory allocation failed for gray_buffer");
    goto cleanup;
  }

  // Main Streaming Loop
  struct timespec start_time, end_time;

  while (keep_running) {
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    fd_set fds;
    struct timeval tv = {0};

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = 1; // Shorter select timeout

    int r = select(fd + 1, &fds, NULL, NULL, &tv);
    if (r < 0) {
      if (errno == EINTR)
        continue; // Handle interrupted select
      perror("Select failed");
      break;
    }

    if (r == 0) {
      fprintf(stderr, "Select timeout on frame availability\n");
      continue;
    }

    // Dequeue buffer
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
      perror("Dequeue buffer failed");
      break;
    }

    // Convert YUYV to grayscale (Y component only)
    uint8_t *yuyv_data = (uint8_t *)buffer;
    for (int i = 0, j = 0; i < capture_width * capture_height * 2;
         i += 2, j++) {
      gray_buffer[j] = yuyv_data[i];
    }

    // Clear screen and move cursor to top-left
    printf(CLEAR_SCREEN);

    // Averaging Resampling and ASCII Conversion
    for (int y = 0; y < ascii_height; y++) {
      for (int x = 0; x < ascii_width; x++) {
        double block_width = (double)capture_width / ascii_width;
        double block_height = (double)capture_height / ascii_height;
        long long total_brightness = 0;
        int pixel_count = 0;

        // Iterate over the source block that corresponds to one ASCII character
        for (int src_y = (int)(y * block_height);
             src_y < (int)((y + 1) * block_height); src_y++) {
          for (int src_x = (int)(x * block_width);
               src_x < (int)((x + 1) * block_width); src_x++) {

            // Bounds check
            if (src_x < capture_width && src_y < capture_height) {
              int idx = src_y * capture_width + src_x;
              total_brightness += gray_buffer[idx];
              pixel_count++;
            }
          }
        }

        unsigned char avg_pixel = 0;
        if (pixel_count > 0) {
          avg_pixel = total_brightness / pixel_count;
        }

        // Map brightness (0-255) to ASCII character set
        int ascii_len = strlen(ASCII_CHARS);
        int ascii_idx = avg_pixel * (ascii_len - 1) / 255;
        putchar(ASCII_CHARS[ascii_idx]);
      }
      putchar('\n');
    }
    fflush(stdout);

    // Re-queue buffer
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
      perror("Re-queue buffer failed");
      break;
    }

    // Precise Frame Rate Control
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    long elapsed_ns = (end_time.tv_sec - start_time.tv_sec) * 1000000000L +
                      (end_time.tv_nsec - start_time.tv_nsec);
    long sleep_ns = frame_duration_ns - elapsed_ns;

    if (sleep_ns > 0) {
      struct timespec ts = {0, 0};
      ts.tv_sec = sleep_ns / 1000000000L;
      ts.tv_nsec = sleep_ns % 1000000000L;
      // Use nanosleep with loop to handle interruptions
      while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
    }
  }

  printf("\nStopping...\n");
  // Stop streaming
  ioctl(fd, VIDIOC_STREAMOFF, &type);

// Centralized Cleanup
cleanup:
  if (gray_buffer != NULL) {
    free(gray_buffer);
  }
  if (buffer != MAP_FAILED) {
    // buf.length is still valid from the initial VIDIOC_QUERYBUF call
    munmap(buffer, buf.length);
  }
  if (fd >= 0) {
    close(fd);
  }

  return (fd < 0 || buffer == MAP_FAILED || gray_buffer == NULL) ? 1 : 0;
}
