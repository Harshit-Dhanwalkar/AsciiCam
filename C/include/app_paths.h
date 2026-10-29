#ifndef APP_PATHS_H
#define APP_PATHS_H

/*
 * Where AsciiCam finds its plugins, plugin sources and charsets
 *
 * Everything used to be relative to current directory (./filters,
 * ./.cache/plugins, ./charsets), which only works when started from a source
 * checkout. Resolution is now relative to running binary as well, so
 * same program works from a checkout, from an unpacked release directory and
 * from an installed prefix (`make install`):
 *
 *   checkout        <C>/build/webcam_ascii     sources <C>/filters
 *   release dir     <dir>/asciicam             plugins <dir>/plugins
 *   installed       <prefix>/bin/asciicam      plugins
 * <prefix>/share/asciicam/plugins
 *
 * Three kinds of plugin location are involved:
 *   prebuilt   ready-made shared objects: load instantly, need no compiler
 *   sources    *.c files picker can build on demand (needs make + a
 *              compiler and "SDK": Makefile + headers)
 *   cache      writable directory runtime builds are written to
 * A shipped application normally carries only `prebuilt`
 */

#include "nl_types.h"

#define AP_LEN 256
#define AP_PREBUILT_MAX 4

typedef struct {
  char sdk_dir[AP_LEN];   // holds Makefile + include/plugins.h; "" if none
  char src_dir[AP_LEN];   // plugin sources
  char cache_dir[AP_LEN]; // writable: runtime builds land here
  char charset_dir[AP_LEN];
  char prebuilt_dir[AP_PREBUILT_MAX][AP_LEN]; // existing dirs, best first
  int prebuilt_count;
} app_paths_t;

typedef struct {
  const char *exe_dir;   // directory of the running binary, NULL if unknown
  const char *home;      // $HOME, NULL if unset
  const char *xdg_cache; // $XDG_CACHE_HOME, NULL if unset
  int (*exists)(const char *path); // file or directory; NULL = real filesystem
} app_env_t;

/*
 * Resolve every location. An override (command line or config file) always
 * wins; pass NULL to use the automatic choice:
 *
 *   sdk      "." if it is a checkout, else <exe>/.., else
 *            <exe>/../share/asciicam/sdk
 *   sources  <sdk>/filters
 *   cache    <sdk>/.cache/plugins when the SDK is a checkout (as before),
 *            otherwise $XDG_CACHE_HOME/asciicam/plugins or
 *            ~/.cache/asciicam/plugins
 *   prebuilt <exe>/plugins, <exe>/../share/asciicam/plugins, <exe> itself
 *   charsets ./charsets, <exe>/../share/asciicam/charsets, <exe>/../charsets
 */
void app_paths_resolve(app_paths_t *p, const app_env_t *env,
                       const char *src_override, const char *cache_override,
                       const char *charset_override);

// Directory of the running executable into buf. 0 on success, -1 if this
// platform cannot tell (callers then fall back to current-directory paths)
int app_exe_dir(char *buf, size_t n);

// Value of NAME in an envp-style array, or NULL
const char *app_getenv(char **envp, const char *name);

// Real-filesystem existence test used when app_env_t.exists is NULL
int app_path_exists(const char *path);

#endif
