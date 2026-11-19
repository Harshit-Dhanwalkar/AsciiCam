/*
 * tests/test_plugin_embedded.c - plugins linked into binary
 * (include/plugin_embedded.h, plugin_load("ramfs:<name>"), catalog_add_archive)
 *
 * Archive is packed here from a real plugin library, so nothing needs to be
 * linked into this test: plugin_set_archive() stands in for built-in one
 *
 *   gcc <test flags> tests/test_plugin_embedded.c src/plugin_catalog.c \
 *     src/plugin_picker.c src/plugins.c lib/nl_dlfcn.c <test libs> -o /tmp/pe
 *   /tmp/pe build/invert.so        (run from C/ after `make plugins`)
 */

#include "../lib/nolibc.h"

#include "plugin_catalog.h"
#include "plugin_embedded.h"
#include "plugins.h"

static int fails, total;
#define CHECK(c, msg)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (!(c)) {                                                                \
      fails++;                                                                 \
      fprintf(stderr, "FAIL [%d] %s\n", __LINE__, msg);                     \
    }                                                                          \
  } while (0)

#define MAX_SO (256 * 1024)
static unsigned char so_img[MAX_SO];
static long so_len;
static unsigned char junk[512];
static unsigned char arc[4 * MAX_SO];
static nl_ramfs_t fs;

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

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }

  return *a == *b;
}

static void pack_archive(void) {
  for (size_t i = 0; i < sizeof(junk); i++) {
    junk[i] = (unsigned char)(0x5a ^ i);
  }

  nl_ramfs_src_t src[] = {
      {"plugins/invert.so", so_img, (size_t)so_len},
      {"plugins/other-one.so", so_img, (size_t)so_len},
      {"plugins/garbage.so", junk, sizeof(junk)},
      {"plugins/bad name.so", so_img, (size_t)so_len}, // space: not a name
      {"plugins/sub/x.so", so_img, (size_t)so_len},    // '/' in name
      {"plugins/readme.txt", junk, 16},                // not a library
      {"charsets/ascii.txt", junk, 16},                // other directory
      {"plugins/.so", so_img, (size_t)so_len},         // empty name
  };
  size_t n = nl_ramfs_pack(arc, sizeof(arc), src, sizeof(src) / sizeof(*src));
  nl_ramfs_init(&fs, arc, n);
}

static void names(void) {
  char s[64];
  CHECK(plugin_embedded_stem("plugins/invert.so", s, sizeof(s)) &&
            streq(s, "invert"),
        "stem of plugins/invert.so");
  CHECK(plugin_embedded_stem("plugins/a-b_C9.so", s, sizeof(s)) &&
            streq(s, "a-b_C9"),
        "stem with - _ digits");
  CHECK(!plugin_embedded_stem("plugins/.so", s, sizeof(s)), "empty stem");
  CHECK(!plugin_embedded_stem("plugins/a.txt", s, sizeof(s)),
        "wrong extension");
  CHECK(!plugin_embedded_stem("plugins/a.so.bak", s, sizeof(s)),
        "trailing text");
  CHECK(!plugin_embedded_stem("plugins/a/b.so", s, sizeof(s)), "slash in name");
  CHECK(!plugin_embedded_stem("plugins/..so", s, sizeof(s)), "dot in name");
  CHECK(!plugin_embedded_stem("charsets/a.so", s, sizeof(s)),
        "other directory");
  CHECK(!plugin_embedded_stem("plugin/a.so", s, sizeof(s)), "directory prefix");
  CHECK(!plugin_embedded_stem("plugins/abcdefgh.so", s, 8),
        "name does not fit");
  CHECK(plugin_embedded_stem("plugins/abcdefg.so", s, 8), "name fits exactly");
  CHECK(plugin_is_embedded("ramfs:x") && plugin_is_embedded("ramfs:"),
        "prefix recognised");
  CHECK(!plugin_is_embedded("ramfs") && !plugin_is_embedded("./ramfs:x") &&
            !plugin_is_embedded("build/invert.so") && !plugin_is_embedded(""),
        "ordinary paths are not embedded");
}

static void catalog(char **envp) {
  plugin_catalog_t c;
  catalog_init(&c, ".cache/pe_src", ".cache/pe_cache", envp);
  CHECK(catalog_add_archive(&c, &fs) == 3, "three usable names (rest skipped)");
  int a = catalog_find(&c, "invert");
  int b = catalog_find(&c, "other-one");
  CHECK(a >= 0 && b >= 0, "both names listed");
  CHECK(catalog_find(&c, "garbage") >= 0, "undecodable image still listed");
  CHECK(catalog_find(&c, "readme") < 0 && catalog_find(&c, "ascii") < 0 &&
            catalog_find(&c, "bad name") < 0 && catalog_find(&c, "x") < 0,
        "non-plugins not listed");
  if (a >= 0) {
    CHECK(c.e[a].state == PC_READY, "entry is ready");
    CHECK(streq(c.e[a].so_path, "ramfs:invert"), "so_path is ramfs:invert");
    CHECK(c.e[a].src_path[0] == '\0', "no source");
    CHECK(catalog_start_build(&c, a) < 0, "nothing to build from");
    CHECK(c.e[a].state == PC_READY, "still ready after refused build");
  }

  // Repeating changes nothing
  int n = c.count;
  CHECK(catalog_add_archive(&c, &fs) == n, "repeat adds nothing");

  // An entry with a source but no build is pointed at built-in copy
  plugin_catalog_t d;
  catalog_init(&d, ".cache/pe_src", ".cache/pe_cache", envp);
  catalog_entry_t *e = &d.e[d.count++];
  nl_memset(e, 0, sizeof(*e));
  nl_snprintf(e->name, sizeof(e->name), "invert");
  nl_snprintf(e->src_path, sizeof(e->src_path), "filters/invert.c");
  nl_snprintf(e->so_path, sizeof(e->so_path), ".cache/pe_cache/invert.so");
  e->state = PC_AVAILABLE;
  catalog_add_archive(&d, &fs);
  CHECK(d.e[0].state == PC_READY && streq(d.e[0].so_path, "ramfs:invert"),
        "source without build uses built-in copy");
  CHECK(streq(d.e[0].src_path, "filters/invert.c"), "source path kept");

  // A cached build, a failed build and a running build are left alone
  plugin_catalog_t f;
  catalog_init(&f, ".cache/pe_src", ".cache/pe_cache", envp);
  pc_state_t keep[] = {PC_READY, PC_FAILED, PC_COMPILING, PC_ACTIVE};
  for (int i = 0; i < 4; i++) {
    nl_memset(&f.e[0], 0, sizeof(f.e[0]));
    f.count = 1;
    nl_snprintf(f.e[0].name, sizeof(f.e[0].name), "invert");
    nl_snprintf(f.e[0].so_path, sizeof(f.e[0].so_path), "cache/invert.so");
    f.e[0].state = keep[i];
    catalog_add_archive(&f, &fs);
    CHECK(streq(f.e[0].so_path, "cache/invert.so") && f.e[0].state == keep[i],
          "existing build is not replaced");
  }

  // Catalog full: nothing is written past end
  plugin_catalog_t g;
  catalog_init(&g, ".cache/pe_src", ".cache/pe_cache", envp);
  g.count = CATALOG_MAX;
  for (int i = 0; i < CATALOG_MAX; i++) {
    nl_snprintf(g.e[i].name, sizeof(g.e[i].name), "p%d", i);
  }

  CHECK(catalog_add_archive(&g, &fs) == CATALOG_MAX, "full catalog unchanged");

  // Empty archive
  nl_ramfs_t none;
  nl_ramfs_init(&none, arc, 0);
  plugin_catalog_t h;
  catalog_init(&h, ".cache/pe_src", ".cache/pe_cache", envp);
  CHECK(catalog_add_archive(&h, &none) == 0, "empty archive adds nothing");
}

static void process_all(const plugin_loader_t *pl, unsigned char *px, int n) {
  int param = 128;
  pl->plugin->process(px, n, 1, &param);
}

static void loading(void) {
  plugin_set_archive(&fs);

  plugin_loader_t emb;
  nl_memset(&emb, 0, sizeof(emb));
  emb.inotify_fd = -1;
  plugin_log_stderr = 0;

  CHECK(plugin_load(&emb, "ramfs:invert") == 0, emb.status_msg);
  CHECK(emb.dl_handle && emb.plugin && emb.plugin->process, "plugin resolved");
  CHECK(emb.tmp_path[0] == '\0', "no temporary copy made");

  // Same plugin from a file
  plugin_loader_t file;
  nl_memset(&file, 0, sizeof(file));
  file.inotify_fd = -1;
  CHECK(plugin_load(&file, "build/invert.so") == 0, file.status_msg);
  CHECK(emb.dl_handle != file.dl_handle, "separate mappings");
  if (emb.plugin && file.plugin) {
    CHECK(streq(emb.plugin->name, file.plugin->name), "same plugin name");
    unsigned char a[16], b[16];
    for (int i = 0; i < 16; i++) {
      a[i] = b[i] = (unsigned char)(i * 17);
    }

    process_all(&emb, a, 16);
    process_all(&file, b, 16);
    int same = 1;
    for (int i = 0; i < 16; i++) {
      same = same && a[i] == b[i];
    }

    CHECK(same, "same output from both loads");
  }

  // No file to watch
  plugin_watch_init(&emb, "ramfs:invert");
  CHECK(emb.inotify_fd == -1, "no watch on a built-in plugin");
  CHECK(streq(emb.path, "ramfs:invert"), "path kept");
  plugin_check_reload(&emb); // must be a no-op, not a crash
  CHECK(emb.plugin != NULL, "still loaded after reload check");

  // Reloading same slot replaces handle cleanly
  CHECK(plugin_load(&emb, "ramfs:other-one") == 0,
        "load another into the slot");

  plugin_cleanup(&emb);
  plugin_cleanup(&file);
  CHECK(emb.dl_handle == NULL, "cleanup closes the handle");

  // Refusals: each says why, none leaves a handle
  struct {
    const char *path;
    const char *what;
  } bad[] = {
      {"ramfs:nosuch", "name not in archive"},
      {"ramfs:", "empty name"},
      {"ramfs:../invert", "path in name"},
      {"ramfs:bad name", "space in name"},
      {"ramfs:sub/x", "slash in name"},
      {"ramfs:garbage", "image that is not a library"},
      {"ramfs:readme", "entry that is not a plugin"},
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
    plugin_loader_t pl;
    nl_memset(&pl, 0, sizeof(pl));
    pl.inotify_fd = -1;
    CHECK(plugin_load(&pl, bad[i].path) != 0, bad[i].what);
    CHECK(pl.dl_handle == NULL && pl.status_msg[0], "no handle, message set");
    plugin_cleanup(&pl);
  }

  // no archive at all
  plugin_set_archive((const nl_ramfs_t *)0);
  nl_ramfs_t none;
  CHECK(plugin_archive(&none) < 0, "no built-in archive in this binary");
  plugin_loader_t pl;
  nl_memset(&pl, 0, sizeof(pl));
  pl.inotify_fd = -1;
  CHECK(plugin_load(&pl, "ramfs:invert") != 0, "load without archive refused");
  CHECK(pl.status_msg[0] != '\0', "message without archive");

  // An archive that failed validation counts as none
  nl_ramfs_t broken;
  nl_ramfs_init(&broken, arc, 3);
  plugin_set_archive(&broken);
  CHECK(plugin_archive(&none) < 0, "rejected archive is not used");
  CHECK(plugin_load(&pl, "ramfs:invert") != 0, "load from rejected archive");

  // A descriptor is never left behind
  plugin_set_archive(&fs);
  int before = open("/dev/null", O_RDONLY, 0);
  close(before);
  for (int i = 0; i < 100; i++) {
    plugin_loader_t q;
    nl_memset(&q, 0, sizeof(q));
    q.inotify_fd = -1;
    plugin_load(&q, "ramfs:invert");
    plugin_cleanup(&q);
    plugin_load(&q, "ramfs:garbage");
    plugin_cleanup(&q);
  }

  int after = open("/dev/null", O_RDONLY, 0);
  close(after);
  CHECK(before == after, "no descriptor leaked");
  plugin_set_archive((const nl_ramfs_t *)0);
}

int main(int argc, char **argv) {
#if defined(__LINUX_NOLIBC__)
  char **envp = argv + argc + 1;
#else
  char **envp = (char **)0;
#endif
  const char *so = argc > 1 ? argv[1] : "build/invert.so";
  so_len = read_file(so, so_img, MAX_SO);
  if (so_len <= 64) {
    fprintf(stderr, "error: cannot read '%s' - run `make plugins` first\n",
               so);

    return 1;
  }

  pack_archive();
  names();
  catalog(envp);
  loading();

  fprintf(stderr, "%d/%d checks passed\n", total - fails, total);

  return fails ? 1 : 0;
}
