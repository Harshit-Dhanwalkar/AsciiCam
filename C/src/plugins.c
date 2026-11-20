#include "nolibc.h"

#include "plugins.h"

#include "nl_defer.h"
#include "nl_dlfcn.h"
#include "nl_inotify.h"
#include "plugin_embedded.h"

// Windows opens files in text mode by default, which would mangle a DLL copied
// byte by byte (CRLF translation, ^Z treated as EOF)
#ifndef O_BINARY
#define O_BINARY 0
#endif

int plugin_log_stderr = 1;

static const nl_ramfs_t *g_archive_override;

void plugin_set_archive(const nl_ramfs_t *fs) { g_archive_override = fs; }

int plugin_archive(nl_ramfs_t *out) {
  if (g_archive_override) {
    *out = *g_archive_override;

    return out->base ? 0 : -1;
  }

  return nl_ramfs_builtin(out);
}

// "ramfs:<name>": bytes come from archive, there is no file to copy
static void *open_embedded(const char *name, const char **err) {
  char key[PLUGIN_EMBEDDED_DIR_LEN + 64 + PLUGIN_EMBEDDED_EXT_LEN + 1];
  if (!plugin_embedded_name_ok(name, 64)) {
    *err = "bad built-in plugin name";

    return NULL;
  }

  nl_ramfs_t fs;
  if (plugin_archive(&fs) < 0) {
    *err = "no built-in plugins in this build";

    return NULL;
  }

  nl_snprintf(key, sizeof(key), "%s%s%s", PLUGIN_EMBEDDED_DIR, name,
              PLUGIN_EMBEDDED_EXT);

  const void *img = NULL;
  size_t len = 0;
  if (nl_ramfs_find(&fs, key, &img, &len) < 0) {
    *err = "not among the built-in plugins";

    return NULL;
  }

  void *h = nl_dlopen_mem(img, len, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    *err = dlerror();
  }

  return h;
}

static int copy_file(const char *src, const char *dst) {
  NL_AUTOCLOSE int fd_src = open(src, O_RDONLY | O_BINARY);
  if (fd_src < 0) {
    if (plugin_log_stderr) {
      nl_perror("[plugin] open src");
    }

    return -1;
  }

  NL_AUTOCLOSE int fd_dst =
      open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0755);
  if (fd_dst < 0) {
    if (plugin_log_stderr) {
      nl_perror("[plugin] open dst");
    }

    return -1;
  }

  char buf[65536] = {0};
  ssize_t n;
  while ((n = read(fd_src, buf, sizeof(buf))) > 0) {
    if (write(fd_dst, buf, (size_t)n) != n) {
      if (plugin_log_stderr) {
        nl_perror("[plugin] write");
      }

      unlink(dst);

      return -1;
    }
  }

  // A failed read used to fall out of loop and report success, leaving a
  // truncated library that loader then tried to map
  if (n < 0) {
    if (plugin_log_stderr) {
      nl_perror("[plugin] read");
    }

    unlink(dst);

    return -1;
  }

  return 0;
}

static const char *base_name(const char *p) {
  const char *b = p;
  for (; *p; p++) {
    if (*p == '/' || *p == '\\') {
      b = p + 1;
    }
  }

  return b;
}

static void (*g_sandbox_stop)(plugin_loader_t *);

void plugin_set_sandbox_stop(void (*stop)(plugin_loader_t *)) {
  g_sandbox_stop = stop;
}

// Code a sandbox child runs is old library: end it before that goes
static void sandbox_end(plugin_loader_t *pl) {
  if (pl->sandbox && g_sandbox_stop) {
    g_sandbox_stop(pl);
  }
}

// Library is loaded from a private copy so that rebuilding it never changes
// file mapped by running process and a fresh dlopen sees new code
// Preferred spot is next to library (where hot-reload watch is); a shipped
// library under a read-only prefix is copied to /tmp instead. If neither works
// tmp_path stays empty and original is loaded directly (works, but without hot
// reload)
static void make_private_copy(plugin_loader_t *pl, const char *path) {
  nl_snprintf(pl->tmp_path, sizeof(pl->tmp_path), "%s.%ld.tmp", path,
              (long)time(NULL));
  if (copy_file(path, pl->tmp_path) == 0) {
    return;
  }

#ifndef PLATFORM_WINDOWS
  nl_snprintf(pl->tmp_path, sizeof(pl->tmp_path), "/tmp/asciicam-%ld-%lx-%s",
              (long)time(NULL), (unsigned long)(size_t)pl, base_name(path));
  if (copy_file(path, pl->tmp_path) == 0) {
    return;
  }
#else
  (void)base_name;
#endif

  pl->tmp_path[0] = '\0';
}

int plugin_load(plugin_loader_t *pl, const char *path) {
  sandbox_end(pl);
  pl->disabled = 0;

  // Close old handle
  if (pl->dl_handle) {
    dlclose(pl->dl_handle);
    pl->dl_handle = NULL;
    pl->plugin = NULL;
    pl->rgb_process = NULL;
  }

  // Delete previous temp copy
  if (pl->tmp_path[0] != '\0') {
    unlink(pl->tmp_path);
    pl->tmp_path[0] = '\0';
  }

  const char *derr = NULL;
  if (plugin_is_embedded(path)) {
    pl->dl_handle = open_embedded(path + PLUGIN_EMBEDDED_PREFIX_LEN, &derr);
  } else {
    make_private_copy(pl, path);
    const char *load_path = pl->tmp_path[0] ? pl->tmp_path : path;

    pl->dl_handle = dlopen(load_path, RTLD_NOW | RTLD_LOCAL);
    if (!pl->dl_handle) {
      derr = dlerror();
    }
  }

  if (!pl->dl_handle) {
    if (plugin_log_stderr) {
      fprintf(stderr, "[plugin] dlopen: %s\n", derr);
    }

    nl_snprintf(pl->status_msg, sizeof(pl->status_msg), "dlopen: %s",
                derr ? derr : "failed");

    if (pl->tmp_path[0]) {
      unlink(pl->tmp_path);
      pl->tmp_path[0] = '\0';
    }

    return -1;
  }

  filter_plugin_t *(*get_plugin)(void) = dlsym(pl->dl_handle, "plugin_get");
  if (!get_plugin) {
    const char *serr = dlerror();
    if (plugin_log_stderr) {
      fprintf(stderr, "[plugin] dlsym plugin_get: %s\n", serr);
    }

    nl_snprintf(pl->status_msg, sizeof(pl->status_msg), "no plugin_get: %s",
                serr);

    dlclose(pl->dl_handle);
    pl->dl_handle = NULL;
    if (pl->tmp_path[0]) {
      unlink(pl->tmp_path);
      pl->tmp_path[0] = '\0';
    }

    return -1;
  }

  // A library built against another version of plugin interface would crash
  // host: refuse it. One without symbol predates check
  const int *abi = (const int *)dlsym(pl->dl_handle, "asciicam_plugin_abi");
  if (abi && *abi != PLUGIN_ABI) {
    nl_snprintf(pl->status_msg, sizeof(pl->status_msg),
                "plugin built for interface v%d, this build is v%d: rebuild it",
                *abi, PLUGIN_ABI);
    if (plugin_log_stderr) {
      fprintf(stderr, "[plugin] %s\n", pl->status_msg);
    }

    dlclose(pl->dl_handle);
    pl->dl_handle = NULL;
    if (pl->tmp_path[0]) {
      unlink(pl->tmp_path);
      pl->tmp_path[0] = '\0';
    }

    return -1;
  }

  pl->plugin = get_plugin();
  pl->rgb_process = (plugin_rgb_fn)dlsym(pl->dl_handle, "plugin_rgb_process");
  nl_snprintf(pl->status_msg, sizeof(pl->status_msg), "loaded: %s",
              pl->plugin->name);

  return 0;
}

void plugin_watch_init(plugin_loader_t *pl, const char *path) {
  nl_strncpy_safe(pl->path, path, sizeof(pl->path) - 1);

  // Built-in plugin: nothing on disk to watch
  if (plugin_is_embedded(path)) {
    pl->inotify_fd = -1;
    pl->inotify_wd = -1;

    return;
  }

  char dir_copy[256];
  nl_strncpy_safe(dir_copy, path, sizeof(dir_copy) - 1);
  const char *dir = dirname(dir_copy);

  pl->inotify_fd = inotify_init1(IN_NONBLOCK);
  if (pl->inotify_fd < 0) {
    nl_perror("[plugin] inotify_init1");

    return;
  }

  pl->inotify_wd =
      inotify_add_watch(pl->inotify_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO);

  if (pl->inotify_wd < 0) {
    nl_perror("[plugin] inotify_add_watch");
  }
}

void plugin_check_reload(plugin_loader_t *pl) {
  if (pl->inotify_fd < 0) {
    return;
  }

  // Read all available events from non-blocking queue
  char buf[4096]
      __attribute__((aligned(__alignof__(struct inotify_event)))) = {0};
  ssize_t n = read(pl->inotify_fd, buf, sizeof(buf));
  if (n <= 0) {
    return; // No new filesystem modifications detected
  }

  char path_copy[256];
  nl_strncpy_safe(path_copy, pl->path, sizeof(path_copy) - 1);
  const char *soname = basename(path_copy);

  int relevant = 0;
  const char *p = buf;
  while (p < buf + n) {
    const struct inotify_event *ev = (const struct inotify_event *)p;
    // NOLINTNEXTLINE(clang-analyzer-core.UndefinedBinaryOperatorResult)
    if (ev->len > 0 && nl_strcmp(ev->name, soname) == 0) {
      relevant = 1;

      break;
    }

    p += sizeof(*ev) + ev->len;
  }

  if (!relevant) {
    return;
  }

  // TODO: Replace with inotify event coalescing for more deterministic reload
  usleep(100000); // 0.1s delay for linker

  if (plugin_load(pl, pl->path) == 0) {
    nl_snprintf(pl->status_msg, sizeof(pl->status_msg), "hot-swapped -> %s",
                pl->plugin->name);
  } else {
    nl_snprintf(pl->status_msg, sizeof(pl->status_msg),
                "hot-swap FAILED - old filter retained");
  }
}

void plugin_cleanup(plugin_loader_t *pl) {
  sandbox_end(pl);

  if (pl->dl_handle) {
    dlclose(pl->dl_handle);
    pl->dl_handle = NULL;
    pl->rgb_process = NULL;
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
