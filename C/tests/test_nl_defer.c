/*
 * tests/test_nl_defer.c - lib/nl_defer.h: cleanup runs once on every exit path,
 * in reverse order and NL_TAKE really hands ownership out
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_nl_defer.c lib/nl_printf.c lib/nl_errno.c \
 *     lib/nl_alloc.c lib/nl_start.c -Iinclude -Ilib -o /tmp/td
 */
#include "../lib/nl_defer.h"
#include "../lib/nolibc.h"
#include "nl_alloc_priv.h"

static int pass_n, fail_n;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

#define TAG(n) ((void *)(long)(n))
static int trace[32];
static int ntrace;
static void note(void *v) { trace[ntrace++] = (int)(long)v; }

static size_t live(void) {
  nl_alloc_stats_t s;
  nl_alloc_get_stats(&s);

  return s.live_blocks;
}

// Every way out of a scope
static int exit_path(int which) {
  NL_DEFER(note, TAG(1));
  if (which == 0) {
    return 10; // early return
  }

  for (int i = 0; i < 3; i++) {
    NL_DEFER(note, TAG(100 + i));
    if (which == 1 && i == 1) {
      break; // leaves loop body scope
    }

    if (which == 2) {
      continue;
    }
  }

  return 20; // normal end
}

static void order(void) {
  NL_DEFER(note, TAG(1));
  NL_DEFER(note, TAG(2));
  NL_DEFER(note, TAG(3));
}

static int alloc_fail_midway(int fail_at) {
  NL_AUTOFREE char *a = nl_malloc(100);
  if (fail_at == 1) {
    return -1;
  }

  NL_AUTOFREE char *b = nl_malloc(200);
  if (fail_at == 2) {
    return -2;
  }

  NL_AUTOFREE char *c = nl_malloc(300);
  (void)a;
  (void)b;
  (void)c;

  return 0;
}

static char *make(int ok) {
  NL_AUTOFREE char *buf = nl_malloc(64);
  if (!ok) {
    return (char *)0; // buf freed
  }

  buf[0] = 'x';

  return NL_TAKE(buf); // buf NOT freed
}

static int open_twice(const char *path, int *seen_fd) {
  NL_AUTOCLOSE int fd = open(path, O_RDONLY, 0);
  *seen_fd = fd;
  if (fd < 0) {
    return -1;
  }

  return 0;
}

static int take_fd(const char *path) {
  NL_AUTOCLOSE int fd = open(path, O_RDONLY, 0);

  return NL_TAKE(fd);
}

int main(void) {
  int fx = open("build/webcam_ascii", O_RDONLY, 0);
  if (fx < 0) {
    fprintf(stderr, "error: build/webcam_ascii missing - run `make`\n");
    return 2;
  }

  close(fx);

  size_t base = live();

  ntrace = 0;
  CHECK(exit_path(0) == 10 && ntrace == 1 && trace[0] == 1,
        "early return runs cleanup (%d)", ntrace);
  ntrace = 0;
  CHECK(exit_path(3) == 20 && ntrace == 4 && trace[0] == 100 &&
            trace[2] == 102 && trace[3] == 1,
        "normal path: loop cleanups per iteration, then outer");
  ntrace = 0;
  CHECK(exit_path(1) == 20 && ntrace == 3 && trace[0] == 100 &&
            trace[1] == 101 && trace[2] == 1,
        "break runs cleanup of scope it leaves");
  ntrace = 0;
  CHECK(exit_path(2) == 20 && ntrace == 4, "continue runs cleanup");

  ntrace = 0;
  order();
  CHECK(ntrace == 3 && trace[0] == 3 && trace[1] == 2 && trace[2] == 1,
        "reverse order of declaration");

  CHECK(alloc_fail_midway(1) == -1 && live() == base, "free on first failure");
  CHECK(alloc_fail_midway(2) == -2 && live() == base, "free on second failure");
  CHECK(alloc_fail_midway(0) == 0 && live() == base, "free on success");
  for (int i = 0; i < 1000; i++) {
    alloc_fail_midway(i % 3);
  }

  CHECK(live() == base, "no leak after 1000 mixed calls");

  CHECK(make(0) == NULL && live() == base, "failure path frees");
  char *kept = make(1);
  CHECK(kept && kept[0] == 'x' && live() == base + 1,
        "NL_TAKE keeps block alive");
  nl_free(kept);
  CHECK(live() == base, "caller frees what it took");

  int seen = -5;
  CHECK(open_twice("build/webcam_ascii", &seen) == 0 && seen >= 0,
        "open succeeded");
  int fd_after = open("build/webcam_ascii", O_RDONLY, 0);
  CHECK(fd_after == seen,
        "descriptor %d was closed at scope end (next open "
        "got %d)",
        seen, fd_after);
  close(fd_after);
  CHECK(open_twice("/nonexistent/x", &seen) == -1 && seen < 0,
        "failed open: nothing to close, no crash");

  int taken = take_fd("build/webcam_ascii");
  CHECK(taken >= 0, "NL_TAKE returns descriptor");
  int probe = open("build/webcam_ascii", O_RDONLY, 0);
  CHECK(probe > taken, "descriptor taken out of scope is still open");
  close(probe);
  close(taken);

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
