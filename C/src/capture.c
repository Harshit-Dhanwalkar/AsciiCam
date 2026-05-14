#include "capture.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

int webcam_init(webcam_t *cam, const char *device, int width, int height) {
    // Open device (non‑blocking for select usage)
    cam->fd = open(device, O_RDWR | O_NONBLOCK);
    if (cam->fd < 0) return -1;

    // Set format
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        close(cam->fd);
        return -1;
    }

    // Read back actual resolution
    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;

    // Request one mmap buffer
    struct v4l2_requestbuffers req = {0};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        close(cam->fd);
        return -1;
    }

    // Query buffer info
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;

    if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
        close(cam->fd);
        return -1;
    }

    // mmap
    cam->buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, cam->fd, buf.m.offset);
    if (cam->buffer == MAP_FAILED) {
        close(cam->fd);
        return -1;
    }

    // Store buffer info for later munmap and requeue
    cam->buf_info = buf;

    // Queue the buffer
    if (ioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
        munmap(cam->buffer, buf.length);
        close(cam->fd);
        return -1;
    }

    // Start streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        munmap(cam->buffer, buf.length);
        close(cam->fd);
        return -1;
    }

    return 0;
}

int webcam_wait_frame(webcam_t *cam, int timeout_ms) {
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return -1;  // timeout or error
    return 0;
}

int webcam_capture_frame(webcam_t *cam, uint8_t *gray_buffer) {
    // Dequeue buffer
    struct v4l2_buffer buf = cam->buf_info;
    if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) return -1;

    // Convert YUYV -> grayscale (Y component)
    uint8_t *yuyv = (uint8_t *)cam->buffer;
    for (int i = 0, j = 0; i < cam->width * cam->height * 2; i += 2, j++) {
        gray_buffer[j] = yuyv[i];
    }

    // Store updated buffer info for requeue
    cam->buf_info = buf;
    return 0;
}

int webcam_requeue_buffer(webcam_t *cam) {
    if (ioctl(cam->fd, VIDIOC_QBUF, &cam->buf_info) < 0) return -1;
    return 0;
}

void webcam_cleanup(webcam_t *cam) {
    if (cam->fd >= 0) {
        // Stop streaming
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(cam->fd, VIDIOC_STREAMOFF, &type);
        // Unmap and close
        if (cam->buffer != MAP_FAILED)
            munmap(cam->buffer, cam->buf_info.length);
        close(cam->fd);
    }
    cam->fd = -1;
    cam->buffer = MAP_FAILED;
}
