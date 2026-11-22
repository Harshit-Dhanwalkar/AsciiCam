/*
 * build.c - AsciiCam build, written in C (no make / cmake)
 *
 *   cc -o nb build.c && ./nb                # bootstrap once, then:
 *   ./nb [all|plugins|test|e2e|bandwidth|install|install-sdk|info|clean]
 *        [-jN] [CC=cc] [PREFIX=/usr/local] [DESTDIR=...] [SECS=4]
 *
 * It rebuilds itself when build.c or nb.h change
 * NOTE: Linux (nolibc, static), macOS (system libc) and Windows (MinGW-w64,
 * system libc, Win32 API only) are all built from here: nothing in this file
 * needs make, sh, ls or install
 */

#include "nb.h"

// How plugins are compiled is shared with running app, which builds them on
// demand from picker (src/plugin_catalog.c)
#include "include/plugin_build.h"

#define BUILD "build"
#define OBJ BUILD "/obj"

#if defined(__APPLE__)
static const char *cc = "clang";
#else
static const char *cc = "gcc";
#endif
static int jobs = 4;
static const char *secs = "4"; // bandwidth: seconds per measurement


#if defined(_WIN32)
#define LINUX 0
#define WINDOWS 1
#define SO_EXT "dll"
#define PLATFORM_NAME "windows"
static const char *prefix = "dist";
#elif defined(__linux__)
#define LINUX 1
#define WINDOWS 0
#define SO_EXT "so"
#define PLATFORM_NAME "linux"
static const char *prefix = "/usr/local";
#else
#define LINUX 0
#define WINDOWS 0
#define SO_EXT "dylib"
#define PLATFORM_NAME "macos"
static const char *prefix = "/usr/local";
#endif
static const char *destdir = "";

static const char *core_src[] = {"src/app_paths.c",
                                 "src/ascii.c",
                                 "src/browser.c",
                                 "src/capture_demo.c",
                                 "src/capture_reinit.c",
                                 "src/curve.c",
                                 "src/curve_ui.c",
                                 "src/image.c",
                                 "src/jpeg.c",
                                 "src/slides.c",
                                 "src/netview.c",
                                 "src/cellcodec.c",
                                 "src/callproto.c",
                                 "src/room.c",
                                 "src/call.c",
                                 "src/call_ui.c",
                                 "src/capsrc.c",
                                 "src/speech.c",
                                 "src/nn.c",
                                 "src/safetensors.c",
                                 "src/asrnet.c",
                                 "src/asrcfg.c",
                                 "src/tokdec.c",
                                 "src/asr.c",
                                 "src/mic.c",
                                 "src/voice_cli.c",
                                 "src/main.c",
                                 "src/mouse.c",
                                 "src/netenc.c",
                                 "src/plugins.c",
                                 "src/plugin_catalog.c",
                                 "src/plugin_picker.c",
                                 "src/plugin_sandbox.c",
                                 "src/recorder.c",
                                 "src/thread_sharing.c",
                                 "src/timing.c",
                                 "src/vcam.c"};
static const char *lib_src[] = {
    "lib/nl_alloc.c",  "lib/nl_gc.c",  "lib/nl_dlfcn.c",  "lib/nl_errno.c",
    "lib/nl_getopt.c", "lib/nl_mem.c", "lib/nl_printf.c", "lib/nl_start.c"};
static const char *plugin_name[] = {
    "edge_detect",  "invert",      "threshold", "chromatic", "crt",
    "dither_bayer", "dither_fs",   "fisheye",   "framediff", "glitch",
    "kaleidoscope", "matrix_rain", "motion",    "neon_edge", "pixel_sort",
    "ripple",       "sepia",       "thermal",   "trails",    "tv_static",
    "vhs",          "vignette"};

#define COUNT(a) (sizeof(a) / sizeof(*a))

#if LINUX
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
#endif

// Flags every compile uses
static void cflags(nb_cmd *c) {
  nb_add_words(c, PB_COMMON);
  nb_add_words(c, PB_TARGET);
  nb_cmd_add(c, "-Iinclude", "-Ilib");
#if LINUX
  nb_cmd_add(c, "-isystem", cc_include_dir());
#elif WINDOWS
  // Media foundation capture + winsock
  nb_add_words(c, "-lmfplat -lmf -lmfreadwrite -lmfuuid -lole32 -loleaut32 "
                  "-luuid -lstrmiids -lwinpthread -lws2_32");
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

static int build_plugins(void);

#if LINUX
// Plugins linked into binary: pack build/*.so into build/ramfs.bin and assemble
// src/ramfs_blob.S around it (lib/nl_ramfs.h). Returns object to link, or NULL
// on failure
static const char *build_ramfs(void) {
  if (!build_plugins()) {
    return NULL;
  }

  const char *tool = BUILD "/mkramfs";
  const char *tool_deps[] = {"tools/mkramfs.c", "lib/nl_ramfs.h"};
  if (nb_needs_rebuild(tool, tool_deps, 2)) {
    // Build machine's compiler and libc, not app's freestanding flags
    nb_cmd c = {0};
    nb_cmd_add(&c, cc, "-O2", "tools/mkramfs.c", "-o", tool);
    if (!nb_run(&c)) {
      return NULL;
    }
  }

  const char *bin = BUILD "/ramfs.bin";
  const char *bin_deps[COUNT(plugin_name) + 1];
  bin_deps[0] = tool;
  for (size_t i = 0; i < COUNT(plugin_name); i++) {
    bin_deps[i + 1] = nb_fmt(BUILD "/%s." SO_EXT, plugin_name[i]);
  }

  if (nb_needs_rebuild(bin, bin_deps, COUNT(plugin_name) + 1)) {
    nb_cmd c = {0};
    nb_cmd_add(&c, tool, bin);
    for (size_t i = 0; i < COUNT(plugin_name); i++) {
      nb_push(&c, nb_fmt("plugins/%s." SO_EXT "=%s", plugin_name[i],
                         bin_deps[i + 1]));
    }

    if (!nb_run(&c)) {
      return NULL;
    }
  }

  // Assembler reads archive itself, so it is a dependency by hand
  const char *obj = OBJ "/ramfs_blob.o";
  const char *obj_deps[] = {"src/ramfs_blob.S", bin};
  if (nb_needs_rebuild(obj, obj_deps, 2)) {
    nb_cmd c = {0};
    nb_cmd_add(&c, cc, nb_fmt("-DNL_RAMFS_FILE=\"%s\"", bin), "-c",
               "src/ramfs_blob.S", "-o", obj);
    if (!nb_run(&c)) {
      return NULL;
    }
  }

  return obj;
}
#endif

static int build_all(void) {
  nb_mkdir(OBJ);
  nb_procs ps;
  nb_procs_init(&ps, jobs);

  // Every object depends on every header
  nb_cmd hl = {0};
  nb_glob(&hl, "include", ".h");
  nb_glob(&hl, "lib", ".h");
  const char **hdrs = hl.v;
  size_t nh = hl.n;

  const char *all[COUNT(core_src) + COUNT(lib_src) + 1];
  size_t na = 0;
  for (size_t i = 0; i < COUNT(core_src); i++) {
    all[na++] = core_src[i];
  }

#if !WINDOWS // Windows uses system libc
  for (size_t i = 0; i < COUNT(lib_src); i++) {
    all[na++] = lib_src[i];
  }
#endif

#if LINUX
  all[na++] = "src/capture_linux.c";
#elif WINDOWS
  all[na++] = "src/capture_windows.c";
#else
  all[na++] = "src/capture_macos.m";
#endif

  nb_cmd link = {0};
  nb_cmd_add(&link, cc);
  for (size_t i = 0; i < na; i++) {
    const char *o = obj_of(all[i]);
    nb_push(&link, o);
    const char **deps = (const char **)malloc((nh + 1) * sizeof *deps);
    size_t nd = 0;
    deps[nd++] = all[i];
    for (size_t h = 0; h < nh; h++) {
      deps[nd++] = hdrs[h];
    }

    int stale = nb_needs_rebuild(o, deps, nd);
    free((void *)deps);
    if (stale) {
      nb_cmd c = {0};
      nb_cmd_add(&c, cc);
      cflags(&c);
#if !LINUX && !WINDOWS
      const char *src = all[i];
      size_t sl = strlen(src);
      if (sl >= 2 && strcmp(src + sl - 2, ".m") == 0) {
        nb_cmd_add(&c, "-x", "objective-c", "-fobjc-arc", "-fmodules");
      }
#endif
      nb_cmd_add(&c, "-c", all[i], "-o", o);
      nb_procs_add(&ps, &c);
    }
  }

  if (!nb_procs_wait(&ps)) {
    return 0;
  }

#if LINUX
  const char *blob = build_ramfs();
  if (!blob) {
    return 0;
  }

  nb_push(&link, blob);
#endif

  const char *target = BUILD "/asciicam" NB_EXE;
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
#if !LINUX && !WINDOWS
    nb_cmd_add(&c, "-undefined", "dynamic_lookup");
#endif
    nb_cmd_add(&c, src, "-o", tmp);
    if (!nb_run(&c) || !nb_rename(tmp, out)) {
      return 0;
    }
  }

  return 1;
}

// Copy every file of dir ending in suffix (NULL = all) into dest
static int install_files(const char *dir, const char *suffix, const char *dest,
                         const char *mode) {
  nb_cmd f = {0};
  nb_glob(&f, dir, suffix);
  int ok = 1;
  for (size_t i = 0; i < f.n; i++) {
    ok &= nb_copy(f.v[i], dest, mode);
  }

  return ok;
}

static int install(int sdk) {
  const char *share = nb_fmt("%s%s/share/asciicam", destdir, prefix);
  const char *bin = nb_fmt("%s%s/bin", destdir, prefix);
  nb_mkdir(bin);
  nb_mkdir(nb_fmt("%s/plugins", share));
  nb_mkdir(nb_fmt("%s/charsets", share));
  int ok = nb_copy(BUILD "/asciicam" NB_EXE, nb_fmt("%s/asciicam" NB_EXE, bin),
                   "755");
  for (size_t i = 0; i < COUNT(plugin_name); i++) {
    ok &= nb_copy(nb_fmt(BUILD "/%s." SO_EXT, plugin_name[i]),
                  nb_fmt("%s/plugins", share), "755");
  }

  ok &= install_files("charsets", NULL, nb_fmt("%s/charsets", share), "644");
  if (sdk) {
    // Plugin build needs: headers and plugin sources
    // No Makefile: app runs compiler itself
    const char *d = nb_fmt("%s/sdk", share);
    nb_mkdir(nb_fmt("%s/include", d));
    nb_mkdir(nb_fmt("%s/lib", d));
    nb_mkdir(nb_fmt("%s/filters", d));
    ok &= install_files("include", ".h", nb_fmt("%s/include", d), "644");
    ok &= install_files("lib", ".h", nb_fmt("%s/lib", d), "644");
    ok &= install_files("filters", ".c", nb_fmt("%s/filters", d), "644");
  }

  return ok;
}

#if LINUX
// Test programs: each is built from its own sources plus small nolibc core and
// run
// Order and flags `make test` follow
typedef struct {
  const char *name;
  const char *opt;
  const char *extra;
  const char *srcs;
  const char *args[3]; // Each non-NULL entry is one run, e.g. "100000 1"
} nb_test;

static const char *test_libs[] = {"lib/nl_mem.c",   "lib/nl_printf.c",
                                  "lib/nl_errno.c", "lib/nl_alloc.c",
                                  "lib/nl_start.c", NULL};

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
      {"dlopen_mem",
       "-O2",
       "",
       "tests/test_dlopen_mem.c lib/nl_dlfcn.c",
       {"build/invert.so"}},
      {"fuzz",
       "-O0",
       "",
       "tests/fuzz_nolibc.c lib/nl_getopt.c",
       {"100000 1", "100000 2", "100000 3"}},
      {"slice_O0", "-O0", "", "tests/test_nl_slice.c", {NULL}},
      {"slice_O2", "-O2", "", "tests/test_nl_slice.c", {NULL}},
      {"defer_O0", "-O0", "", "tests/test_nl_defer.c", {NULL}},
      {"defer_O2", "-O2", "", "tests/test_nl_defer.c", {NULL}},
      {"hash", "-O0", "", "tests/test_nl_hash.c", {NULL}},
      {"hash_O2", "-O2", "", "tests/test_nl_hash.c", {NULL}},
      {"ramfs_O0", "-O0", "", "tests/test_nl_ramfs.c", {NULL}},
      {"ramfs_O2", "-O2", "", "tests/test_nl_ramfs.c", {NULL}},
      {"fuzz_elf",
       "-O2",
       "",
       "tests/fuzz_elf.c lib/nl_dlfcn.c",
       {"build/edge_detect.so 2000 1 build/tests/fuzz_elf.tmp",
        "build/invert.so 2000 1 build/tests/fuzz_elf.tmp",
        "build/threshold.so 2000 1 build/tests/fuzz_elf.tmp"}},
      {"mouse", "-O0", "", "tests/test_mouse.c src/mouse.c", {NULL}},
      {"demo", "-O2", "", "tests/test_demo.c src/capture_demo.c", {NULL}},
      {"image_O0",
       "-O0",
       "",
       "tests/test_image.c src/image.c src/jpeg.c",
       {NULL}},
      {"image_O2",
       "-O2",
       "",
       "tests/test_image.c src/image.c src/jpeg.c",
       {NULL}},
      {"jpeg_O0",
       "-O0",
       "",
       "tests/test_jpeg.c src/image.c src/jpeg.c",
       {NULL}},
      {"jpeg_O2",
       "-O2",
       "",
       "tests/test_jpeg.c src/image.c src/jpeg.c",
       {NULL}},
      {"yuv_simd_O0",
       "-O0",
       "",
       "tests/test_yuv_simd.c src/ascii.c lib/nl_gc.c",
       {NULL}},
      {"yuv_simd_O2",
       "-O2",
       "",
       "tests/test_yuv_simd.c src/ascii.c lib/nl_gc.c",
       {NULL}},
      {"vcam_O0", "-O0", "", "tests/test_vcam.c src/vcam.c", {NULL}},
      {"vcam_O2", "-O2", "", "tests/test_vcam.c src/vcam.c", {NULL}},
      {"recorder_O0",
       "-O0",
       "",
       "tests/test_recorder.c src/recorder.c",
       {NULL}},
      {"recorder_O2",
       "-O2",
       "",
       "tests/test_recorder.c src/recorder.c",
       {NULL}},
      {"browser_O0",
       "-O0",
       "",
       "tests/test_browser.c src/browser.c src/recorder.c",
       {NULL}},
      {"browser_O2",
       "-O2",
       "",
       "tests/test_browser.c src/browser.c src/recorder.c",
       {NULL}},
      {"curve_O0", "-O0", "", "tests/test_curve.c src/curve.c", {NULL}},
      {"curve_O2", "-O2", "", "tests/test_curve.c src/curve.c", {NULL}},
      {"curve_ui_O0",
       "-O0",
       "",
       "tests/test_curve_ui.c src/curve_ui.c src/curve.c src/browser.c "
       "src/recorder.c",
       {NULL}},
      {"curve_ui_O2",
       "-O2",
       "",
       "tests/test_curve_ui.c src/curve_ui.c src/curve.c src/browser.c "
       "src/recorder.c",
       {NULL}},
      {"tone_O0",
       "-O0",
       "",
       "tests/test_tone.c src/ascii.c lib/nl_gc.c",
       {NULL}},
      {"tone_O2",
       "-O2",
       "",
       "tests/test_tone.c src/ascii.c lib/nl_gc.c",
       {NULL}},
      {"colour_O0",
       "-O0",
       "",
       "tests/test_colour.c src/ascii.c lib/nl_gc.c",
       {NULL}},
      {"colour_O2",
       "-O2",
       "",
       "tests/test_colour.c src/ascii.c lib/nl_gc.c",
       {NULL}},
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
      {"plugin_embedded",
       "-O2",
       "",
       "tests/test_plugin_embedded.c src/plugin_catalog.c src/plugin_picker.c "
       "src/plugins.c lib/nl_dlfcn.c",
       {"build/invert.so"}},
      {"copy_file", "-O2", "", "tests/test_copy_file.c lib/nl_dlfcn.c", {NULL}},
      {"gc_O0", "-O0", "", "tests/test_nl_gc.c lib/nl_gc.c", {NULL}},
      {"gc_O2", "-O2", "", "tests/test_nl_gc.c lib/nl_gc.c", {NULL}},
      {"thread", "-O2", "", "tests/test_nl_thread.c", {NULL}},
      {"seccomp_O0", "-O0", "", "tests/test_nl_seccomp.c", {NULL}},
      {"seccomp_O2", "-O2", "", "tests/test_nl_seccomp.c", {NULL}},
      {"plugin_sandbox_O0",
       "-O0",
       "",
       "tests/test_plugin_sandbox.c src/plugin_sandbox.c src/plugins.c "
       "lib/nl_dlfcn.c",
       {NULL}},
      {"plugin_sandbox_O2",
       "-O2",
       "",
       "tests/test_plugin_sandbox.c src/plugin_sandbox.c src/plugins.c "
       "lib/nl_dlfcn.c",
       {NULL}},
      {"capture_pipe",
       "-O2",
       "",
       "tests/test_capture_pipe.c src/thread_sharing.c",
       {NULL}},
      {"effects_O0", "-O0", "-DTESTING", "tests/test_effects.c", {NULL}},
      {"effects_O2", "-O2", "-DTESTING", "tests/test_effects.c", {NULL}},
      {"netenc_O0",
       "-O0",
       "",
       "tests/test_netenc.c src/netenc.c src/ascii.c lib/nl_gc.c",
       {NULL}},
      {"netenc_O2",
       "-O2",
       "",
       "tests/test_netenc.c src/netenc.c src/ascii.c lib/nl_gc.c",
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

  // Second program: real binary, for checks that run it with --demo
  if (!build_all()) {
    return 0;
  }

  nb_cmd py = {0};
  nb_cmd_add(&py, "python3", "tests/e2e_capture.py", bin, BUILD "/asciicam");

  return nb_run(&py);
}
#else
static int run_tests(void) {
  nb_log("tests are Linux only");

  return 1;
}

static int run_e2e(void) {
  nb_log("e2e is Linux only");

  return 1;
}
#endif

#if !WINDOWS
// Bytes per frame with and without --net (README table)
// A host program that drives real binary through a pseudo-terminal
static int run_bandwidth(void) {
  if (!build_all()) {
    return 0;
  }

  nb_mkdir(BUILD "/tests");
  const char *bin = BUILD "/tests/measure_bandwidth";
  nb_cmd c = {0};
  nb_cmd_add(&c, cc, "-O1", "-Wall", "-Wextra", "-o", bin,
             "tests/measure_bandwidth.c");
  if (!nb_run(&c)) {
    return 0;
  }

  nb_cmd r = {0};
  nb_cmd_add(&r, bin, BUILD "/asciicam", secs);

  return nb_run(&r);
}
#else
static int run_bandwidth(void) {
  nb_log("bandwidth runs the program in a pseudo-terminal: not on Windows");

  return 1;
}
#endif


static void info(void) {
  printf("Platform : %s\n", PLATFORM_NAME);
  printf("CC       : %s\n", cc);
  printf("Prefix   : %s%s\n", destdir, prefix);
  printf("Plugins  : %s %s\n", PB_COMMON, PB_TARGET);
  printf("Shared   : %s\n", PB_SHARED);
}

int main(int argc, char **argv) {
  // Windows MinGW-w64 gcc
  NB_REBUILD_SELF(argc, argv, WINDOWS ? "gcc" : "cc");
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
    } else if (!strncmp(argv[i], "SECS=", 5)) {
      secs = argv[i] + 5;
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
    ok = build_all() && run_tests(); // Tests read build/asciicam
  } else if (!strcmp(target, "e2e")) {
    ok = run_e2e();
  } else if (!strcmp(target, "bandwidth")) {
    ok = run_bandwidth();
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
}
