#ifndef CAPTURE_H
#define CAPTURE_H

#include <stddef.h>
#include <stdint.h>

typedef struct webcam_impl webcam_impl_t;

typedef struct {
  int fd; // Linux: V4L2 fd. macOS: -1 (unused externally)
  int width;
  int height;
  void *buffer;
  webcam_impl_t *impl;
} webcam_t;

// Initialize webcam
// On Linux: device = "/dev/video0"
// On macOS: device = NULL (uses system default camera) or a device name string
int webcam_init(webcam_t *cam, const char *device, int width, int height);

// Wait for frame to be ready
int webcam_wait_frame(webcam_t *cam, int timeout_ms);

// Capture frame, dequeue buffer, fill grayscale output buffer
int webcam_capture_frame(webcam_t *cam, uint8_t *gray_buffer);

// Re‑queue buffer
int webcam_requeue_buffer(webcam_t *cam);

// Stop streaming and clean up resources
void webcam_cleanup(webcam_t *cam);

// Hardware camera controls.
int webcam_set_auto_exposure(webcam_t *cam, int enable);
int webcam_set_auto_white_balance(webcam_t *cam, int enable);

int webcam_adjust_exposure(webcam_t *cam, int delta, int *out_value);
int webcam_adjust_contrast(webcam_t *cam, int delta, int *out_value);
int webcam_adjust_white_balance(webcam_t *cam, int delta, int *out_value);

int webcam_get_exposure(webcam_t *cam, int *value);
int webcam_get_contrast(webcam_t *cam, int *value);
int webcam_get_white_balance(webcam_t *cam, int *value);

int webcam_get_exposure_range(webcam_t *cam, int *min, int *max);
int webcam_get_contrast_range(webcam_t *cam, int *min, int *max);
int webcam_get_white_balance_range(webcam_t *cam, int *min, int *max);

#endif
