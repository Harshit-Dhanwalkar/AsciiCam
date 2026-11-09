/*
 * tests/test_effects.c - effect plugins in filters/ and include/fx.h
 *
 * Plugin sources are included directly (with TESTING defined, so they
 * leave out plugin_get) and their process functions called with hand made
 * frames. Every plugin gets same battery:
 *   - bad frames (NULL, zero/negative/huge sizes) are left untouched
 *   - guard bytes either side of frame survive every size x parameter
 *   - parameter 0 changes nothing (all but dithers, which always quantise)
 *   - at default parameter a textured frame really is changed
 * followed by checks of what each effect is supposed to do
 *
 * Build (see Makefile / build.c): nolibc flags, -DTESTING, -Ifilters not needed
 */

#include "../lib/nolibc.h"

#include "../filters/chromatic.c"
#include "../filters/crt.c"
#include "../filters/dither_bayer.c"
#include "../filters/dither_fs.c"
#include "../filters/fisheye.c"
#include "../filters/framediff.c"
#include "../filters/glitch.c"
#include "../filters/kaleidoscope.c"
#include "../filters/matrix_rain.c"
#include "../filters/neon_edge.c"
#include "../filters/pixel_sort.c"
#include "../filters/ripple.c"
#include "../filters/sepia.c"
#include "../filters/thermal.c"
#include "../filters/trails.c"
#include "../filters/tv_static.c"
#include "../filters/vhs.c"
#include "../filters/vignette.c"

static int _pass = 0;
static int _fail = 0;

static void check(int ok, const char *what, const char *name, int line) {
  if (ok) {
    _pass++;

    return;
  }

  _fail++;
  char buf[256];
  int n = nl_snprintf(buf, sizeof(buf), "FAIL [%d] %s: %s\n", line, name, what);
  nl_write(1, buf, (size_t)n);
}

#define CHECK(cond, what, name) check((cond) != 0, (what), (name), __LINE__)

static int nl_memcmp_(const void *a, const void *b, size_t n) {
  const uint8_t *x = a;
  const uint8_t *y = b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) {
      return x[i] < y[i] ? -1 : 1;
    }
  }

  return 0;
}

typedef void (*fx_fn)(uint8_t *, int, int, void *);

typedef struct {
  const char *name;
  fx_fn fn;
  int identity_at_zero; // dithers always quantise, so they are exempt
} effect_t;

static const effect_t effects[] = {
    {"chromatic", chromatic_process, 1},
    {"crt", crt_process, 1},
    {"dither_bayer", dither_bayer_process, 0},
    {"dither_fs", dither_fs_process, 0},
    {"fisheye", fisheye_process, 1},
    {"framediff", framediff_process, 1},
    {"glitch", glitch_process, 1},
    {"kaleidoscope", kaleidoscope_process, 1},
    {"matrix_rain", matrix_rain_process, 1},
    {"neon_edge", neon_edge_process, 1},
    {"pixel_sort", pixel_sort_process, 1},
    {"ripple", ripple_process, 1},
    {"sepia", sepia_process, 1},
    {"thermal", thermal_process, 1},
    {"trails", trails_process, 1},
    {"tv_static", tv_static_process, 1},
    {"vhs", vhs_process, 1},
    {"vignette", vignette_process, 1},
};

#define NEFFECTS ((int)(sizeof(effects) / sizeof(effects[0])))

// A frame with structure at several scales: rings, a gradient and texture
static void make_image(uint8_t *g, int w, int h) {
  uint32_t s = 12345;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int dx = x - w / 2;
      int dy = y - h / 2;
      int ring = fx_sin(fx_isqrt(dx * dx + dy * dy) * 1024 / 24) / 4;
      int grad = x * 120 / (w > 1 ? w - 1 : 1) + y * 60 / (h > 1 ? h - 1 : 1);
      int tex = fx_rand_n(&s, 24);
      g[y * w + x] = fx_u8(60 + grad + ring + tex);
    }
  }
}

static int differs(const uint8_t *a, const uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i]) {
      return 1;
    }
  }

  return 0;
}

#define GUARD 64

// Run one process call on a frame between guard bands; returns 1 when guards
// survived
static int guarded_run(fx_fn fn, int w, int h, int param, int frames,
                       int image) {
  size_t n = (size_t)w * (size_t)h;
  uint8_t *mem = nl_malloc(n + 2 * GUARD);
  if (!mem) {
    return 0;
  }

  nl_memset(mem, 0xA5, n + 2 * GUARD);
  uint8_t *g = mem + GUARD;
  int ok = 1;
  for (int f = 0; f < frames; f++) {
    if (image) {
      make_image(g, w, h);
    } else {
      nl_memset(g, (int)(f * 37 % 256), n);
    }

    int p = param;
    fn(g, w, h, &p);
  }

  for (int i = 0; i < GUARD; i++) {
    if (mem[i] != 0xA5 || mem[GUARD + n + (size_t)i] != 0xA5) {
      ok = 0;
    }
  }

  nl_free(mem);

  return ok;
}

static void test_bad_frames(const effect_t *e) {
  uint8_t canary[64];
  struct {
    int w, h;
  } bad[] = {{0, 0},          {-1, 48},
             {64, -1},        {65536, 65536},
             {2147483647, 2}, {2, 2147483647},
             {8193, 1},       {1, 8193},
             {1921, 1081},    {-2147483647 - 1, 4}};

  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    nl_memset(canary, 0x5A, sizeof(canary));
    int p = 128;
    e->fn(canary, bad[i].w, bad[i].h, &p);
    int same = 1;
    for (size_t k = 0; k < sizeof(canary); k++) {
      if (canary[k] != 0x5A) {
        same = 0;
      }
    }

    CHECK(same, "bad frame size leaves the buffer alone", e->name);
  }

  e->fn(NULL, 64, 48, NULL); // must simply return
  CHECK(1, "NULL frame returns", e->name);
}

static void test_guards(const effect_t *e) {
  static const int sizes[][2] = {{1, 1},    {2, 2},     {3, 3},
                                 {1, 300},  {300, 1},   {64, 48},
                                 {161, 97}, {640, 480}, {1920, 1080}};
  static const int params[] = {0, 1, 64, 127, 128, 129, 200, 254, 255};

  for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
    int w = sizes[s][0];
    int h = sizes[s][1];
    int big = (size_t)w * (size_t)h > 100000;
    for (size_t k = 0; k < sizeof(params) / sizeof(params[0]); k++) {
      if (big && params[k] != 1 && params[k] != 255 && params[k] != 128) {
        continue; // keep big frames to a few parameters
      }

      CHECK(guarded_run(e->fn, w, h, params[k], big ? 3 : 6, s % 2 == 0),
            "guard bytes intact", e->name);
    }
  }

  // parameter out of range is clamped, not trusted
  int w = 64;
  int h = 48;
  uint8_t g[64 * 48];
  make_image(g, w, h);
  int huge = 1000000;
  e->fn(g, w, h, &huge);
  int neg = -5;
  e->fn(g, w, h, &neg);
  CHECK(1, "out of range parameters survive", e->name);
  make_image(g, w, h);
  e->fn(g, w, h, NULL);
  CHECK(1, "missing parameter uses the default", e->name);
}

static void test_zero_and_default(const effect_t *e) {
  int w = 160;
  int h = 120;
  size_t n = (size_t)w * (size_t)h;
  uint8_t *src = nl_malloc(n);
  uint8_t *out = nl_malloc(n);
  if (!src || !out) {
    CHECK(0, "allocation", e->name);
    nl_free(src);
    nl_free(out);

    return;
  }

  if (e->identity_at_zero) {
    int identical = 1;
    for (int f = 0; f < 20; f++) {
      make_image(src, w, h);
      nl_memcpy(out, src, n);
      int p = 0;
      e->fn(out, w, h, &p);
      if (differs(src, out, n)) {
        identical = 0;
      }
    }

    CHECK(identical, "parameter 0 leaves every frame unchanged", e->name);
  }

  int changed = 0;
  for (int f = 0; f < 40 && !changed; f++) {
    make_image(src, w, h);
    if (f % 2 == 1) {
      // alternate with negative so temporal effects (trails, framediff) have
      // something to react to
      for (size_t i = 0; i < n; i++) {
        src[i] = (uint8_t)(255 - src[i]);
      }
    }

    nl_memcpy(out, src, n);
    int p = 128;
    e->fn(out, w, h, &p);
    changed = differs(src, out, n);
  }

  CHECK(changed, "default parameter visibly changes a textured frame", e->name);

  nl_free(src);
  nl_free(out);
}

// effect-specific behaviour

static const effect_t *find(const char *name) {
  for (int i = 0; i < NEFFECTS; i++) {
    if (nl_strcmp(effects[i].name, name) == 0) {
      return &effects[i];
    }
  }

  return NULL;
}

static void run_param(const char *name, uint8_t *g, int w, int h, int p) {
  find(name)->fn(g, w, h, &p);
}

static void test_fx_helpers(void) {
  const char *n = "fx.h";
  // sine: quarter points and symmetry; reference values from real sine
  CHECK(fx_sin(0) == 0, "sin 0", n);
  CHECK(fx_sin(256) == 256, "sin quarter turn", n);
  CHECK(fx_sin(512) == 0, "sin half turn", n);
  CHECK(fx_sin(768) == -256, "sin three quarters", n);
  CHECK(fx_sin(1024) == fx_sin(0) && fx_sin(-256) == -256, "sin periodic", n);
  static const int ref[] = {0,   50,  98,  142, 181, 213, 237, 251, 256,
                            251, 237, 213, 181, 142, 98,  50}; // p = 0, 32, ...
  int worst = 0;
  for (int i = 0; i < 16; i++) {
    int d = fx_abs(fx_sin(i * 32) - ref[i]);
    if (d > worst) {
      worst = d;
    }
  }

  CHECK(worst <= 4, "sin within 4/256 of the real thing", n);
  int sym = 1;
  for (int p = 0; p < 1024; p++) {
    if (fx_sin(p) != -fx_sin(p + 512)) {
      sym = 0;
    }
  }

  CHECK(sym, "sin(p + half turn) == -sin(p)", n);

  int exact = 1;
  for (int v = 0; v < 70000; v++) {
    int r = fx_isqrt(v);
    if (r * r > v || (r + 1) * (r + 1) <= v) {
      exact = 0;
    }
  }

  CHECK(exact, "isqrt exact for 0..69999", n);
  CHECK(fx_isqrt(2147483647) == 46340, "isqrt INT_MAX", n);
  CHECK(fx_isqrt(-5) == 0, "isqrt of negative is 0", n);

  uint32_t s = 1;
  int nonzero = 1;
  for (int i = 0; i < 100000; i++) {
    if (fx_rand(&s) == 0) {
      nonzero = 0;
    }
  }

  CHECK(nonzero, "xorshift never hits 0", n);
  uint32_t zero = 0;
  CHECK(fx_rand(&zero) != 0, "xorshift recovers from a 0 seed", n);
  int p = 300;
  int q = -4;
  CHECK(fx_param(&p, 7) == 255 && fx_param(&q, 7) == 0, "fx_param clamps", n);
  CHECK(fx_param(NULL, 7) == 7, "fx_param default", n);
  CHECK(fx_ok(NULL, 4, 4) == 0, "fx_ok rejects NULL", n);
  uint8_t b[4];
  CHECK(fx_ok(b, 1920, 1080) == 1 && fx_ok(b, 1921, 1080) == 0,
        "fx_ok pixel limit", n);
  CHECK(fx_mix(0, 255, 0) == 0 && fx_mix(0, 255, 255) == 255 &&
            fx_mix(100, 200, 255) == 200,
        "fx_mix end points", n);
}

static void test_vignette(void) {
  uint8_t g[200 * 100];
  nl_memset(g, 200, sizeof(g));
  run_param("vignette", g, 200, 100, 255);
  CHECK(g[50 * 200 + 100] >= 195, "centre stays bright", "vignette");
  CHECK(g[0] < 100 && g[199] < 100 && g[99 * 200] < 100 &&
            g[99 * 200 + 199] < 100,
        "corners are dark", "vignette");
  CHECK(g[0] < g[50 * 200 + 20], "darker toward the corner", "vignette");
}

static void test_chromatic(void) {
  uint8_t g[120 * 60];
  nl_memset(g, 90, sizeof(g));
  run_param("chromatic", g, 120, 60, 255);
  int flat = 1;
  for (size_t i = 0; i < sizeof(g); i++) {
    if (g[i] != 90) {
      flat = 0;
    }
  }

  CHECK(flat, "a flat picture has no edges to fringe", "chromatic");

  // a vertical edge grows a bright fringe
  for (int y = 0; y < 60; y++) {
    for (int x = 0; x < 120; x++) {
      g[y * 120 + x] = x < 60 ? 40 : 160;
    }
  }

  run_param("chromatic", g, 120, 60, 200);
  int bright = 0;
  for (int x = 50; x < 70; x++) {
    if (g[30 * 120 + x] > 160) {
      bright = 1;
    }
  }

  CHECK(bright, "an edge gets a fringe brighter than either side", "chromatic");
}

static void test_fisheye_ripple(void) {
  uint8_t g[160 * 120];
  const char *names[] = {"fisheye", "ripple", "kaleidoscope"};
  for (int e = 0; e < 3; e++) {
    nl_memset(g, 123, sizeof(g));
    run_param(names[e], g, 160, 120, 200);
    int flat = 1;
    for (size_t i = 0; i < sizeof(g); i++) {
      if (g[i] != 123) {
        flat = 0;
      }
    }

    CHECK(flat, "warping a flat picture gives a flat picture", names[e]);
  }

  make_image(g, 160, 120);
  uint8_t centre = g[60 * 160 + 80];
  run_param("fisheye", g, 160, 120, 255);
  CHECK(g[60 * 160 + 80] == centre, "fisheye keeps the middle pixel",
        "fisheye");
  make_image(g, 160, 120);
  centre = g[60 * 160 + 80];
  run_param("ripple", g, 160, 120, 255);
  CHECK(g[60 * 160 + 80] == centre, "ripple keeps the middle pixel", "ripple");
}

static void test_kaleidoscope(void) {
  uint8_t g[160 * 120];
  make_image(g, 160, 120);
  run_param("kaleidoscope", g, 160, 120, 128); // four-fold
  int mirror = 1;
  for (int y = 0; y < 120; y++) {
    for (int x = 1; x <= 80; x++) {
      if (g[y * 160 + x] != g[y * 160 + (160 - x)]) {
        mirror = 0;
      }
    }
  }

  CHECK(mirror, "four-fold fold is left/right symmetric", "kaleidoscope");
  int vmirror = 1;
  for (int y = 1; y <= 60; y++) {
    for (int x = 0; x < 160; x++) {
      if (g[y * 160 + x] != g[(120 - y) * 160 + x]) {
        vmirror = 0;
      }
    }
  }

  CHECK(vmirror, "four-fold fold is top/bottom symmetric", "kaleidoscope");
  make_image(g, 160, 120);
  run_param("kaleidoscope", g, 160, 120, 255); // eight-fold
  int diag = 1;
  for (int y = 0; y < 120; y++) {
    for (int x = 0; x < 160; x++) {
      int dx = x - 80 < 0 ? 80 - x : x - 80;
      int dy = y - 60 < 0 ? 60 - y : y - 60;
      int mx = 80 + dy;
      int my = 60 + dx;
      if (mx < 160 && my < 120 && dx != dy &&
          g[y * 160 + x] != g[my * 160 + mx]) {
        diag = 0;
      }
    }
  }

  CHECK(diag, "eight-fold fold is symmetric about the diagonal",
        "kaleidoscope");
}

static void test_dither(void) {
  static const char *names[] = {"dither_bayer", "dither_fs"};
  for (int e = 0; e < 2; e++) {
    uint8_t g[320 * 240];
    for (int y = 0; y < 240; y++) {
      for (int x = 0; x < 320; x++) {
        g[y * 320 + x] = (uint8_t)(x * 255 / 319);
      }
    }

    run_param(names[e], g, 320, 240, 0); // 2 levels
    int two = 1;
    long sum = 0;
    for (size_t i = 0; i < sizeof(g); i++) {
      if (g[i] != 0 && g[i] != 255) {
        two = 0;
      }

      sum += g[i];
    }

    CHECK(two, "two levels output only 0 and 255", names[e]);
    long mean = sum / (long)sizeof(g);
    CHECK(mean > 118 && mean < 138, "average brightness is preserved",
          names[e]);

    for (int y = 0; y < 240; y++) {
      for (int x = 0; x < 320; x++) {
        g[y * 320 + x] = (uint8_t)(x * 255 / 319);
      }
    }

    run_param(names[e], g, 320, 240, 128); // 10 levels, step 28
    int on_grid = 1;
    for (size_t i = 0; i < sizeof(g); i++) {
      if (g[i] % 28 != 0) {
        on_grid = 0;
      }
    }

    CHECK(on_grid, "ten levels sit on the 28 step grid", names[e]);
  }
}

static void test_trails_framediff(void) {
  uint8_t g[64 * 48];
  // a different size first, so history starts over
  uint8_t tiny[4] = {0, 0, 0, 0};
  run_param("trails", tiny, 2, 2, 128);
  nl_memset(g, 200, sizeof(g));
  run_param("trails", g, 64, 48, 128);
  CHECK(g[0] == 200, "first frame passes through", "trails");
  nl_memset(g, 0, sizeof(g));
  run_param("trails", g, 64, 48, 128);
  int keep = 255 - (255 - 128) * (255 - 128) / 255;
  CHECK(g[100] == 200 * keep / 256, "a vanished subject fades, not vanishes",
        "trails");
  nl_memset(g, 0, sizeof(g));
  run_param("trails", g, 64, 48, 128);
  CHECK(g[100] < 200 * keep / 256 && g[100] > 0, "and keeps fading", "trails");
  nl_memset(g, 10, sizeof(g));
  run_param("trails", g, 64, 48, 0);
  nl_memset(g, 0, sizeof(g));
  run_param("trails", g, 64, 48, 0);
  CHECK(g[5] == 0, "parameter 0 keeps no history", "trails");

  uint8_t t2[9] = {0};
  run_param("framediff", t2, 3, 3, 128);
  nl_memset(g, 120, sizeof(g));
  run_param("framediff", g, 64, 48, 128);
  uint8_t still1 = g[0];
  nl_memset(g, 120, sizeof(g));
  run_param("framediff", g, 64, 48, 128);
  CHECK(g[0] == g[2000] && g[0] < 120,
        "identical frames fade the still background", "framediff");
  (void)still1;
  nl_memset(g, 120, sizeof(g));
  g[24 * 64 + 32] = 250; // something moved
  run_param("framediff", g, 64, 48, 128);
  CHECK(g[24 * 64 + 32] > g[24 * 64 + 10] + 100, "motion stands out",
        "framediff");
}

static void test_noise_effects(void) {
  uint8_t g[200 * 100];
  nl_memset(g, 100, sizeof(g));
  run_param("tv_static", g, 200, 100, 255);
  long sum = 0;
  int unchanged = 0;
  for (size_t i = 0; i < sizeof(g); i++) {
    sum += g[i];
    if (g[i] == 100) {
      unchanged++;
    }
  }

  long mean = sum / (long)sizeof(g);
  CHECK(mean > 108 && mean < 148, "full static averages mid grey", "tv_static");
  CHECK(unchanged < (int)sizeof(g) / 50, "full static replaces the picture",
        "tv_static");

  nl_memset(g, 100, sizeof(g));
  run_param("sepia", g, 200, 100, 255);
  int lo = 255;
  int hi = 0;
  for (size_t i = 0; i < sizeof(g); i++) {
    lo = g[i] < lo ? g[i] : lo;
    hi = g[i] > hi ? g[i] : hi;
  }

  CHECK(lo >= 30 && hi <= 220, "sepia grain stays near the faded tone",
        "sepia");
  for (int i = 0; i < 256; i++) {
    g[i] = (uint8_t)i;
  }

  run_param("sepia", g, 256, 1, 255);
  CHECK(g[0] >= 14 && g[255] <= 236, "sepia lifts blacks and softens whites",
        "sepia");
}

static void test_sort_and_glitch(void) {
  uint8_t g[64];
  for (int i = 0; i < 64; i++) {
    g[i] = (uint8_t)(255 - i * 3); // descending, all bright
  }

  run_param("pixel_sort", g, 64, 1, 255);
  int ascending = 1;
  for (int i = 1; i < 64; i++) {
    if (g[i - 1] > g[i]) {
      ascending = 0;
    }
  }

  CHECK(ascending, "a bright run is sorted dark to bright", "pixel_sort");

  uint8_t big[300 * 20];
  make_image(big, 300, 20);
  long before = 0;
  for (size_t i = 0; i < sizeof(big); i++) {
    before += big[i];
  }

  run_param("pixel_sort", big, 300, 20, 200);
  long after = 0;
  for (size_t i = 0; i < sizeof(big); i++) {
    after += big[i];
  }

  CHECK(before == after, "sorting only reorders pixels", "pixel_sort");

  // glitch: quiet at 0, active at 255
  uint8_t f[160 * 120];
  int changed0 = 0;
  int changed255 = 0;
  for (int i = 0; i < 60; i++) {
    uint8_t ref[160 * 120];
    make_image(f, 160, 120);
    nl_memcpy(ref, f, sizeof(f));
    run_param("glitch", f, 160, 120, 0);
    changed0 |= differs(f, ref, sizeof(f));
  }

  for (int i = 0; i < 60; i++) {
    uint8_t ref[160 * 120];
    make_image(f, 160, 120);
    nl_memcpy(ref, f, sizeof(f));
    run_param("glitch", f, 160, 120, 255);
    changed255 |= differs(f, ref, sizeof(f));
  }

  CHECK(!changed0, "glitch at 0 never fires", "glitch");
  CHECK(changed255, "glitch at 255 fires", "glitch");
}

static void test_rain_crt_neon(void) {
  uint8_t g[320 * 240];
  nl_memset(g, 0, sizeof(g));
  int bright = 0;
  for (int i = 0; i < 80; i++) {
    run_param("matrix_rain", g, 320, 240, 255);
  }

  for (size_t i = 0; i < sizeof(g); i++) {
    if (g[i] > 200) {
      bright = 1;
    }
  }

  CHECK(bright, "rain draws bright heads over a black picture", "matrix_rain");
  nl_memset(g, 200, sizeof(g));
  run_param("matrix_rain", g, 320, 240, 255);
  CHECK(g[0] <= 200 || g[1] <= 200, "rain darkens the picture behind it",
        "matrix_rain");

  nl_memset(g, 200, sizeof(g));
  run_param("crt", g, 320, 240, 255);
  CHECK(g[0] == 0 && g[319] == 0 && g[239 * 320] == 0,
        "crt corners fall off the curved screen", "crt");
  CHECK(g[120 * 320 + 160] > 100, "crt centre stays lit", "crt");
  int dark_rows = 0;
  int lit_rows = 0;
  for (int y = 100; y < 140; y++) {
    if (g[y * 320 + 160] < 150) {
      dark_rows++;
    } else {
      lit_rows++;
    }
  }

  CHECK(dark_rows > 5 && lit_rows > 5, "crt has scanlines", "crt");

  nl_memset(g, 0, sizeof(g));
  for (int y = 80; y < 160; y++) {
    for (int x = 100; x < 220; x++) {
      g[y * 320 + x] = 220;
    }
  }

  run_param("neon_edge", g, 320, 240, 255);
  int edge = g[120 * 320 + 100];
  int inside = g[120 * 320 + 160];
  int outside = g[120 * 320 + 20];
  CHECK(edge > inside + 50 && edge > outside + 50,
        "neon_edge lights the outline, not the fill", "neon_edge");
}

// Colour hooks (plugin_rgb_process): guards, parameter 0, and what each does
typedef void (*rgb_fn)(uint8_t *, int, int, void *);

static void test_rgb_hooks(void) {
  static const struct {
    const char *name;
    rgb_fn fn;
  } hooks[] = {{"thermal_rgb", thermal_rgb},
               {"sepia_rgb", sepia_rgb},
               {"chromatic_rgb", chromatic_rgb}};

  for (size_t k = 0; k < sizeof(hooks) / sizeof(hooks[0]); k++) {
    const char *name = hooks[k].name;

    // bad frames are left alone
    uint8_t px[12] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 1, 2, 3};
    uint8_t keep[12];
    nl_memcpy(keep, px, sizeof(px));
    hooks[k].fn(NULL, 2, 2, NULL);
    hooks[k].fn(px, 0, 2, NULL);
    hooks[k].fn(px, 2, -1, NULL);
    hooks[k].fn(px, FX_MAX_SIDE + 1, 1, NULL);
    CHECK(nl_memcmp_(px, keep, sizeof(px)) == 0, "bad frames untouched", name);

    // guard bytes around a w x h x 3 picture survive; parameter 0 is "off"
    for (int w = 1; w <= 33; w += 8) {
      for (int h = 1; h <= 9; h += 4) {
        size_t n = (size_t)w * (size_t)h * 3u;
        uint8_t buf[16 + 33 * 9 * 3 + 16];
        nl_memset(buf, 0xA5, sizeof(buf));
        uint32_t seed = 77u;
        for (size_t i = 0; i < n; i++) {
          buf[16 + i] = (uint8_t)fx_rand(&seed);
        }

        uint8_t orig[33 * 9 * 3];
        nl_memcpy(orig, buf + 16, n);
        for (int param = 0; param <= 255; param += 85) {
          nl_memcpy(buf + 16, orig, n);
          hooks[k].fn(buf + 16, w, h, &param);
          int guards = 1;
          for (int i = 0; i < 16; i++) {
            if (buf[i] != 0xA5 || buf[16 + n + (size_t)i] != 0xA5) {
              guards = 0;
            }
          }

          CHECK(guards, "guard bytes survive", name);
          if (param == 0) {
            CHECK(nl_memcmp_(buf + 16, orig, n) == 0, "parameter 0 is off",
                  name);
          }
        }
      }
    }
  }

  // sepia: at full strength every pixel is warm (R >= G >= B), grey stays
  // brown rather than neutral
  {
    uint8_t p[6] = {120, 120, 120, 30, 90, 200};
    int full = 255;
    sepia_rgb(p, 2, 1, &full);
    CHECK(p[0] >= p[1] && p[1] >= p[2], "sepia: grey becomes warm",
          "sepia_rgb");
    CHECK(p[0] > p[2] + 20, "sepia: tint is visible", "sepia_rgb");
    CHECK(p[3] >= p[4] && p[4] >= p[5], "sepia: blue becomes warm too",
          "sepia_rgb");
  }

  // thermal: dark -> blue-ish, bright -> yellow/white, ordered by brightness
  {
    uint8_t p[9] = {0, 0, 0, 128, 128, 128, 255, 255, 255};
    int full = 255;
    thermal_rgb(p, 3, 1, &full);
    CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0, "thermal: black stays black",
          "thermal_rgb");
    CHECK(p[3] > p[5], "thermal: mid brightness is red/orange, not blue",
          "thermal_rgb");
    CHECK(p[6] >= 250 && p[7] >= 250 && p[8] >= 250,
          "thermal: white-hot stays white", "thermal_rgb");
    uint8_t q[9] = {40, 40, 40, 90, 90, 90, 200, 200, 200};
    thermal_rgb(q, 3, 1, &full);
    int cold = q[2] > q[0]; // dark end is blue
    int hot = q[6] > q[8];  // bright end is yellow
    CHECK(cold && hot, "thermal: cold is blue, hot is yellow", "thermal_rgb");
  }

  // chromatic: red slides left, blue slides right, green stays put
  {
    uint8_t row[60 * 3];
    nl_memset(row, 0, sizeof(row));
    for (int x = 30; x < 60; x++) {
      row[x * 3 + 0] = 200;
      row[x * 3 + 1] = 200;
      row[x * 3 + 2] = 200;
    }

    int full = 255;
    chromatic_rgb(row, 60, 1, &full);
    int d = 1 + 255 * 60 / (255 * 80); // matches the plugin
    CHECK(row[(30 - d) * 3 + 0] == 200 && row[(30 - d - 1) * 3 + 0] == 0 &&
              row[(30 + d) * 3 + 2] == 200 && row[(30 + d - 1) * 3 + 2] == 0,
          "chromatic: red edge moved left, blue edge right", "chromatic_rgb");
    CHECK(row[29 * 3 + 1] == 0 && row[30 * 3 + 1] == 200,
          "chromatic: green edge unmoved", "chromatic_rgb");
  }
}

int main(void) {
  test_fx_helpers();
  for (int i = 0; i < NEFFECTS; i++) {
    test_bad_frames(&effects[i]);
    test_guards(&effects[i]);
    test_zero_and_default(&effects[i]);
  }

  test_vignette();
  test_chromatic();
  test_fisheye_ripple();
  test_kaleidoscope();
  test_dither();
  test_trails_framediff();
  test_noise_effects();
  test_sort_and_glitch();
  test_rain_crt_neon();
  test_rgb_hooks();

  char buf[96];
  int n = nl_snprintf(buf, sizeof(buf), "%d passed  %d failed\n", _pass, _fail);
  nl_write(1, buf, (size_t)n);

  return _fail ? 1 : 0;
}
