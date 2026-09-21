#ifndef NL_DLFCN_H
#define NL_DLFCN_H

#ifdef PLATFORM_WINDOWS
  #include <windows.h>

 #ifndef RTLD_LAZY
    #define RTLD_LAZY 0
  #endif
  #ifndef RTLD_NOW
    #define RTLD_NOW 0
  #endif

  static inline void *nl_dlopen(const char *path, int flags) {
    (void)flags;

    return (void *)LoadLibraryA(path);
  }

  static inline void *nl_dlsym(void *handle, const char *name) {
    return (void *)GetProcAddress((HMODULE)handle, name);
  }

  static inline int nl_dlclose(void *handle) {
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
  }

  static inline const char *nl_dlerror(void) {
    return "dynamic loading error (see GetLastError)";
  }

  #define dlopen(p, f)  nl_dlopen(p, f)
  #define dlsym(h, n)   nl_dlsym(h, n)
  #define dlclose(h)    nl_dlclose(h)
  #define dlerror()     nl_dlerror()

#else
  /* Linux, macOS, and other POSIX systems */
  // TODO: implement raw assembly or pure C dlfcn.h
  #include <dlfcn.h>
#endif

#endif /* NL_DLFCN_H */
