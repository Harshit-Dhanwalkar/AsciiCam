#ifndef NL_INOTIFY_H
#define NL_INOTIFY_H

/*
 * inotify is Linux-only. On macOS/Windows this shim makes
 * inotify_init1() always fail (-1), which is the "unavailable" path
 * charset_registry_init()/plugin_watch_init() handle: the fd stays -1, and
 * charset_registry_check_reload()/plugin_check_reload() bail out immediately.
 * Net effect: hot-reload of charsets/plugins is disabled on non-Linux platforms
 */

#include "platform.h"

#ifdef PLATFORM_LINUX
#include <sys/inotify.h>
#else

#define IN_NONBLOCK 0
#define IN_CLOSE_WRITE 0
#define IN_MOVED_TO 0
#define IN_CREATE 0
#define IN_DELETE 0

struct inotify_event {
  int wd;
  unsigned int mask;
  unsigned int cookie;
  unsigned int len;
  char name[1];
};

static inline int inotify_init1(int flags) {
  (void)flags;
  return -1;
}

static inline int inotify_add_watch(int fd, const char *path,
                                    unsigned int mask) {
  (void)fd;
  (void)path;
  (void)mask;
  return -1;
}

#endif // PLATFORM_LINUX

#endif // NL_INOTIFY_H
