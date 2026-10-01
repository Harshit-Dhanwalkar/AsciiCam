/*
 * tests/test_nl_dlfcn.c - load plugin .so files through nl_dlopen() and run
 * them. Guards: symbol lookup (.hash present), external-symbol resolution
 * (nl_calloc/nl_free), and filters' actual output
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) \
 *     -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static \
 *     test_nl_dlfcn.c ../lib/nl_dlfcn.c ../lib/nl_alloc.c ../lib/nl_printf.c \
 *     ../lib/nl_errno.c ../lib/nl_start.c -I../include -I../lib -o
 *     test_nl_dlfcn run from C/ after `make plugins`:  ./tests/test_nl_dlfcn
 *     build
 */
#include "../lib/nolibc.h"
#include "plugins.h"

static int fails, total;
#define CHECK(c, msg)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (!(c)) {                                                                \
      fails++;                                                                 \
      fprintf(stderr, "FAIL [%d] %s\n", __LINE__, msg);                        \
    }                                                                          \
  } while (0)

static int dir_exists(const char *p) {
  char probe[256];
  nl_snprintf(probe, sizeof(probe), "%s/invert.so", p);
  int fd = open(probe, O_RDONLY, 0);
  if (fd < 0) {
    return 0;
  }

  close(fd);

  return 1;
}

static filter_plugin_t *load(const char *dir, const char *so, void **h) {
  char path[256];
  nl_snprintf(path, sizeof(path), "%s/%s", dir, so);
  *h = dlopen(path, 0);
  if (!*h) {
    return (filter_plugin_t *)0;
  }

  filter_plugin_t *(*get)(void) =
      (filter_plugin_t * (*)(void)) dlsym(*h, "plugin_get");

  return get ? get() : (filter_plugin_t *)0;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "build";
  if (!dir_exists(dir)) {
    fprintf(stderr,
            "error: no plugins in '%s' — run `make plugins` first, "
            "or pass the correct directory\n",
            dir);

    return 1;
  }

  void *h;
  uint8_t img[64];
  int p;

  filter_plugin_t *inv = load(dir, "invert.so", &h);
  CHECK(inv != NULL, "invert.so loads + plugin_get found");
  if (inv) {
    for (int i = 0; i < 64; i++) {
      img[i] = (uint8_t)i;
    }

    p = 255;
    inv->process(img, 8, 8, &p);
    CHECK(img[0] == 255 && img[10] == 245, "invert output");

    dlclose(h);
  }

  filter_plugin_t *th = load(dir, "threshold.so", &h);
  CHECK(th != NULL, "threshold.so loads");
  if (th) {
    for (int i = 0; i < 64; i++) {
      img[i] = (uint8_t)(i * 4);
    }

    p = 100;
    th->process(img, 8, 8, &p);
    CHECK(img[10] == 0 && img[40] == 255, "threshold output");

    dlclose(h);
  }

  // edge_detect calls calloc/free -> must resolve to nl_calloc/nl_free
  filter_plugin_t *ed = load(dir, "edge_detect.so", &h);
  CHECK(ed != NULL, "edge_detect.so loads");
  if (ed) {
    uint8_t big[16 * 16];
    for (int i = 0; i < 256; i++) {
      big[i] = (uint8_t)((i % 16) < 8 ? 40 : 200);
    }

    p = 128;
    ed->process(big, 16, 16, &p); /* would jump to NULL if unresolved */
    CHECK(1, "edge_detect ran without crashing");

    dlclose(h);
  }

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
