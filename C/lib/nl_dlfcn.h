#ifndef NL_DLFCN_H
#define NL_DLFCN_H

#include "platform.h"

/*
 * ELF64 shared-library loader
 *
 * Provides nl_dlopen / nl_dlsym / nl_dlclose / nl_dlerror using only raw Linux
 * syscalls, replacing glibc's libdl
 *
 * Dynamic loading abstraction
 *
 * Linux-nolibc: raw ELF64 loader
 * Windows     : LoadLibraryA / GetProcAddress wrappers
 * POSIX       : <dlfcn.h> passthroughs
 *
 * Supported relocation types (covers all types emitted for x86-64 -fPIC):
 *   R_X86_64_NONE, R_X86_64_RELATIVE, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT,
 *   R_X86_64_64
 */

#ifdef __LINUX_NOLIBC__

/* RTLD_* flags (accepted but not acted on by the loader) */
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100

void *nl_dlopen(const char *path, int flags);
void *nl_dlsym(void *handle, const char *name);
int nl_dlclose(void *handle);
const char *nl_dlerror(void);

/* Macro redirects */
#define dlopen(path, flags) nl_dlopen(path, flags)
#define dlsym(handle, name) nl_dlsym(handle, name)
#define dlclose(handle) nl_dlclose(handle)
#define dlerror() nl_dlerror()

#elif defined(PLATFORM_WINDOWS)

#include <windows.h>

#ifndef RTLD_LAZY
#define RTLD_LAZY 0
#endif
#ifndef RTLD_NOW
#define RTLD_NOW 0
#endif
#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0
#endif

static inline void *nl_dlopen(const char *p, int f) {
  (void)f;

  return (void *)LoadLibraryA(p);
}

static inline void *nl_dlsym(void *h, const char *n) {
  return (void *)GetProcAddress((HMODULE)h, n);
}

static inline int nl_dlclose(void *h) {
  return FreeLibrary((HMODULE)h) ? 0 : -1;
}

static inline const char *nl_dlerror(void) {
  return "LoadLibrary/GetProcAddress failed";
}

/* Macro redirects */
#define dlopen(path, flags) nl_dlopen(path, flags)
#define dlsym(handle, name) nl_dlsym(handle, name)
#define dlclose(handle) nl_dlclose(handle)
#define dlerror() nl_dlerror()

#else /* POSIX: macOS, system-libc Linux */

#include <dlfcn.h>

static inline void *nl_dlopen(const char *p, int f) { return dlopen(p, f); }

static inline void *nl_dlsym(void *h, const char *n) { return dlsym(h, n); }

static inline int nl_dlclose(void *h) { return dlclose(h); }

static inline const char *nl_dlerror(void) { return dlerror(); }

#endif /* PLATFORM BRANCHES */

#endif /* NL_DLFCN_H */
