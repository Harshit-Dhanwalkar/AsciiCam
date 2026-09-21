#ifndef NL_DLFCN_H
#define NL_DLFCN_H

#include "platform.h"   
#if defined(PLATFORM_WINDOWS)
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

#define dlopen(p, f) nl_dlopen(p, f)
#define dlsym(h, n) nl_dlsym(h, n)
#define dlclose(h) nl_dlclose(h)
#define dlerror() nl_dlerror()

#else
/* Linux, macOS, and other POSIX systems */
// TODO: implement raw assembly or pure C dlfcn.h
#include <dlfcn.h>
#endif

#endif /* NL_DLFCN_H */
