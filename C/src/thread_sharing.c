/*
Still uses pthread functions, TODO: replace them with raw futex syscalls and
clone()
*/

#include "nolibc.h"

#include "ascii.h"
#include "capture.h"
#include "thread_sharing.h"

#include <stdint.h>

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

    // Capture grayscale into inactive frame buffer slot
    if (webcam_capture_frame(&cam, sf->gray_buf[write_idx]) < 0) {
      break;
    }
    webcam_requeue_buffer(&cam);

    // If color enabled, convert YUYV to RGB
    if (sf->opts.color && cam.buffer && cam.buffer != MAP_FAILED) {
      yuyv_to_rgb((const uint8_t *)cam.buffer, sf->rgb_buf[write_idx],
                  cam.width, cam.height);
    }

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

  int braille_w = sf->ascii_w * 2;
  int braille_h = sf->ascii_h * 4;
  size_t out_size = ascii_out_size(braille_w, braille_h, sf->opts.color);
  char *out_buf = nl_malloc(out_size);
  uint8_t *local_rgb = NULL;

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

    // Get RGB pointer if color is on
    uint8_t *rgb_ptr = sf->opts.color ? sf->rgb_buf[read_idx] : NULL;

    // Process frame outside locked state
    int len = grayscale_to_ascii(sf->gray_buf[read_idx], rgb_ptr, sf->width,
                                 sf->height, braille_w, braille_h, out_buf,
                                 out_size, &sf->opts);

    if (len > 0) {
      nl_write(STDOUT_FILENO, "\033[H", 3); // cursor to top-left
      nl_write(STDOUT_FILENO, out_buf, (size_t)len);
    }

    pthread_mutex_lock(&sf->lock);
  }

  pthread_mutex_unlock(&sf->lock);

  nl_free(out_buf);
  nl_free(local_rgb);

  return NULL;
}
