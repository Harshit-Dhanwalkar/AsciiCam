/*
 * tests/test_copy_file.c - plugins.c copy_file(): reports failure when source
 * cannot be read (it used to return success with a truncated copy) and leaves
 * no descriptor open on any path
 *
 * Includes .c file to reach static function
 */

#include "../src/plugins.c"

static int pass_n, fail_n;
#define CHECK(c, msg)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, msg);                    \
    }                                                                          \
  } while (0)

static long size_of(const char *p) {
  int fd = open(p, O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  long n = __sc3(SYS_lseek, fd, 0, 2);
  close(fd);

  return n;
}

int main(void) {
  plugin_log_stderr = 0;
  const char *dst = "build/tests/copy_file.out";

  // Next descriptor number before and after: nothing may leak
  int probe = open("build/webcam_ascii", O_RDONLY, 0);
  close(probe);

  CHECK(copy_file("build/webcam_ascii", dst) == 0, "ordinary copy succeeds");
  CHECK(size_of(dst) == size_of("build/webcam_ascii") && size_of(dst) > 0,
        "copy has same size");
  unlink(dst);

  // A directory opens fine but read() fails with EISDIR
  CHECK(copy_file("build", dst) == -1, "unreadable source is an error");
  CHECK(size_of(dst) < 0, "no partial output left behind");

  CHECK(copy_file("build/does-not-exist", dst) == -1, "missing source");
  CHECK(copy_file("build/webcam_ascii", "build/no-such-dir/x") == -1,
        "uncreatable destination");

  for (int i = 0; i < 2000; i++) { // would run out of descriptors if any leaked
    copy_file("build", dst);
    copy_file("build/does-not-exist", dst);
    copy_file("build/webcam_ascii", "build/no-such-dir/x");
  }

  int probe2 = open("build/webcam_ascii", O_RDONLY, 0);
  CHECK(probe2 == probe, "no descriptor leaked over 6000 failing calls");
  close(probe2);

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
