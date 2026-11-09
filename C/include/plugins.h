#ifndef PLUGINS_H
#define PLUGINS_H

#include "nl_types.h"

typedef struct {
  void (*process)(uint8_t *gray, int w, int h, void *ctx);
  const char *name;
} filter_plugin_t;

// Version of plugin interface (filter_plugin_t and hooks host calls). Bump it
// when that layout changes. A plugin built from these headers carries number it
// was built against, so a prebuilt library from anthero release is refused with
// a message instead of crashing host. Put it once in plugin source, next to
// plugin_get(): PLUGIN_ABI_EXPORT; Libraries without it (older plugins) are
// still accepted
#define PLUGIN_ABI 1
#define PLUGIN_ABI_EXPORT                                                      \
  const int asciicam_plugin_abi __attribute__((visibility("default"), used)) = \
      PLUGIN_ABI

// Optional colour hook. A plugin may also export
//   void plugin_rgb_process(uint8_t *rgb, int w, int h, void *ctx);
// (rgb = w * h * 3 bytes, R G B per pixel)
// Host calls it in colour mode only, after every plugin's luma process() has
// run and colour picture has been built from it, with same parameter
typedef void (*plugin_rgb_fn)(uint8_t *rgb, int w, int h, void *ctx);

typedef struct {
  void *dl_handle;           // handle from dlopen
  filter_plugin_t *plugin;   // resolved plugin vtable
  plugin_rgb_fn rgb_process; // optional colour hook, NULL when absent
  char path[256];            // absolute path to .so
  char tmp_path[280];        // temp copy path used for current dlopen // HACK:
  char status_msg[128];      // last load/swap message
  int inotify_fd;            // inotify instance fd
  int inotify_wd;            // watch descriptor
} plugin_loader_t;

// 0 silences stderr diagnostics (TUI owns terminal), status_msg is always
// filled in
extern int plugin_log_stderr;

int plugin_load(plugin_loader_t *pl, const char *path);
void plugin_watch_init(plugin_loader_t *pl, const char *path);
void plugin_check_reload(plugin_loader_t *pl);
void plugin_cleanup(plugin_loader_t *pl);

#endif
