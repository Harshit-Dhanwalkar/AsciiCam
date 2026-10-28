/*
 * tests/test_capture_pipe.c - frame_exchange state machine and capture
 * thread (src/thread_sharing.c) against a scripted fake camera, no hardware
 *
 * Fake camera writes every frame in 2 phases with a pause in between and
 * scribbles over its "mmap" buffer when buffer is requeued, so a torn read
 * (consumer sees a slot producer is still filling) or a copy made after requeue
 * shows up as a mismatched byte
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static test_capture_pipe.c ../src/thread_sharing.c \
 *     ../lib/nl_printf.c ../lib/nl_errno.c ../lib/nl_alloc.c ../lib/nl_start.c
 * \ -I../include -I../lib -o /tmp/test_capture_pipe
 */

#include "../lib/nolibc.h"

#include "capture.h"
#include "frame_exchange.h"
#include "thread_sharing.h"

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

// Watchdog: a hang is a failure, not a stuck CI job
static void *watchdog(void *arg) {
  (void)arg;

  for (int i = 0; i < 600; i++) { // 60 s
    nl_usleep(100000);
  }

  fprintf(stderr, "FAIL: watchdog fired, a test hung\n");
  nl_exit(2);

  return NULL;
}

// Fake camera
#define FW 64
#define FH 32
#define STRIDE (FW * 2 + 32) // padded rows, like some UVC drivers
#define RAW_BYTES (STRIDE * FH)

static uint8_t raw_buf[RAW_BYTES];

static volatile int script_wait_ok; // 1: frames available, 0: never ready
static volatile long
    script_max_frames; // after this many, fail (-1) if fail_at_end
static volatile int script_fail_at_end;
static volatile int
    script_bad_every; // every Nth capture reports a corrupt frame
static volatile int script_tear_pause_us;

static long frames_made; // good frames written (n)
static long bad_frames;  // corrupt frames reported
static long requeues;
static long captures;

static void put_n(uint8_t *p, long n) {
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t)((unsigned long)n >> (8 * i));
  }
}

static long get_n(const uint8_t *p) {
  unsigned long n = 0;
  for (int i = 0; i < 8; i++) {
    n |= (unsigned long)p[i] << (8 * i);
  }

  return (long)n;
}

int webcam_wait_frame(const webcam_t *cam, int timeout_ms) {
  (void)cam;

  if (!script_wait_ok) {
    nl_usleep((unsigned)timeout_ms * 1000u); // behave like select() timing out

    return -1;
  }

  nl_usleep(200);

  return 0;
}

int webcam_capture_frame(webcam_t *cam, uint8_t *gray) {
  captures++;

  if (script_fail_at_end && frames_made >= script_max_frames) {
    return -1;
  }

  if (script_bad_every > 0 && captures % script_bad_every == 0) {
    bad_frames++; // driver gives buffer back itself: caller must not requeue

    return 1;
  }

  long n = ++frames_made;
  size_t gray_n = (size_t)FW * FH;

  // luma: n in first 8 bytes, byte (n) everywhere else, written in two
  // phases so a concurrent reader would see two different frames
  put_n(gray, n);
  for (size_t i = 8; i < gray_n / 2; i++) {
    gray[i] = (uint8_t)n;
  }

  if (script_tear_pause_us > 0) {
    nl_usleep((unsigned)script_tear_pause_us);
  }

  for (size_t i = gray_n / 2; i < gray_n; i++) {
    gray[i] = (uint8_t)n;
  }

  // "mmap" buffer: n then byte (n + 1), covering whole padded stride
  put_n(raw_buf, n);
  for (size_t i = 8; i < RAW_BYTES; i++) {
    raw_buf[i] = (uint8_t)(n + 1);
  }

  cam->buffer = raw_buf;

  return 0;
}

int webcam_requeue_buffer(webcam_t *cam) {
  (void)cam;

  requeues++;
  // driver is free to overwrite buffer from now on
  for (int i = 0; i < RAW_BYTES; i++) {
    raw_buf[i] = 0xEE;
  }

  return 0;
}

static void fake_reset(void) {
  script_wait_ok = 1;
  script_max_frames = 0;
  script_fail_at_end = 0;
  script_bad_every = 0;
  script_tear_pause_us = 0;
  frames_made = bad_frames = requeues = captures = 0;
}

static webcam_t make_cam(void) {
  webcam_t c = {.fd = 3,
                .buffer = (void *)0,
                .width = FW,
                .height = FH,
                .stride = STRIDE};

  return c;
}

// returns 1 if frame is internally consistent; *n = its frame number
static int frame_ok(const capture_frame_t *f, int want_yuyv, long *n) {
  *n = get_n(f->gray);
  size_t gray_n = (size_t)FW * FH;

  for (size_t i = 8; i < gray_n; i++) {
    if (f->gray[i] != (uint8_t)*n) {
      return 0;
    }
  }

  if (want_yuyv) {
    if (!f->has_yuyv || get_n(f->yuyv) != *n) {
      return 0;
    }

    for (int i = 8; i < RAW_BYTES; i++) {
      if (f->yuyv[i] != (uint8_t)(*n + 1)) {
        return 0; // wrong data, or copied after buffer was requeued
      }
    }
  }

  return 1;
}

// Pure state machine
static void test_exchange_basic(void) {
  frame_exchange_t fx;
  fx_init(&fx);

  CHECK(fx_read_acquire(&fx) == -1, "empty exchange yields nothing");

  int a = fx_write_begin(&fx);
  CHECK(a >= 0 && a < FX_SLOTS, "write slot in range (%d)", a);
  CHECK(fx_read_acquire(&fx) == -1, "unpublished frame is invisible");
  fx_write_publish(&fx);

  int r = fx_read_acquire(&fx);
  CHECK(r == a, "consumer gets published slot (%d vs %d)", r, a);
  CHECK(fx_read_acquire(&fx) == -1, "a frame is delivered once");

  // producer must avoid slot consumer holds
  for (int i = 0; i < 20; i++) {
    int w = fx_write_begin(&fx);
    CHECK(w != r, "writer avoided reading slot");
    fx_write_publish(&fx);
  }

  CHECK(fx.dropped == 19, "19 unread frames replaced (%lu)", fx.dropped);

  // aborted write publishes nothing
  unsigned long before = fx.published;
  fx_write_begin(&fx);
  fx_write_abort(&fx);
  CHECK(fx.published == before, "abort does not publish");
}

static void test_exchange_invariants(void) {
  frame_exchange_t fx;
  fx_init(&fx);

  unsigned long s = 88172645463325252UL;
  int writing = 0;
  int bad = 0;

  for (int i = 0; i < 200000; i++) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;

    switch (s % 4) {
    case 0:
      if (!writing) {
        fx_write_begin(&fx);
        writing = 1;
      }

      break;
    case 1:
      if (writing) {
        fx_write_publish(&fx);
        writing = 0;
      }

      break;
    case 2:
      if (writing) {
        fx_write_abort(&fx);
        writing = 0;
      }

      break;
    default:
      fx_read_acquire(&fx);

      break;
    }

    int w = fx.writing, rd = fx.reading, rdy = fx.ready;
    if ((w >= 0 && (w == rd || w == rdy)) || (rd >= 0 && rd == rdy) ||
        w >= FX_SLOTS || rd >= FX_SLOTS || rdy >= FX_SLOTS) {
      bad++;
    }
  }

  CHECK(bad == 0, "slot roles stayed disjoint across 200000 random ops (%d)",
        bad);
}

// Pipeline
static void run_consumer(capture_pipe_t *p, int want_yuyv, long want_frames,
                         int slow_us, long *torn, long *nonmono, long *got) {
  long last = 0;
  *torn = *nonmono = *got = 0;

  while (*got < want_frames) {
    capture_frame_t *f = NULL;
    int rc = capture_pipe_acquire(p, 200, &f);
    if (rc < 0) {
      break;
    }

    if (rc == 0) {
      continue;
    }

    long n;
    if (!frame_ok(f, want_yuyv, &n)) {
      (*torn)++;
    }

    if (n <= last) {
      (*nonmono)++;
    }

    last = n;
    (*got)++;

    if (slow_us) {
      nl_usleep((unsigned)slow_us); // slow renderer: frames must be dropped
    }
  }
}

static void test_pipe_stream(int want_yuyv, int slow_us, int tear_us,
                             int bad_every, const char *name) {
  fake_reset();
  script_tear_pause_us = tear_us;
  script_bad_every = bad_every;

  webcam_t cam = make_cam();
  capture_pipe_t p;
  CHECK(capture_pipe_start(&p, &cam, want_yuyv) == 0, "%s: start", name);
  CHECK((p.frame[0].yuyv != (void *)0) == (want_yuyv != 0),
        "%s: yuyv slot only when requested", name);

  long torn, nonmono, got;
  run_consumer(&p, want_yuyv, 300, slow_us, &torn, &nonmono, &got);
  capture_pipe_stop(&p);

  CHECK(got == 300, "%s: consumed 300 frames (%ld)", name, got);
  CHECK(torn == 0, "%s: no torn or stale frames (%ld)", name, torn);
  CHECK(nonmono == 0, "%s: frame numbers strictly increase (%ld)", name,
        nonmono);
  CHECK(requeues == frames_made, "%s: every good frame requeued once (%ld/%ld)",
        name, requeues, frames_made);
  CHECK(bad_frames == 0 || requeues + bad_frames == captures,
        "%s: corrupt frames were not requeued by thread", name);
  CHECK((long)p.fx.published == frames_made,
        "%s: every good frame published (%lu/%ld)", name, p.fx.published,
        frames_made);
  CHECK(p.fx.published - p.fx.dropped >= (unsigned long)got - 1 &&
            p.fx.published >= (unsigned long)got,
        "%s: frames not consumed were counted as dropped", name);

  if (slow_us) {
    CHECK(p.fx.dropped > 0, "%s: slow consumer dropped frames (%lu)", name,
          p.fx.dropped);
  }
}

static void test_pipe_failure(void) {
  fake_reset();
  script_fail_at_end = 1;
  script_max_frames = 5;

  webcam_t cam = make_cam();
  capture_pipe_t p;
  CHECK(capture_pipe_start(&p, &cam, 1) == 0, "failure: start");

  long got = 0;
  int rc = 0;
  for (int i = 0; i < 100 && rc >= 0; i++) {
    capture_frame_t *f = NULL;
    rc = capture_pipe_acquire(&p, 100, &f);
    if (rc > 0) {
      got++;
    }
  }

  CHECK(rc == -1, "failure: acquire reports capture error (%d)", rc);
  CHECK(got >= 1 && got <= 5, "failure: frames before error delivered (%ld)",
        got);

  capture_pipe_stop(&p); // thread already ended itself
  capture_pipe_stop(&p); // idempotent
}

static void test_pipe_stop_while_idle(void) {
  fake_reset();
  script_wait_ok = 0; // camera never delivers

  webcam_t cam = make_cam();
  capture_pipe_t p;
  CHECK(capture_pipe_start(&p, &cam, 1) == 0, "idle: start");

  capture_frame_t *f = NULL;
  CHECK(capture_pipe_acquire(&p, 30, &f) == 0, "idle: timeout returns 0");

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  capture_pipe_stop(&p);
  clock_gettime(CLOCK_MONOTONIC, &t1);

  long ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
  CHECK(ms < 500, "idle: stop returns promptly (%ld ms)", ms);
}

static void test_pipe_start_errors(void) {
  capture_pipe_t p;
  CHECK(capture_pipe_start(&p, (webcam_t *)0, 0) == -1, "NULL camera rejected");

  webcam_t cam = make_cam();
  cam.width = 0;
  CHECK(capture_pipe_start(&p, &cam, 0) == -1, "zero width rejected");
  capture_pipe_stop(&p); // never started: must be harmless

  // restart cycle: slab and thread are fully released each time
  fake_reset();
  for (int i = 0; i < 50; i++) {
    cam = make_cam();
    CHECK(capture_pipe_start(&p, &cam, i & 1) == 0, "cycle %d start", i);
    capture_frame_t *f = NULL;
    capture_pipe_acquire(&p, 100, &f);
    capture_pipe_stop(&p);
  }
}

int main(void) {
  nl_thread_t wd;
  nl_thread_create(&wd, watchdog, (void *)0);

  test_exchange_basic();
  test_exchange_invariants();

  test_pipe_stream(1, 0, 0, 0, "colour");
  test_pipe_stream(0, 0, 0, 0, "luma only");
  test_pipe_stream(1, 0, 300, 0, "two-phase writes");
  test_pipe_stream(1, 4000, 100, 0, "slow consumer");
  test_pipe_stream(1, 0, 100, 7, "corrupt frames");
  test_pipe_failure();
  test_pipe_stop_while_idle();
  test_pipe_start_errors();

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
