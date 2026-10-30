/*
 * Location of plugins, plugin sources, caches and charsets, relative to
 * running binary as well as current directory. See include/app_paths.h
 *
 * System headers must come before nolibc.h on libc platforms (see
 * plugin_catalog.c)
 */

#include "platform.h"

#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#elif !defined(__LINUX_NOLIBC__)
#include <fcntl.h>
#include <unistd.h>
#if defined(PLATFORM_MACOS)
#include <mach-o/dyld.h>
#endif
#endif

#include "nolibc.h"

#include "app_paths.h"

#if defined(__LINUX_NOLIBC__)
#ifndef SYS_readlink
#define SYS_readlink 89
#endif
#endif

int app_path_exists(const char *path) {
#if defined(PLATFORM_WINDOWS)
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
  // open(2) accepts directories read-only, so this covers files and dirs
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    return 0;
  }

  close(fd);

  return 1;
#endif
}

static void strip_last_component(char *s) {
  size_t n = nl_strlen(s);
  while (n > 0 && s[n - 1] != '/' && s[n - 1] != '\\') {
    n--;
  }

  if (n > 1) {
    n--; // drop separator too, except for a lone leading "/"
  }

  s[n] = '\0';
}

int app_exe_dir(char *buf, size_t n) {
  if (n < 2) {
    return -1;
  }

#if defined(__LINUX_NOLIBC__)
  long r =
      __sc3(SYS_readlink, (long)"/proc/self/exe", (long)buf, (long)(n - 1));
  if (r <= 0) {
    return -1;
  }

  buf[r] = '\0';
#elif defined(PLATFORM_MACOS)
  uint32_t sz = (uint32_t)n;
  if (_NSGetExecutablePath(buf, &sz) != 0) {
    return -1;
  }
#elif defined(PLATFORM_WINDOWS)
  DWORD r = GetModuleFileNameA(NULL, buf, (DWORD)n);
  if (r == 0 || r >= n) {
    return -1;
  }
#else
  ssize_t r = readlink("/proc/self/exe", buf, n - 1);
  if (r <= 0) {
    return -1;
  }

  buf[r] = '\0';
#endif

  strip_last_component(buf);

  return buf[0] ? 0 : -1;
}

const char *app_getenv(char **envp, const char *name) {
  if (!envp || !name) {
    return (const char *)0;
  }

  size_t nl = nl_strlen(name);
  for (; *envp; envp++) {
    const char *e = *envp;
    size_t i = 0;
    while (i < nl && e[i] == name[i]) {
      i++;
    }

    if (i == nl && e[i] == '=') {
      return e + i + 1;
    }
  }

  return (const char *)0;
}

static void copy_to(char *dst, const char *src) {
  nl_strncpy_safe(dst, src, AP_LEN);
}

static void join(char *dst, const char *a, const char *b) {
  nl_snprintf(dst, AP_LEN, "%s/%s", a, b);
}

static void join3(char *dst, const char *a, const char *b, const char *c) {
  nl_snprintf(dst, AP_LEN, "%s/%s/%s", a, b, c);
}

typedef int (*exists_fn)(const char *);

// dir is a plugin SDK
static int is_sdk(exists_fn ex, const char *dir) {
  char p[AP_LEN];

  join(p, dir, "Makefile");
  if (!ex(p)) {
    return 0;
  }

  join3(p, dir, "include", "plugins.h");

  return ex(p);
}

static void add_prebuilt(app_paths_t *p, exists_fn ex, const char *dir) {
  if (p->prebuilt_count >= AP_PREBUILT_MAX || !ex(dir)) {
    return;
  }

  copy_to(p->prebuilt_dir[p->prebuilt_count++], dir);
}

void app_paths_resolve(app_paths_t *p, const app_env_t *env,
                       const char *src_override, const char *cache_override,
                       const char *charset_override) {
  nl_memset(p, 0, sizeof(*p));

  exists_fn ex = (env && env->exists) ? env->exists : app_path_exists;
  const char *exe =
      (env && env->exe_dir && env->exe_dir[0]) ? env->exe_dir : (const char *)0;
  char cand[AP_LEN];

  // SDK: a checkout we are standing in, a checkout binary lives in
  // (<checkout>/build/webcam_ascii), or an installed copy
  int sdk_is_checkout = 0;
  if (is_sdk(ex, ".")) {
    copy_to(p->sdk_dir, ".");
    sdk_is_checkout = 1;
  } else if (exe) {
    join(cand, exe, "..");
    if (is_sdk(ex, cand)) {
      copy_to(p->sdk_dir, cand);
      sdk_is_checkout = 1;
    } else {
      join3(cand, exe, "../share", "asciicam/sdk");
      if (is_sdk(ex, cand)) {
        copy_to(p->sdk_dir, cand);
      }
    }
  }

  if (src_override) {
    copy_to(p->src_dir, src_override);
  } else if (p->sdk_dir[0]) {
    join(p->src_dir, p->sdk_dir, "filters");
  } else {
    copy_to(p->src_dir, "./filters");
  }

  // Cache must be writable: next to a checkout it is, under an installed prefix
  // it is not
  if (cache_override) {
    copy_to(p->cache_dir, cache_override);
  } else if (sdk_is_checkout) {
    join3(p->cache_dir, p->sdk_dir, ".cache", "plugins");
  } else if (env && env->xdg_cache && env->xdg_cache[0]) {
    join3(p->cache_dir, env->xdg_cache, "asciicam", "plugins");
  } else if (env && env->home && env->home[0]) {
    nl_snprintf(p->cache_dir, AP_LEN, "%s/.cache/asciicam/plugins", env->home);
  } else {
    copy_to(p->cache_dir, "./.cache/plugins");
  }

  if (exe) {
    join(cand, exe, "plugins");
    add_prebuilt(p, ex, cand);

    join3(cand, exe, "../share", "asciicam/plugins");
    add_prebuilt(p, ex, cand);

    add_prebuilt(p, ex, exe); // loose layout: .so beside binary
  }

  if (charset_override) {
    copy_to(p->charset_dir, charset_override);
  } else {
    // Default fallback
    const char *cset = "./charsets";

    if (!exe || ex("./charsets")) {
      cset = "./charsets";
    } else {
      join3(cand, exe, "../share", "asciicam/charsets");
      if (ex(cand)) {
        cset = cand;
      } else {
        join(cand, exe, "../charsets");
        if (ex(cand)) {
          cset = cand;
        }
      }
    }

    // Single copy point
    copy_to(p->charset_dir, cset);
  }
}
