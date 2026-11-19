/*
 * tools/mkramfs.c - pack files into an nl_ramfs archive (lib/nl_ramfs.h)
 *
 * Runs on build machine:
 *
 *   cc -O2 tools/mkramfs.c -o build/mkramfs
 *   build/mkramfs OUT [NAME=FILE]...
 *
 * NAME is what app looks entry up by (e.g. plugins/invert.so), FILE is where
 * its bytes come from
 * Entries are sorted by NAME, so same inputs always give same archive byte for
 * byte
 * OUT is written to OUT.tmp first and renamed, so an interrupted run never
 * leaves half an archive behind
 * No NAME=FILE arguments gives a valid empty archive
 */

#define NL_RAMFS_HOSTED 1
#include "../lib/nl_ramfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE (64UL << 20)

typedef struct {
  char *name;
  uint8_t *data;
  size_t len;
} item_t;

static int die(const char *what, const char *arg) {
  fprintf(stderr, "mkramfs: %s%s%s\n", what, arg ? ": " : "", arg ? arg : "");

  return 1;
}

static uint8_t *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }

  size_t cap = 4096, n = 0;
  uint8_t *grown = realloc(buf, cap);
  if (!grown) {
    free(buf);
    buf = NULL;

    break;
  }

  buf = grown;
  while (buf) {
    size_t r = fread(buf + n, 1, cap - n, f);
    n += r;
    if (n < cap) {
      break; // EOF or error
    }

    if (cap >= MAX_FILE) {
      free(buf);
      buf = NULL;

      break;
    }

    cap *= 2;
    buf = realloc(buf, cap);
  }

  int bad = ferror(f);
  fclose(f);
  if (bad) {
    free(buf);

    return NULL;
  }

  *len = n;

  return buf;
}

static int by_name(const void *a, const void *b) {
  return strcmp(((const item_t *)a)->name, ((const item_t *)b)->name);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    return die("usage: mkramfs OUT [NAME=FILE]...", NULL);
  }

  size_t n = (size_t)argc - 2;
  if (n > NL_RAMFS_MAX_ENTRIES) {
    return die("too many entries", NULL);
  }

  item_t *it = calloc(n ? n : 1, sizeof(*it));
  nl_ramfs_src_t *src = calloc(n ? n : 1, sizeof(*src));
  if (!it || !src) {
    return die("out of memory", NULL);
  }

  for (size_t i = 0; i < n; i++) {
    char *arg = argv[i + 2];
    const char *eq = strchr(arg, '=');
    if (!eq || eq == arg || !eq[1]) {
      return die("argument is not NAME=FILE", arg);
    }

    size_t nlen = (size_t)(eq - arg);
    it[i].name = malloc(nlen + 1);
    if (!it[i].name) {
      return die("out of memory", NULL);
    }

    memcpy(it[i].name, arg, nlen);
    it[i].name[nlen] = '\0';
    it[i].data = slurp(eq + 1, &it[i].len);
    if (!it[i].data) {
      return die("cannot read", eq + 1);
    }
  }

  qsort(it, n, sizeof(*it), by_name);
  for (size_t i = 0; i < n; i++) {
    src[i].name = it[i].name;
    src[i].data = it[i].data;
    src[i].len = it[i].len;
  }

  size_t need = nl_ramfs_pack(NULL, 0, src, (uint32_t)n);
  if (need == 0) {
    return die("cannot pack (duplicate or over-long name, or too large)", NULL);
  }

  uint8_t *out = malloc(need);
  if (!out || nl_ramfs_pack(out, need, src, (uint32_t)n) != need) {
    return die("pack failed", NULL);
  }

  nl_ramfs_t check;
  if (nl_ramfs_init(&check, out, need) < 0 || check.count != n) {
    return die("internal error: packed archive does not validate", NULL);
  }

  size_t plen = strlen(argv[1]) + 5;
  char *tmp = malloc(plen);
  if (!tmp) {
    return die("out of memory", NULL);
  }

  snprintf(tmp, plen, "%s.tmp", argv[1]);
  FILE *f = fopen(tmp, "wb");
  if (!f || fwrite(out, 1, need, f) != need || fclose(f) != 0) {
    remove(tmp);

    return die("cannot write", tmp);
  }

  if (rename(tmp, argv[1]) != 0) {
    remove(tmp);

    return die("cannot rename to", argv[1]);
  }

  fprintf(stderr, "mkramfs: %s: %zu entries, %zu bytes\n", argv[1], n, need);

  return 0;
}
