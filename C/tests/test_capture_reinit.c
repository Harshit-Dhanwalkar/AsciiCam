/*
 * tests/test_capture_reinit.c - capture_reinit() against a scripted fake
 * camera backend (no hardware): success, driver-adjusted size, init failure,
 * stale fd after a failed init, allocation failure, and "requested mode
 * failed -> restore previous" sequence main.c performs.
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) \
 *     -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static \
 *     test_capture_reinit.c ../src/capture_reinit.c ../lib/nl_printf.c \
 *     ../lib/nl_errno.c ../lib/nl_alloc.c ../lib/nl_start.c \
 *     -I../include -I../lib -o test_capture_reinit
 */

#include "../lib/nolibc.h"
#include "capture.h"

static int fails, total;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (!(c)) {                                                                \
      fails++;                                                                 \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

// fake backend
static int open_cameras;  /* webcam_init successes - cleanups of open ones */
static int cleanups_open; /* cleanup calls that had fd >= 0 (real closes) */
static int init_calls;
static int req_w, req_h;     /* last size requested from init */
static int fail_next_init;   /* script: 1 = next init fails */
static int stale_fd_on_fail; /* script: failed init leaves fd = 7 behind */
static int huge_dims;        /* script: driver reports absurd dims */
static int quant;            /* driver rounds dims up to a multiple of this */

int webcam_init(webcam_t *cam, const char *device, int width, int height) {
  (void)device;

  init_calls++;
  req_w = width;
  req_h = height;
  if (fail_next_init) {
    fail_next_init = 0;
    if (stale_fd_on_fail) {
      cam->fd = 7;
    }

    return -1;
  }

  cam->fd = 5;
  cam->buffer = (void *)0;
  if (huge_dims) {
    cam->width = 0x7fffffff;
    cam->height = 0x7fffffff;
  } else {
    int q = quant ? quant : 1;

    cam->width = (width + q - 1) / q * q;
    cam->height = (height + q - 1) / q * q;
  }

  open_cameras++;

  return 0;
}

void webcam_cleanup(webcam_t *cam) {
  if (cam->fd >= 0) {
    cleanups_open++;
    open_cameras--;
  }

  cam->fd = -1;
  cam->buffer = MAP_FAILED;
}

int webcam_get_exposure(const webcam_t *c, int *v) {
  (void)c;
  *v = 111;

  return 0;
}

int webcam_get_contrast(const webcam_t *c, int *v) {
  (void)c;
  *v = 222;

  return 0;
}

int webcam_get_white_balance(const webcam_t *c, int *v) {
  (void)c;
  *v = 3333;

  return 0;
}

// WARN: unused by capture_reinit but declared in capture.h
int webcam_wait_frame(const webcam_t *c, int t) {
  (void)c;
  (void)t;

  return 0;
}

int webcam_capture_frame(webcam_t *c, uint8_t *g) {
  (void)c;
  (void)g;

  return 0;
}

int webcam_requeue_buffer(webcam_t *c) {
  (void)c;

  return 0;
}

int webcam_set_auto_exposure(const webcam_t *c, int e) {
  (void)c;
  (void)e;

  return 0;
}

int webcam_set_auto_white_balance(const webcam_t *c, int e) {
  (void)c;
  (void)e;

  return 0;
}

int webcam_adjust_exposure(const webcam_t *c, int d, int *o) {
  (void)c;
  (void)d;
  (void)o;

  return 0;
}

int webcam_adjust_contrast(const webcam_t *c, int d, int *o) {
  (void)c;
  (void)d;
  (void)o;

  return 0;
}

int webcam_adjust_white_balance(const webcam_t *c, int d, int *o) {
  (void)c;
  (void)d;
  (void)o;

  return 0;
}

int webcam_get_exposure_range(const webcam_t *c, int *a, int *b) {
  (void)c;
  (void)a;
  (void)b;

  return 0;
}

int webcam_get_contrast_range(const webcam_t *c, int *a, int *b) {
  (void)c;
  (void)a;
  (void)b;

  return 0;
}

int webcam_get_white_balance_range(const webcam_t *c, int *a, int *b) {
  (void)c;
  (void)a;
  (void)b;

  return 0;
}

// helpers
static void reset_script(void) {
  open_cameras = cleanups_open = init_calls = 0;
  req_w = req_h = 0;
  fail_next_init = stale_fd_on_fail = huge_dims = 0;
  quant = 0;
}

// an "already running" camera at 640x480 with matching buffers
static void start(webcam_t *cam, uint8_t **g, uint8_t **r, int color) {
  *cam = (webcam_t){.fd = -1, .buffer = MAP_FAILED};
  webcam_init(cam, "x", 640, 480);

  *g = nl_malloc(640 * 480);
  *r = color ? nl_malloc(640 * 480 * 3) : (uint8_t *)0;
  for (int i = 0; i < 640 * 480; i++) {
    (*g)[i] = (uint8_t)i;
  }

  init_calls = 0; // only count calls made by capture_reinit
}

static void test_success(void) {
  reset_script();
  webcam_t cam;
  uint8_t *g, *r;
  int he = -1, hc = -1, hw = -1;
  start(&cam, &g, &r, 1);

  int rc = capture_reinit(&cam, "x", 1280, 720, &g, &r, 1, &he, &hc, &hw);
  CHECK(rc == 0, "rc=%d", rc);
  CHECK(cam.width == 1280 && cam.height == 720, "dims %dx%d", cam.width,
        cam.height);
  CHECK(cam.fd >= 0, "camera open");
  CHECK(g && r, "buffers present");
  if (g && r) { // must be writable over whole new size
    g[1280 * 720 - 1] = 7;
    r[1280 * 720 * 3 - 1] = 7;
  }

  CHECK(he == 111 && hc == 222 && hw == 3333, "hw values re-read %d %d %d", he,
        hc, hw);
  CHECK(open_cameras == 1, "exactly one open camera, got %d", open_cameras);
  CHECK(cleanups_open == 1, "old camera closed once, got %d", cleanups_open);

  // no colour: rgb must end up NULL
  rc = capture_reinit(&cam, "x", 320, 240, &g, &r, 0, &he, &hc, &hw);
  CHECK(rc == 0 && r == (uint8_t *)0, "rgb NULL when colour off");
  CHECK(cam.width == 320 && cam.height == 240, "dims %dx%d", cam.width,
        cam.height);

  nl_free(g);
  webcam_cleanup(&cam);

  CHECK(open_cameras == 0, "no camera left open");
}

static void test_even_and_driver_adjust(void) {
  reset_script();
  webcam_t cam;
  uint8_t *g, *r;
  int he, hc, hw;
  start(&cam, &g, &r, 0);

  capture_reinit(&cam, "x", 321, 241, &g, &r, 0, &he, &hc, &hw);
  CHECK(req_w == 322 && req_h == 242, "odd request rounded to even: %dx%d",
        req_w, req_h);

  // driver snaps to multiples of 16: buffers must follow cam->width/height, not
  // requested size
  quant = 16;
  int rc = capture_reinit(&cam, "x", 700, 500, &g, &r, 0, &he, &hc, &hw);
  CHECK(rc == 0 && cam.width == 704 && cam.height == 512, "driver size %dx%d",
        cam.width, cam.height);
  g[704 * 512 - 1] = 1; // would overflow a buffer sized for 700x500

  nl_free(g);
  webcam_cleanup(&cam);
}

static void test_init_failure(void) {
  reset_script();
  webcam_t cam;
  uint8_t *g;
  uint8_t *r;
  int he = 5;
  int hc = 5;
  int hw = 5;

  start(&cam, &g, &r, 1);
  uint8_t *g0 = g;
  uint8_t *r0 = r;
  fail_next_init = 1;

  int rc = capture_reinit(&cam, "x", 1280, 720, &g, &r, 1, &he, &hc, &hw);
  CHECK(rc == -1, "rc=%d", rc);
  CHECK(cam.fd == -1 && cam.buffer == MAP_FAILED,
        "camera left in closed state");
  CHECK(g == g0 && r == r0, "old buffers untouched");
  CHECK(g[640 * 480 - 1] == (uint8_t)(640 * 480 - 1), "old contents intact");
  CHECK(he == 5 && hc == 5 && hw == 5, "hw values untouched on failure");
  CHECK(open_cameras == 0 && cleanups_open == 1,
        "old closed once (open=%d closes=%d)", open_cameras, cleanups_open);

  // what main does next: restore previous size
  int rc2 = capture_reinit(&cam, "x", 640, 480, &g, &r, 1, &he, &hc, &hw);
  CHECK(rc2 == 0 && cam.width == 640 && cam.height == 480, "restore ok");
  CHECK(open_cameras == 1, "one camera open after restore");

  nl_free(g);
  nl_free(r);
  webcam_cleanup(&cam);

  CHECK(open_cameras == 0, "closed at end");
}

static void test_stale_fd(void) {
  reset_script();

  webcam_t cam;
  uint8_t *g, *r;
  int he, hc, hw;
  start(&cam, &g, &r, 0);

  fail_next_init = 1;
  stale_fd_on_fail =
      1; // a failed init leaves fd=7 behind, as capture_linux does
  int rc = capture_reinit(&cam, "x", 800, 600, &g, &r, 0, &he, &hc, &hw);
  CHECK(rc == -1 && cam.fd == -1, "stale fd cleared (fd=%d)", cam.fd);

  int before = cleanups_open;
  webcam_cleanup(&cam); // main's final cleanup

  CHECK(cleanups_open == before, "no second close of stale fd");

  nl_free(g);
}

static void test_alloc_failure(void) {
  reset_script();
  webcam_t cam;
  uint8_t *g, *r;
  int he = 9, hc = 9, hw = 9;
  start(&cam, &g, &r, 1);

  huge_dims = 1; /* init succeeds, but pixel buffers cannot be allocated */
  int rc = capture_reinit(&cam, "x", 800, 600, &g, &r, 1, &he, &hc, &hw);
  CHECK(rc == -1, "rc=%d", rc);
  CHECK(cam.fd == -1 && cam.buffer == MAP_FAILED, "camera closed again");
  CHECK(open_cameras == 0, "newly opened camera was not leaked (%d)",
        open_cameras);
  CHECK(g == (uint8_t *)0 && r == (uint8_t *)0,
        "buffers are NULL after an allocation failure (no dangling pointers)");
  CHECK(he == 9 && hc == 9 && hw == 9, "hw values untouched");

  // caller's recovery: same call with previous size works from NULL state
  huge_dims = 0;
  CHECK(capture_reinit(&cam, "x", 640, 480, &g, &r, 1, &he, &hc, &hw) == 0,
        "recovers afterwards");
  CHECK(g && r, "buffers allocated again");
  nl_free(g);
  nl_free(r);
  webcam_cleanup(&cam);
}

/* Real memory shape: colour 640x480 -> 1280x720 -> 1280x720 ... 2 MiB
 * small-block arena cannot hold old and new gray/rgb frames at once;
 * freeing old before allocating new is what makes this succeed. */
static void test_arena_pressure(void) {
  reset_script();
  webcam_t cam;
  uint8_t *g, *r;
  int he, hc, hw, bad = 0;
  start(&cam, &g, &r, 1);

  static const int sizes[][2] = {{1280, 720}, {1024, 576}, {1280, 720},
                                 {800, 600},  {1280, 720}, {640, 480}};
  for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    int rc = capture_reinit(&cam, "x", sizes[i][0], sizes[i][1], &g, &r, 1, &he,
                            &hc, &hw);
    if (rc != 0) {
      bad++;
      continue;
    }
    g[(size_t)cam.width * (size_t)cam.height - 1] = 1;
    r[(size_t)cam.width * (size_t)cam.height * 3 - 1] = 1;
  }
  CHECK(bad == 0, "%d of 6 resizes failed under arena pressure", bad);
  nl_free(g);
  nl_free(r);
  webcam_cleanup(&cam);
}

static void test_repeated(void) {
  reset_script();
  webcam_t cam;
  uint8_t *g, *r;
  int he, hc, hw;
  start(&cam, &g, &r, 1);
  int bad = 0;
  for (int i = 0; i < 200; i++) {
    int w = 160 + (i * 37) % 1120, h = 120 + (i * 53) % 600;
    if (i % 7 == 3)
      fail_next_init = 1;
    int rc = capture_reinit(&cam, "x", w, h, &g, &r, 1, &he, &hc, &hw);
    if (rc != 0) {
      rc = capture_reinit(&cam, "x", 640, 480, &g, &r, 1, &he, &hc, &hw);
      if (rc != 0)
        bad++;
    }

    // last byte of both buffers must be writable at current size
    g[(size_t)cam.width * (size_t)cam.height - 1] = 1;
    r[(size_t)cam.width * (size_t)cam.height * 3 - 1] = 1;
    if (open_cameras != 1)
      bad++;
  }

  CHECK(bad == 0, "%d inconsistencies over 200 reinits", bad);

  nl_free(g);
  nl_free(r);
  webcam_cleanup(&cam);

  CHECK(open_cameras == 0, "closed at end");
}

int main(void) {
  test_success();
  test_even_and_driver_adjust();
  test_init_failure();
  test_stale_fd();
  test_alloc_failure();
  test_arena_pressure();
  test_repeated();

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
