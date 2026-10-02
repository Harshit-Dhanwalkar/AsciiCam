#ifndef CAPTURE_H
#define CAPTURE_H

#include "nl_types.h"

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
int webcam_wait_frame(const webcam_t *cam, int timeout_ms);

// Capture frame, dequeue buffer, fill grayscale output buffer
int webcam_capture_frame(webcam_t *cam, uint8_t *gray_buffer);

// Re‑queue buffer
int webcam_requeue_buffer(webcam_t *cam);

// Stop streaming and clean up resources
void webcam_cleanup(webcam_t *cam);

// Stop streaming and clean up resources
void webcam_cleanup(webcam_t *cam);

/* Reopen camera at (w, h) (rounded up to even) and reallocate pixel buffers for
 * size driver actually picked (cam->width/height); color selects whether *rgb
 * is allocated. Hardware control values are re-read.
 *
 * Returns 0 on success. On failure camera is left closed (fd == -1, safe for
 * webcam_cleanup) and *gray / *rgb are old buffers (open failed) or NULL
 * (allocation failed); either is valid input for a retry
 */
int capture_reinit(webcam_t *cam, const char *device, int w, int h,
                   uint8_t **gray, uint8_t **rgb, int color, int *hw_exposure,
                   int *hw_contrast, int *hw_wb);

// Hardware camera controls
int webcam_set_auto_exposure(const webcam_t *cam, int enable);
int webcam_set_auto_white_balance(const webcam_t *cam, int enable);

int webcam_adjust_exposure(const webcam_t *cam, int delta, int *out_value);
int webcam_adjust_contrast(const webcam_t *cam, int delta, int *out_value);
int webcam_adjust_white_balance(const webcam_t *cam, int delta, int *out_value);

int webcam_get_exposure(const webcam_t *cam, int *value);
int webcam_get_contrast(const webcam_t *cam, int *value);
int webcam_get_white_balance(const webcam_t *cam, int *value);

int webcam_get_exposure_range(const webcam_t *cam, int *min, int *max);
int webcam_get_contrast_range(const webcam_t *cam, int *min, int *max);
int webcam_get_white_balance_range(const webcam_t *cam, int *min, int *max);

#endif
