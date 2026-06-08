#include "plugins.h"

#include <dlfcn.h>
#include <sys/inotify.h>

#include "nolibc.h"

static int copy_file(const char *src, const char *dst) {
  int fd_src = open(src, O_RDONLY);
  if (fd_src < 0) {
    perror("[plugin] open src");
    return -1;
  }

  int fd_dst = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd_dst < 0) {
    perror("[plugin] open dst");
    close(fd_src);
    return -1;
  }

  char buf[65536];
  ssize_t n;
  while ((n = read(fd_src, buf, sizeof(buf))) > 0) {
    if (write(fd_dst, buf, (size_t)n) != n) {
      perror("[plugin] write");
      close(fd_src);
      close(fd_dst);
      unlink(dst);
      return -1;
    }
  }

  close(fd_src);
  close(fd_dst);
  return 0;
}

int plugin_load(plugin_loader_t *pl, const char *path) {
  // Close old handle
  if (pl->dl_handle) {
    dlclose(pl->dl_handle);
    pl->dl_handle = NULL;
    pl->plugin = NULL;
  }

  // Delete the previous temp copy
  if (pl->tmp_path[0] != '\0') {
    unlink(pl->tmp_path);
    pl->tmp_path[0] = '\0';
  }

  snprintf(pl->tmp_path, sizeof(pl->tmp_path), "%s.%ld.tmp", path,
           (long)time(NULL));

  if (copy_file(path, pl->tmp_path) < 0) {
    fprintf(stderr, "[plugin] could not copy %s -> %s\n", path, pl->tmp_path);
    pl->tmp_path[0] = '\0';
    return -1;
  }

  pl->dl_handle = dlopen(pl->tmp_path, RTLD_NOW | RTLD_LOCAL);
  if (!pl->dl_handle) {
    fprintf(stderr, "[plugin] dlopen: %s\n", dlerror());
    unlink(pl->tmp_path);
    pl->tmp_path[0] = '\0';
    return -1;
  }

  void *sym = dlsym(pl->dl_handle, "plugin_get");
  if (!sym) {
    fprintf(stderr, "[plugin] dlsym: %s\n", dlerror());
    dlclose(pl->dl_handle);
    pl->dl_handle = NULL;
    return -1;
  }

  filter_plugin_t *(*get_plugin)(void) = dlsym(pl->dl_handle, "plugin_get");
  if (!get_plugin) {
    fprintf(stderr, "[plugin] dlsym: %s\n", dlerror());
    dlclose(pl->dl_handle);
    pl->dl_handle = NULL;
    unlink(pl->tmp_path);
    pl->tmp_path[0] = '\0';
    return -1;
  }

  pl->plugin = get_plugin();
  snprintf(pl->status_msg, sizeof(pl->status_msg), "loaded: %s",
           pl->plugin->name);

  // FIX: Temporary file leak in plugin_load() when dlsym fails
  // unlink(pl->tmp_path)
  return 0;
}

void plugin_watch_init(plugin_loader_t *pl, const char *path) {
  nl_strncpy_safe(pl->path, path, sizeof(pl->path) - 1);

  char dir_copy[256];
  nl_strncpy_safe(dir_copy, path, sizeof(dir_copy) - 1);
  const char *dir = dirname(dir_copy);

  pl->inotify_fd = inotify_init1(IN_NONBLOCK);
  if (pl->inotify_fd < 0) {
    perror("[plugin] inotify_init1");
    return;
  }

  pl->inotify_wd =
      inotify_add_watch(pl->inotify_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO);

  if (pl->inotify_wd < 0)
    perror("[plugin] inotify_add_watch");
}

void plugin_check_reload(plugin_loader_t *pl) {
  if (pl->inotify_fd < 0)
    return;

  // Read all available events from the non-blocking queue
  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  // char buf[sizeof(struct inotify_event) + 256];
  ssize_t n = read(pl->inotify_fd, buf, sizeof(buf));
  if (n <= 0)
    return; // No new filesystem modifications detected

  char path_copy[256];
  nl_strncpy_safe(path_copy, pl->path, sizeof(path_copy) - 1);
  const char *soname = basename(path_copy);

  int relevant = 0;
  const char *p = buf;
  while (p < buf + n) {
    const struct inotify_event *ev = (const struct inotify_event *)p;
    if (ev->len > 0 && strcmp(ev->name, soname) == 0) {
      relevant = 1;
      break;
    }
    p += sizeof(*ev) + ev->len;
  }

  if (!relevant)
    return;

  // TODO: Replace with inotify event coalescing for more deterministic reload.
  usleep(100000); // 0.1s delay for linker

  if (plugin_load(pl, pl->path) == 0) {
    snprintf(pl->status_msg, sizeof(pl->status_msg), "hot-swapped -> %s",
             pl->plugin->name);
  } else {
    snprintf(pl->status_msg, sizeof(pl->status_msg),
             "hot-swap FAILED - old filter retained");
  }
}

void plugin_cleanup(plugin_loader_t *pl) {
  if (pl->dl_handle) {
    dlclose(pl->dl_handle);
    pl->dl_handle = NULL;
  }

  // Remove tmp copy
  if (pl->tmp_path[0] != '\0') {
    unlink(pl->tmp_path);
    pl->tmp_path[0] = '\0';
  }

  if (pl->inotify_fd >= 0) {
    close(pl->inotify_fd);
    pl->inotify_fd = -1;
  }
}
