/*
Still uses pthread functions, TODO: replace them with raw futex syscalls and clone()
*/


#include "ascii.h"
#include "capture.h"
#include "thread_sharing.h"

#include <stdint.h>

#include "nolibc.h"

// Producer thread for capturing frames
void *capture_thread(void *arg) {
  shared_frame_t *sf = arg;
  webcam_t cam = {.fd = -1, .buffer = MAP_FAILED};

  // Intialialize webcam into the thread context
  if (webcam_init(&cam, "/dev/video0", sf->width, sf->height) < 0) {
    perror("webcam_init in capture thread");
    sf->stop = 1;
    return NULL;
  }

  int write_idx = 0;

  while (!sf->stop) {
    if (webcam_wait_frame(&cam, 100) < 0) {
      continue;
    }

    // Capture into inactive frame buffer slot
    if (webcam_capture_frame(&cam, sf->buf[write_idx]) < 0) {
      break;
    }
    webcam_requeue_buffer(&cam);

    pthread_mutex_lock(&sf->lock);
    sf->ready_idx = write_idx;
    sf->has_frame = 1;
    pthread_cond_signal(&sf->cond); // wake render thread
    pthread_mutex_unlock(&sf->lock);

    write_idx ^= 1; // XOR, swap buffers
  }

  webcam_cleanup(&cam);
  return NULL;
}

// Producer thread for rendering
void *render_thread(void *arg) {
  shared_frame_t *sf = arg;

  // Allocate dynamic buffers
  size_t out_size = ascii_out_size(sf->ascii_w, sf->ascii_h, sf->opts.color);
  char *out_buf = malloc(out_size);
  uint8_t *local_rgb =
      sf->opts.color ? malloc(sf->width * sf->height * 3) : NULL;

  if (!out_buf) {
    perror("render_thread malloc");
    return NULL;
  }

  pthread_mutex_lock(&sf->lock);
  while (!sf->stop) {
    while (!sf->has_frame && !sf->stop) {
      pthread_cond_wait(&sf->cond, &sf->lock); // sleep
    }
    if (sf->stop)
      break;

    int read_idx = sf->ready_idx;
    sf->has_frame = 0;

    pthread_mutex_unlock(&sf->lock);

    // Process frame outside locked state
    int len = grayscale_to_ascii(sf->buf[read_idx], local_rgb, sf->width,
                                 sf->height, sf->ascii_w, sf->ascii_h, out_buf,
                                 out_size, &sf->opts);

    if (len > 0) {
      write(STDOUT_FILENO, "\033[H", 3); // cursor to top-left
      write(STDOUT_FILENO, out_buf, (size_t)len);
    }

    pthread_mutex_lock(&sf->lock);
  }

  pthread_mutex_unlock(&sf->lock);

  // Cleanup memory
  free(out_buf);
  if (local_rgb) {
    free(local_rgb);
  }

  return NULL;
}
