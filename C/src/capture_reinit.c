#include "nolibc.h"

#include "capture.h"

// Reopens camera at (w, h) and reallocates pixel buffers for size
// driver actually picked (cam->width/height)
//
// Returns 0 on success. On failure camera is left closed (fd = -1, safe to pass
// to webcam_cleanup) and *gray / *rgb are either still old buffers (camera
// could not be opened) or NULL (new buffers could not be  allocated); both
// states are valid input for another capture_reinit, which is how caller
// restores previous size
//
// Old buffers are freed before new ones are allocated: allocator's small-block
// arena is only 2 MiB, so holding old and new frames at once (e.g. 640x480 ->
// 1280x720 with colour) exhausts it and every resize fails
int capture_reinit(webcam_t *cam, const char *device, int w, int h,
                   uint8_t **gray, uint8_t **rgb, int color, int *hw_exposure,
                   int *hw_contrast, int *hw_wb) {
  w = (w + 1) & ~1;
  h = (h + 1) & ~1;

  webcam_cleanup(cam);
  *cam = (webcam_t){.fd = -1, .buffer = MAP_FAILED};

  // USB UVC needs a moment between STREAMOFF and re-open
  nl_usleep(120000);

  if (webcam_init(cam, device, w, h) < 0) {
    // a failed init may leave a stale fd behind; never close it twice
    *cam = (webcam_t){.fd = -1, .buffer = MAP_FAILED};

    return -1;
  }

  size_t px = (size_t)cam->width * (size_t)cam->height;

  nl_free(*gray);
  nl_free(*rgb);

  *gray = NULL;
  *rgb = NULL;

  *gray = nl_malloc(px);
  if (color) {
    *rgb = nl_malloc(px * 3);
  }

  if (!*gray || (color && !*rgb)) {
    nl_free(*gray);
    nl_free(*rgb);

    *gray = NULL;
    *rgb = NULL;

    webcam_cleanup(cam);
    *cam = (webcam_t){.fd = -1, .buffer = MAP_FAILED};

    return -1;
  }

  // hardware control ranges can differ per mode: re-read them
  *hw_exposure = -1;
  *hw_contrast = -1;
  *hw_wb = -1;

  webcam_get_exposure(cam, hw_exposure);
  webcam_get_contrast(cam, hw_contrast);
  webcam_get_white_balance(cam, hw_wb);

  return 0;
}
