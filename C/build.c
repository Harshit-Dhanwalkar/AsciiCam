/*
 * build.c - AsciiCam build, written in C (no make / cmake)
 *
 *   cc -o nb build.c && ./nb                # bootstrap once, then:
 *   ./nb [all|plugins|test|e2e|install|install-sdk|clean] [-jN] [CC=cc]
 *                                          [PREFIX=/usr/local] [DESTDIR=...]
 *
 * It rebuilds itself when build.c or nb.h change
 * Linux (nolibc, static) and macOS(system libc) are handled here
 * (WARN: Windows keeps using Makefile until a Windows backend for nb.h exists)
 * Makefile is still reference build
 */
#include "nb.h"

#define BUILD "build"
#define OBJ BUILD "/obj"

static const char *cc = "gcc";
static const char *prefix = "/usr/local";
static const char *destdir = "";
static int jobs = 4;

#if defined(__linux__)
#define LINUX 1
#define SO_EXT "so"
#else
#define LINUX 0
#define SO_EXT "dylib"
#endif

static const char *core_src[] = {"src/app_paths.c",      "src/ascii.c",
                                 "src/capture_reinit.c", "src/main.c",
                                 "src/mouse.c",          "src/plugins.c",
                                 "src/plugin_catalog.c", "src/plugin_picker.c",
                                 "src/thread_sharing.c", "src/timing.c"};
static const char *lib_src[] = {
    "lib/nl_alloc.c",  "lib/nl_gc.c",     "lib/nl_dlfcn.c", "lib/nl_errno.c",
    "lib/nl_getopt.c", "lib/nl_printf.c", "lib/nl_start.c"};
static const char *plugin_name[] = {"edge_detect", "invert", "threshold"};

#define COUNT(a) (sizeof(a) / sizeof(*a))

static void cflags(nb_cmd *c) {
  nb_cmd_add(c, "-Wall", "-Wextra", "-O2", "-Iinclude", "-Ilib",
             "-fno-stack-protector", "-fno-builtin", "-ffreestanding");
#if LINUX
  nb_cmd_add(c, "-DPLATFORM_LINUX", "-D__LINUX_NOLIBC__", "-msse4.1",
             "-nostdinc", "-isystem");
  // compiler's own freestanding headers (immintrin.h ...)
  FILE *f = popen(nb_fmt("%s -print-file-name=include", cc), "r");
  char dir[512] = {0};
  if (f && fgets(dir, sizeof dir, f)) {
    dir[strcspn(dir, "\n")] = 0;
  }

  if (f) {
    pclose(f);
  }

  nb_push(c, nb_fmt("%s", dir));
#else
  nb_cmd_add(c, "-DPLATFORM_MACOS");
#endif
}

static void ldflags(nb_cmd *c) {
#if LINUX
  nb_cmd_add(c, "-nostdlib", "-nostdinc", "-Wl,--no-as-needed", "-B/usr/bin/",
             "-L/usr/lib/x86_64-linux-gnu", "-static", "-msse4.1");
#endif
  (void)c;
}

static const char *obj_of(const char *src) {
  char *o = nb_fmt("%s/%s", OBJ, src);
  for (char *s = o + strlen(OBJ) + 1; *s; s++) {
    if (*s == '/') {
      *s = '_';
    }
  }

  o[strlen(o) - 1] = 'o';

  return o;
}

static int build_all(void) {
  nb_mkdir(OBJ);
  nb_procs ps;
  nb_procs_init(&ps, jobs);

  // every object depends on every header: simple and always correct
  const char *hdrs[64];
  size_t nh = 0;
  const char *hdir[] = {"include", "lib"};
  for (int d = 0; d < 2; d++) {
    FILE *f = popen(nb_fmt("ls %s/*.h", hdir[d]), "r");
    char line[256];
    while (f && fgets(line, sizeof line, f) && nh < 64) {
      line[strcspn(line, "\n")] = 0;
      hdrs[nh++] = nb_fmt("%s", line);
    }

    if (f) {
      pclose(f);
    }
  }

  const char *all[COUNT(core_src) + COUNT(lib_src) + 1];
  size_t na = 0;
  for (size_t i = 0; i < COUNT(core_src); i++) {
    all[na++] = core_src[i];
  }

  for (size_t i = 0; i < COUNT(lib_src); i++) {
    all[na++] = lib_src[i];
  }

#if LINUX
  all[na++] = "src/capture_linux.c";
#endif

  nb_cmd link = {0};
  nb_cmd_add(&link, cc);
  for (size_t i = 0; i < na; i++) {
    const char *o = obj_of(all[i]);
    nb_push(&link, o);
    const char *deps[65];
    size_t nd = 0;
    deps[nd++] = all[i];
    for (size_t h = 0; h < nh; h++) {
      deps[nd++] = hdrs[h];
    }

    if (nb_needs_rebuild(o, deps, nd)) {
      nb_cmd c = {0};
      nb_cmd_add(&c, cc);
      cflags(&c);
      nb_cmd_add(&c, "-c", all[i], "-o", o);
      nb_procs_add(&ps, &c);
    }
  }

  if (!nb_procs_wait(&ps)) {
    return 0;
  }

  const char *target = BUILD "/webcam_ascii";
  if (nb_needs_rebuild(target, (const char **)link.v + 1, link.n - 1)) {
    nb_cmd_add(&link, "-o", target);
    ldflags(&link);
    if (!nb_run(&link)) {
      return 0;
    }
  }

  return 1;
}

static int build_plugins(void) {
  nb_mkdir(BUILD);
  for (size_t i = 0; i < COUNT(plugin_name); i++) {
    const char *src = nb_fmt("filters/%s.c", plugin_name[i]);
    const char *out = nb_fmt(BUILD "/%s." SO_EXT, plugin_name[i]);
    const char *deps[] = {src, "include/plugins.h"};
    if (!nb_needs_rebuild(out, deps, 2)) {
      continue;
    }

    nb_cmd c = {0};
    nb_cmd_add(&c, cc);
    cflags(&c);
#if LINUX
    nb_cmd_add(&c, "-fPIC", "-shared", "-Wl,--hash-style=both");
#else
    nb_cmd_add(&c, "-dynamiclib", "-undefined", "dynamic_lookup");
#endif
    nb_cmd_add(&c, src, "-o", out);
    if (!nb_run(&c)) {
      return 0;
    }
  }

  return 1;
}

static int install(int sdk) {
  const char *share = nb_fmt("%s%s/share/asciicam", destdir, prefix);
  const char *bin = nb_fmt("%s%s/bin", destdir, prefix);
  nb_mkdir(bin);
  nb_mkdir(nb_fmt("%s/plugins", share));
  nb_mkdir(nb_fmt("%s/charsets", share));
  int ok = nb_copy(BUILD "/webcam_ascii", nb_fmt("%s/asciicam", bin), "755");
  for (size_t i = 0; i < COUNT(plugin_name); i++) {
    ok &= nb_copy(nb_fmt(BUILD "/%s." SO_EXT, plugin_name[i]),
                  nb_fmt("%s/plugins", share), "755");
  }

  nb_cmd c = {0};
  nb_cmd_add(&c, "sh", "-c",
             nb_fmt("install -m 644 charsets/* %s/charsets", share));
  ok &= nb_run(&c);
  if (sdk) {
    nb_cmd s = {0};
    nb_cmd_add(&s, "sh", "-c",
               nb_fmt("d=%s/sdk; mkdir -p $d/include $d/lib $d/filters && "
                      "install -m 644 Makefile $d && "
                      "install -m 644 include/*.h $d/include && "
                      "install -m 644 lib/*.h $d/lib && "
                      "install -m 644 filters/*.c $d/filters",
                      share));
    ok &= nb_run(&s);
  }

  return ok;
}

// Unit tests are Makefile's `test` recipe, compiled one by one
static int run_tests(void) {
#if !LINUX
  nb_log("tests are Linux only");

  return 1;
#else
  nb_mkdir(BUILD "/tests");
  nb_cmd base = {0};
  nb_cmd_add(&base, cc);
  cflags(&base);
  nb_cmd_add(&base, "-nostdlib", "-static", "-D__LINUX_NOLIBC__");

  const char *libs =
      "lib/nl_printf.c lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c";
  struct {
    const char *name, *opt, *srcs;
    int arg;
  } t[] = {
      {"alloc", "-O2", "tests/test_nl_alloc.c", 0},
      {"printf", "-O2", "tests/test_nl_printf.c", 0},
      {"dlfcn", "-O2", "tests/test_nl_dlfcn.c lib/nl_dlfcn.c", 1},
      {"mouse", "-O2", "tests/test_mouse.c src/mouse.c", 0},
      {"app_paths", "-O2", "tests/test_app_paths.c src/app_paths.c", 0},
      {"gc_O0", "-O0", "tests/test_nl_gc.c lib/nl_gc.c", 0},
      {"gc_O2", "-O2", "tests/test_nl_gc.c lib/nl_gc.c", 0},
      {"thread", "-O2", "tests/test_nl_thread.c", 0},
  };
  nb_log("plugins first (dlfcn test loads them)");
  if (!build_plugins()) {
    return 0;
  }

  for (size_t i = 0; i < COUNT(t); i++) {
    const char *out = nb_fmt(BUILD "/tests/%s", t[i].name);
    // compile flags come from `base` (everything after compiler)
    nb_cmd cc_line = {0};
    char *flags = nb_fmt("%s", "");
    for (size_t k = 1; k < base.n; k++) {
      flags = nb_fmt("%s '%s'", flags, base.v[k]);
    }

    nb_cmd_add(&cc_line, "sh", "-c",
               nb_fmt("%s %s %s %s %s -o %s && %s%s", cc, t[i].opt, flags,
                      t[i].srcs, libs, out, out, t[i].arg ? " build" : ""));
    if (!nb_run(&cc_line)) {
      nb_log("test %s FAILED", t[i].name);

      return 0;
    }
  }

  nb_log("tests passed (subset: alloc printf dlfcn mouse app_paths gc thread; "
         "full list is still `make test`)");

  return 1;
#endif
}

int main(int argc, char **argv) {
  NB_REBUILD_SELF(argc, argv, "cc");
  const char *target = "all";
  for (int i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "-j", 2) && argv[i][2]) {
      jobs = atoi(argv[i] + 2);
    } else if (!strncmp(argv[i], "CC=", 3)) {
      cc = argv[i] + 3;
    } else if (!strncmp(argv[i], "PREFIX=", 7)) {
      prefix = argv[i] + 7;
    } else if (!strncmp(argv[i], "DESTDIR=", 8)) {
      destdir = argv[i] + 8;
    } else {
      target = argv[i];
    }
  }

  int ok;
  if (!strcmp(target, "all")) {
    ok = build_all() && build_plugins();
  } else if (!strcmp(target, "plugins")) {
    ok = build_plugins();
  } else if (!strcmp(target, "test")) {
    ok = run_tests();
  } else if (!strcmp(target, "install") || !strcmp(target, "install-sdk")) {
    ok = build_all() && build_plugins() && install(target[7] == '-');
  } else if (!strcmp(target, "clean")) {
    ok = nb_rm_tree(BUILD);
  } else {
    nb_log("unknown target '%s'", target);
    ok = 0;
  }

  return ok ? 0 : 1;
}
