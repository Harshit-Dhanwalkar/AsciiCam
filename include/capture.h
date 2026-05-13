#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>
#include <linux/videodev2.h>

typedef struct {
    int fd;
    int width;
    int height;
    void *buffer;
    struct v4l2_buffer buf_info;
} webcam_t;

// Initialize webcam
int webcam_init(webcam_t *cam, const char *device, int width, int height);

// Wait for frame to be ready
int webcam_wait_frame(webcam_t *cam, int timeout_ms);

// Capture frame, dequeue buffer, fill grayscale output buffer
int webcam_capture_frame(webcam_t *cam, uint8_t *gray_buffer);

// Re‑queue buffer
int webcam_requeue_buffer(webcam_t *cam);

// Stop streaming and clean up resources
void webcam_cleanup(webcam_t *cam);

#endif
