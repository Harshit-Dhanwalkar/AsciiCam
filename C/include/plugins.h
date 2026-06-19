#ifndef PLUGINS_H
#define PLUGINS_H

#include <stdint.h>

typedef struct {
  void (*process)(uint8_t *gray, int w, int h, void *ctx);
  const char *name;
} filter_plugin_t;

typedef struct {
  void *dl_handle;         // handle from dlopen
  filter_plugin_t *plugin; // resolved plugin vtable
  char path[256];          // absolute path to .so
  char tmp_path[280];      // temp copy path used for current dlopen // HACK:
  char status_msg[128];    // last load/swap message
  int inotify_fd;          // inotify instance fd
  int inotify_wd;          // watch descriptor
} plugin_loader_t;

int plugin_load(plugin_loader_t *pl, const char *path);
void plugin_watch_init(plugin_loader_t *pl, const char *path);
void plugin_check_reload(plugin_loader_t *pl);
void plugin_cleanup(plugin_loader_t *pl);

#endif
