/*
 * build.c - AsciiCam build, written in C (no make / cmake)
 *
 *   cc -o nb build.c && ./nb                # bootstrap once, then:
 *   ./nb [all|plugins|test|e2e|install|install-sdk|info|clean] [-jN] [CC=cc]
 *                                          [PREFIX=/usr/local] [DESTDIR=...]
 *
 * It rebuilds itself when build.c or nb.h change
 * NOTE: Only Linux (nolibc, static) and macOS (system libc) are built entirely
 * from here: nothing in this file and nothing installed app does, needs make
 * (WARN: Windows keeps using Makefile until a Windows backend for nb.h exists)
 * and `make sanitize`
 */

#include "nb.h"

// How plugins are compiled is shared with running app, which builds them on
// demand from picker (src/plugin_catalog.c)
#include "include/plugin_build.h"

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

static const char *core_src[] = {
    "src/app_paths.c",      "src/ascii.c",          "src/capture_demo.c",
    "src/capture_reinit.c", "src/main.c",           "src/mouse.c",
    "src/plugins.c",        "src/plugin_catalog.c", "src/plugin_picker.c",
    "src/thread_sharing.c", "src/timing.c"};
static const char *lib_src[] = {
    "lib/nl_alloc.c",  "lib/nl_gc.c",     "lib/nl_dlfcn.c", "lib/nl_errno.c",
    "lib/nl_getopt.c", "lib/nl_printf.c", "lib/nl_start.c"};
static const char *plugin_name[] = {"edge_detect", "invert", "threshold"};

#define COUNT(a) (sizeof(a) / sizeof(*a))

// Directory of compiler's own freestanding headers (immintrin.h ...)
// Linux build uses no other system headers
static const char *cc_include_dir(void) {
  static char dir[512];
  if (!dir[0]) {
    FILE *f = popen(nb_fmt("%s -print-file-name=include", cc), "r");
    if (f && fgets(dir, sizeof dir, f)) {
      dir[strcspn(dir, "\n")] = 0;
    }

    if (f) {
      pclose(f);
    }
  }

  return dir;
}

// Flags every compile uses
static void cflags(nb_cmd *c) {
  nb_add_words(c, PB_COMMON);
  nb_add_words(c, PB_TARGET);
  nb_cmd_add(c, "-Iinclude", "-Ilib");
#if LINUX
  nb_cmd_add(c, "-isystem", cc_include_dir());
#endif
}

static void ldflags(nb_cmd *c) {
#if LINUX
  nb_cmd_add(c, "-nostdlib", "-nostdinc", "-Wl,--no-as-needed", "-B/usr/bin/",
             "-L/usr/lib/x86_64-linux-gnu", "-static", "-msse4.1");
#else
  nb_add_words(c, "-framework AVFoundation -framework CoreMedia "
                  "-framework CoreVideo -framework Foundation -lpthread");
#endif
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
  const char *hdrs[128];
  size_t nh = 0;
  const char *hdir[] = {"include", "lib"};
  for (int d = 0; d < 2; d++) {
    FILE *f = popen(nb_fmt("ls %s/*.h", hdir[d]), "r");
    char line[256];
    while (f && fgets(line, sizeof line, f) && nh < 128) {
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
#else
  all[na++] = "src/capture_macos.m";
#endif

  nb_cmd link = {0};
  nb_cmd_add(&link, cc);
  for (size_t i = 0; i < na; i++) {
    const char *o = obj_of(all[i]);
    nb_push(&link, o);
    const char *deps[129];
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
    const char *deps[] = {src, "include/plugins.h", "include/plugin_build.h"};
    if (!nb_needs_rebuild(out, deps, 3)) {
      continue;
    }

    // Same flags app uses when it builds a plugin on demand; written to a
    // temporary name and renamed so nothing sees a half-written library
    const char *tmp = nb_fmt("%s.tmp", out);
    nb_cmd c = {0};
    nb_cmd_add(&c, cc);
    cflags(&c);
    nb_add_words(&c, PB_SHARED);
    nb_cmd_add(&c, src, "-o", tmp);
    if (!nb_run(&c) || rename(tmp, out) != 0) {
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
    // plugin build needs: headers and plugin sources
    // No Makefile: app runs compiler itself
    nb_cmd s = {0};
    nb_cmd_add(&s, "sh", "-c",
               nb_fmt("d=%s/sdk; mkdir -p $d/include $d/lib $d/filters && "
                      "install -m 644 include/*.h $d/include && "
                      "install -m 644 lib/*.h $d/lib && "
                      "install -m 644 filters/*.c $d/filters",
                      share));
    ok &= nb_run(&s);
  }

  return ok;
}

#if LINUX
// Test programs: each is built from its own sources plus small nolibc core, and
// run
// Order and flags `make test` follow
typedef struct {
  const char *name, *opt, *extra, *srcs;
  const char *args[3]; // each non-NULL entry is one run, e.g. "100000 1"
} nb_test;

static const char *test_libs[] = {"lib/nl_printf.c", "lib/nl_errno.c",
                                  "lib/nl_alloc.c", "lib/nl_start.c", NULL};

static int run_one_test(const nb_test *t) {
  const char *out = nb_fmt(BUILD "/tests/%s", t->name);
  nb_cmd c = {0};
  nb_cmd_add(&c, cc, t->opt);
  nb_add_words(&c, "-D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc");
  nb_cmd_add(&c, "-isystem", cc_include_dir());
  nb_add_words(&c, "-ffreestanding -fno-builtin -fno-stack-protector -nostdlib "
                   "-static -msse4.1 -Iinclude -Ilib");
  if (t->extra[0]) {
    nb_add_words(&c, t->extra);
  }

  nb_add_words(&c, t->srcs);
  for (int i = 0; test_libs[i]; i++) {
    nb_push(&c, test_libs[i]);
  }

  nb_cmd_add(&c, "-o", out);
  if (!nb_run(&c)) {
    nb_log("test %s: BUILD FAILED", t->name);

    return 0;
  }

  for (int i = 0; i < 3; i++) {
    if (!t->args[i] && i > 0) {
      break;
    }

    nb_cmd r = {0};
    nb_push(&r, out);
    if (t->args[i]) {
      nb_add_words(&r, t->args[i]);
    }

    if (!nb_run(&r)) {
      nb_log("test %s: FAILED", t->name);

      return 0;
    }
  }

  return 1;
}

static int run_tests(void) {
  nb_mkdir(BUILD "/tests");
  nb_mkdir(BUILD "/tests");
  nb_log("plugins first (several tests load them)");
  if (!build_plugins()) {
    return 0;
  }

  static const nb_test t[] = {
      {"alloc", "-O0", "", "tests/test_nl_alloc.c", {NULL}},
      {"printf", "-O0", "", "tests/test_nl_printf.c", {NULL}},
      {"dlfcn", "-O0", "", "tests/test_nl_dlfcn.c lib/nl_dlfcn.c", {"build"}},
      {"fuzz",
       "-O0",
       "",
       "tests/fuzz_nolibc.c lib/nl_getopt.c",
       {"100000 1", "100000 2", "100000 3"}},
      {"slice_O0", "-O0", "", "tests/test_nl_slice.c", {NULL}},
      {"slice_O2", "-O2", "", "tests/test_nl_slice.c", {NULL}},
      {"defer_O0", "-O0", "", "tests/test_nl_defer.c", {NULL}},
      {"defer_O2", "-O2", "", "tests/test_nl_defer.c", {NULL}},
      {"fuzz_elf",
       "-O2",
       "",
       "tests/fuzz_elf.c lib/nl_dlfcn.c",
       {"build/edge_detect.so 2000 1 build/tests/fuzz_elf.tmp",
        "build/invert.so 2000 1 build/tests/fuzz_elf.tmp",
        "build/threshold.so 2000 1 build/tests/fuzz_elf.tmp"}},
      {"mouse", "-O0", "", "tests/test_mouse.c src/mouse.c", {NULL}},
      {"demo", "-O2", "", "tests/test_demo.c src/capture_demo.c", {NULL}},
      {"reinit",
       "-O2",
       "",
       "tests/test_capture_reinit.c src/capture_reinit.c",
       {NULL}},
      {"app_paths",
       "-O2",
       "",
       "tests/test_app_paths.c src/app_paths.c",
       {NULL}},
      {"catalog",
       "-O2",
       "",
       "tests/test_plugin_catalog.c src/plugin_catalog.c src/plugin_picker.c "
       "src/plugins.c lib/nl_dlfcn.c",
       {NULL}},
      {"copy_file", "-O2", "", "tests/test_copy_file.c lib/nl_dlfcn.c", {NULL}},
      {"gc_O0", "-O0", "", "tests/test_nl_gc.c lib/nl_gc.c", {NULL}},
      {"gc_O2", "-O2", "", "tests/test_nl_gc.c lib/nl_gc.c", {NULL}},
      {"thread", "-O2", "", "tests/test_nl_thread.c", {NULL}},
      {"capture_pipe",
       "-O2",
       "",
       "tests/test_capture_pipe.c src/thread_sharing.c",
       {NULL}},
      {"security",
       "-O0",
       "-DTESTING",
       "tests/security_tests.c filters/edge_detect.c filters/invert.c "
       "filters/threshold.c",
       {NULL}},
  };

  for (size_t i = 0; i < COUNT(t); i++) {
    if (!run_one_test(&t[i])) {
      return 0;
    }
  }

  nb_log("All tests passed");

  return 1;
}

// Main loop linked against a software camera, driven through a pty
static int run_e2e(void) {
  nb_mkdir(BUILD "/tests");
  const char *bin = BUILD "/tests/asciicam_fake";
  nb_cmd c = {0};
  nb_cmd_add(&c, cc);
  cflags(&c);
  for (size_t i = 0; i < COUNT(core_src); i++) {
    nb_push(&c, core_src[i]);
  }

  nb_push(&c, "tests/fake_capture.c");
  for (size_t i = 0; i < COUNT(lib_src); i++) {
    nb_push(&c, lib_src[i]);
  }

  nb_cmd_add(&c, "-o", bin);
  ldflags(&c);
  if (!nb_run(&c)) {
    return 0;
  }

  // Second program: the real binary, for the checks that run it with --demo
  if (!build_all()) {
    return 0;
  }

  nb_cmd py = {0};
  nb_cmd_add(&py, "python3", "tests/e2e_capture.py", bin,
             BUILD "/webcam_ascii");

  return nb_run(&py);
}
#else
static int run_tests(void) {
  nb_log("tests are Linux only");

  return 1;
}

static void info(void) {
  printf("Platform : %s\n", LINUX ? "linux" : "macos");
  printf("CC       : %s\n", cc);
  printf("Prefix   : %s%s\n", destdir, prefix);
  printf("Plugins  : %s %s\n", PB_COMMON, PB_TARGET);
  printf("Shared   : %s\n", PB_SHARED);
}
#endif

static void info(void) {
  printf("Platform : %s\n", LINUX ? "linux" : "macos");
  printf("CC       : %s\n", cc);
  printf("Prefix   : %s%s\n", destdir, prefix);
  printf("Plugins  : %s %s\n", PB_COMMON, PB_TARGET);
  printf("Shared   : %s\n", PB_SHARED);
}

int main(int argc, char **argv) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  fputs("nb: Windows is not supported yet; use `make`\n", stderr);

  return 1;
#else
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
  } else if (!strcmp(target, "e2e")) {
    ok = run_e2e();
  } else if (!strcmp(target, "info")) {
    info();
    ok = 1;
  } else if (!strcmp(target, "install") || !strcmp(target, "install-sdk")) {
    ok = build_all() && build_plugins() && install(target[7] == '-');
  } else if (!strcmp(target, "clean")) {
    ok = nb_rm_tree(BUILD) && nb_rm_tree(".cache");
  } else {
    nb_log("unknown target '%s'", target);
    ok = 0;
  }

  return ok ? 0 : 1;
#endif
}
