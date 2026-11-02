/*
 * tests/test_app_paths.c - where plugins, sources, caches and charsets are
 * looked for, against a fake filesystem (see include/app_paths.h)
 */

#include "../lib/nolibc.h"

#include "app_paths.h"

static int fails, total;
#define CHECK(c, msg)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (!(c)) {                                                                \
      fails++;                                                                 \
      fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);                  \
    }                                                                          \
  } while (0)

static const char *fake_fs[16];
static int fake_n;

static void fs_set(const char **list, int n) {
  fake_n = n;
  for (int i = 0; i < n; i++) {
    fake_fs[i] = list[i];
  }
}

static int fake_exists(const char *p) {
  for (int i = 0; i < fake_n; i++) {
    if (nl_strcmp(fake_fs[i], p) == 0) {
      return 1;
    }
  }

  return 0;
}

static int streq(const char *a, const char *b) { return nl_strcmp(a, b) == 0; }

int main(void) {
  app_paths_t p;

  // source checkout, started from inside it
  {
    const char *l[] = {"./include/plugins.h", "./filters", "./charsets",
                       "/co/build"};
    fs_set(l, 4);
    app_env_t e = {"/co/build", "/home/u", NULL, fake_exists};
    app_paths_resolve(&p, &e, NULL, NULL, NULL);
    CHECK(streq(p.sdk_dir, "."), "checkout: sdk is cwd");
    CHECK(streq(p.src_dir, "./filters"), "checkout: sources ./filters");
    CHECK(streq(p.cache_dir, "./.cache/plugins"), "checkout: cache in tree");
    CHECK(streq(p.charset_dir, "./charsets"), "checkout: charsets ./charsets");
    CHECK(p.prebuilt_count == 1 && streq(p.prebuilt_dir[0], "/co/build"),
          "checkout: build dir counts as prebuilt");
  }

  // checkout binary run from elsewhere
  {
    const char *l[] = {"/co/build/../include/plugins.h", "/co/build"};
    fs_set(l, 2);
    app_env_t e = {"/co/build", "/home/u", NULL, fake_exists};
    app_paths_resolve(&p, &e, NULL, NULL, NULL);
    CHECK(streq(p.sdk_dir, "/co/build/.."), "exe in checkout: sdk found");
    CHECK(streq(p.src_dir, "/co/build/../filters"), "exe in checkout: sources");
    CHECK(streq(p.cache_dir, "/co/build/../.cache/plugins"),
          "exe in checkout: cache in tree");
  }

  // Makefile without plugin headers is not an SDK; headers alone are
  {
    const char *l[] = {"./Makefile", "./filters", "./charsets"};
    fs_set(l, 3);
    app_env_t e = {"/co/build", "/home/u", NULL, fake_exists};
    app_paths_resolve(&p, &e, NULL, NULL, NULL);
    CHECK(p.sdk_dir[0] == '\0', "Makefile alone is not an SDK");
  }

  // installed prefix with SDK: cache must go to user, not /usr
  {
    const char *l[] = {"/usr/bin/../share/asciicam/sdk/include/plugins.h",
                       "/usr/bin/../share/asciicam/plugins",
                       "/usr/bin/../share/asciicam/charsets"};
    fs_set(l, 3);
    app_env_t e = {"/usr/bin", "/home/u", NULL, fake_exists};
    app_paths_resolve(&p, &e, NULL, NULL, NULL);
    CHECK(streq(p.sdk_dir, "/usr/bin/../share/asciicam/sdk"), "installed: sdk");
    CHECK(streq(p.src_dir, "/usr/bin/../share/asciicam/sdk/filters"),
          "installed: sources inside sdk");
    CHECK(streq(p.cache_dir, "/home/u/.cache/asciicam/plugins"),
          "installed: cache under $HOME");
    CHECK(p.prebuilt_count == 1 &&
              streq(p.prebuilt_dir[0], "/usr/bin/../share/asciicam/plugins"),
          "installed: prebuilt dir");
    CHECK(streq(p.charset_dir, "/usr/bin/../share/asciicam/charsets"),
          "installed: charsets");

    e.xdg_cache = "/xdg";
    app_paths_resolve(&p, &e, NULL, NULL, NULL);
    CHECK(streq(p.cache_dir, "/xdg/asciicam/plugins"), "XDG_CACHE_HOME wins");
  }

  // shipped without SDK: nothing to compile with, prebuilt only
  {
    const char *l[] = {"/opt/ac/plugins"};
    fs_set(l, 1);
    app_env_t e = {"/opt/ac", NULL, NULL, fake_exists};
    app_paths_resolve(&p, &e, NULL, NULL, NULL);
    CHECK(p.sdk_dir[0] == '\0', "release dir: no sdk");
    CHECK(p.prebuilt_count == 1 && streq(p.prebuilt_dir[0], "/opt/ac/plugins"),
          "release dir: plugins next to binary");
    CHECK(streq(p.cache_dir, "./.cache/plugins"), "no home: cwd cache");
  }

  // explicit settings always win, existing dirs only are listed
  {
    fs_set(NULL, 0);
    app_env_t e = {"/x", "/h", NULL, fake_exists};
    app_paths_resolve(&p, &e, "/my/src", "/my/cache", "/my/cs");
    CHECK(streq(p.src_dir, "/my/src") && streq(p.cache_dir, "/my/cache") &&
              streq(p.charset_dir, "/my/cs"),
          "overrides win");
    CHECK(p.prebuilt_count == 0, "missing dirs are not listed");
  }

  // unknown exe location degrades to cwd-relative behaviour
  {
    fs_set(NULL, 0);
    app_env_t e = {NULL, NULL, NULL, fake_exists};
    app_paths_resolve(&p, &e, NULL, NULL, NULL);
    CHECK(streq(p.src_dir, "./filters") && streq(p.charset_dir, "./charsets"),
          "no exe dir: old defaults");
  }

  // helpers
  {
    char *env[] = {"HOME=/h", "XDG_CACHE_HOME=/c", "HOMEX=bad", (char *)0};
    CHECK(streq(app_getenv(env, "HOME"), "/h"), "getenv exact");
    CHECK(app_getenv(env, "HOM") == (const char *)0, "getenv no prefix match");
    CHECK(app_getenv((char **)0, "HOME") == (const char *)0, "getenv NULL env");

    char d[AP_LEN];
    CHECK(app_exe_dir(d, sizeof(d)) == 0 && d[0] == '/', "exe dir is absolute");
    CHECK(app_path_exists("/") && !app_path_exists("/definitely/not/here"),
          "real exists()");
  }

  fprintf(stderr, "%d/%d checks passed\n", total - fails, total);

  return fails ? 1 : 0;
}
