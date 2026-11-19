/*
 * tests/test_dlopen_mem.c - nl_dlopen_mem(): Loading a shared object that is
 * already in memory (what an archive linked into binary hands to loader)
 *
 *   1. same plugin through nl_dlopen (file) and nl_dlopen_mem (bytes): ABI
 *      marker, plugin_get and its code must come out identical
 *   2. refusals: NULL, empty, truncated, garbage, header only; each gives
 *      NULL and a message, and nothing is loaded
 *   3. no descriptor is left open, whether load worked or not
 *
 * NOTE: Damaged-image robustness is nl_dlopen's own and tested by fuzz_elf.c:
 * nl_dlopen_mem hands bytes to that same code through a memfd
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_dlopen_mem.c lib/nl_dlfcn.c lib/nl_mem.c \
 *     lib/nl_printf.c lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c -Iinclude \
 *     -Ilib -o /tmp/dm
 *
 * /tmp/dm [plugin.so=build/invert.so]
 */

#include "../lib/nolibc.h"

#include "../include/plugins.h"
#include "../lib/nl_dlfcn.h"

#define MAX_FILE (256 * 1024)
#define CODE_BYTES 64

static unsigned char img[MAX_FILE];
static long img_len;

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

static long read_file(const char *path, unsigned char *dst, long cap) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  long n = 0;
  for (;;) {
    long r = read(fd, dst + n, (size_t)(cap - n));
    if (r <= 0) {
      break;
    }

    n += r;
  }

  close(fd);

  return n;
}

static int same(const void *a, const void *b, size_t n) {
  const unsigned char *x = (const unsigned char *)a;
  const unsigned char *y = (const unsigned char *)b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) {
      return 0;
    }
  }

  return 1;
}

// Lowest free descriptor number: a leak shows up as this number growing
static int next_fd(void) {
  int fd = open("/dev/null", O_RDONLY, 0);
  if (fd >= 0) {
    close(fd);
  }

  return fd;
}

static void load_both(const char *path) {
  void *hf = nl_dlopen(path, RTLD_NOW);
  void *hm = nl_dlopen_mem(img, (size_t)img_len, RTLD_NOW);
  CHECK(hf != (void *)0, "nl_dlopen(%s): %s", path, nl_dlerror());
  CHECK(hm != (void *)0, "nl_dlopen_mem: %s", nl_dlerror());
  if (!hf || !hm) {
    return;
  }

  CHECK(nl_dlerror() == (const char *)0, "error left set after good load");
  CHECK(hf != hm, "two loads share one handle");

  // PLUGIN_ABI_EXPORT is optional in a plugin: both loads must agree and when
  // it is there it must be version this host was built with
  const int *abi_f = (const int *)nl_dlsym(hf, "asciicam_plugin_abi");
  const int *abi_m = (const int *)nl_dlsym(hm, "asciicam_plugin_abi");
  nl_dlerror();
  CHECK((abi_f != (const int *)0) == (abi_m != (const int *)0),
        "abi marker found by only one of the loaders");
  if (abi_f && abi_m) {
    CHECK(*abi_f == *abi_m, "abi marker %d (file) vs %d (memory)", *abi_f,
          *abi_m);
    CHECK(*abi_m == PLUGIN_ABI, "abi marker %d, host expects %d", *abi_m,
          PLUGIN_ABI);
  }

  void *pf = nl_dlsym(hf, "plugin_get");
  void *pm = nl_dlsym(hm, "plugin_get");
  CHECK(pf && pm, "plugin_get missing");
  if (pf && pm) {
    CHECK(pf != pm, "both loads mapped at the same address");
    CHECK(same(pf, pm, CODE_BYTES), "code differs between the two loads");
  }

  CHECK(nl_dlsym(hm, "no_such_symbol_here") == (void *)0, "bogus symbol found");
  CHECK(nl_dlerror() != (const char *)0, "no message for missing symbol");

  CHECK(nl_dlclose(hf) == 0, "close (file)");
  CHECK(nl_dlclose(hm) == 0, "close (memory)");
}

static void refusals(void) {
  CHECK(nl_dlopen_mem((const void *)0, 100, RTLD_NOW) == (void *)0,
        "NULL image loaded");
  CHECK(nl_dlerror() != (const char *)0, "NULL image: no message");

  CHECK(nl_dlopen_mem(img, 0, RTLD_NOW) == (void *)0, "empty image loaded");
  CHECK(nl_dlerror() != (const char *)0, "empty image: no message");

  CHECK(nl_dlopen_mem(img, (size_t)img_len / 2, RTLD_NOW) == (void *)0,
        "half an image loaded");
  CHECK(nl_dlerror() != (const char *)0, "half image: no message");

  CHECK(nl_dlopen_mem(img, 64, RTLD_NOW) == (void *)0,
        "ELF header alone loaded");
  CHECK(nl_dlerror() != (const char *)0, "header only: no message");

  static unsigned char junk[4096];
  for (size_t i = 0; i < sizeof(junk); i++) {
    junk[i] = 0x5a;
  }

  CHECK(nl_dlopen_mem(junk, sizeof(junk), RTLD_NOW) == (void *)0,
        "garbage loaded");
  CHECK(nl_dlerror() != (const char *)0, "garbage: no message");

  static unsigned char bad[MAX_FILE];
  for (long i = 0; i < img_len; i++) {
    bad[i] = img[i];
  }

  bad[1] = 'X';
  CHECK(nl_dlopen_mem(bad, (size_t)img_len, RTLD_NOW) == (void *)0,
        "bad magic loaded");
  CHECK(nl_dlerror() != (const char *)0, "bad magic: no message");
}

static void no_fd_leak(void) {
  int before = next_fd();
  CHECK(before >= 0, "cannot open /dev/null");

  for (int i = 0; i < 200; i++) {
    void *h = nl_dlopen_mem(img, (size_t)img_len, RTLD_NOW);
    CHECK(h != (void *)0, "load %d: %s", i, nl_dlerror());
    if (h) {
      nl_dlclose(h);
    }

    // Refused loads go through same close paths
    CHECK(nl_dlopen_mem(img, (size_t)img_len / 2, RTLD_NOW) == (void *)0,
          "truncated image loaded (round %d)", i);
    CHECK(nl_dlopen_mem(img, 64, RTLD_NOW) == (void *)0,
          "header-only image loaded (round %d)", i);
    nl_dlerror();
  }

  CHECK(next_fd() == before, "descriptors leaked: lowest free %d -> %d", before,
        next_fd());
}

int main(int argc, char **argv) {
  const char *so = argc > 1 ? argv[1] : "build/invert.so";
  img_len = read_file(so, img, MAX_FILE);
  if (img_len <= 64) {
    fprintf(stderr, "error: cannot read '%s' - run `make plugins` first\n", so);

    return 1;
  }

  load_both(so);
  refusals();
  no_fd_leak();

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
