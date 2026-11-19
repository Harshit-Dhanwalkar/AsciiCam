#ifndef PLUGIN_EMBEDDED_H
#define PLUGIN_EMBEDDED_H

// Plugins linked into binary (see lib/nl_ramfs.h, src/ramfs_blob.S)
//
// They live in built-in archive as "plugins/<name>.so" and are named
// "ramfs:<name>" wherever a plugin path is taken: -p ramfs:invert, a catalog
// entry's so_path. plugin_load() recognises that prefix and hands bytes to
// nl_dlopen_mem() instead of copying a file and opening it
// There is nothing on disk to watch, so no hot reload; a plugin of same name
// built from source or found in a prebuilt directory wins in catalog
//
// Archive comes from binary, or from plugin_set_archive() (tests)

#include "nl_ramfs.h"

#define PLUGIN_EMBEDDED_PREFIX "ramfs:"
#define PLUGIN_EMBEDDED_PREFIX_LEN 6
#define PLUGIN_EMBEDDED_DIR "plugins/"
#define PLUGIN_EMBEDDED_DIR_LEN 8
#define PLUGIN_EMBEDDED_EXT ".so"
#define PLUGIN_EMBEDDED_EXT_LEN 3

// Archive to look in: one set with plugin_set_archive(), else one linked into
// binary
// Return 0 and *out filled in, or -1 when there is none
int plugin_archive(nl_ramfs_t *out);

// Use fs instead of built-in archive; NULL goes back to built-in one
// fs must outlive its use
void plugin_set_archive(const nl_ramfs_t *fs);

// 1 if path is "ramfs:..." (whether or not anything by that name exists)
static inline int plugin_is_embedded(const char *path) {
  const char *p = PLUGIN_EMBEDDED_PREFIX;
  while (*p) {
    if (*path++ != *p++) {
      return 0;
    }
  }

  return 1;
}

// A plugin name is what an entry is called on screen and in a path: letters,
// digits, '_' and '-', at least one, shorter than cap
static inline int plugin_embedded_name_ok(const char *s, size_t cap) {
  size_t n = 0;
  for (; s[n]; n++) {
    char ch = s[n];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
      return 0;
    }
  }

  return n > 0 && n < cap;
}

// Archive entry name "plugins/<name>.so" -> name (NUL terminated in out)
// Rertun 1 if it is one, 0 for anything else (other directories, other
// extensions, names plugin_embedded_name_ok refuses)
static inline int plugin_embedded_stem(const char *entry, char *out,
                                       size_t cap) {
  const char *d = PLUGIN_EMBEDDED_DIR;
  while (*d) {
    if (*entry++ != *d++) {
      return 0;
    }
  }

  size_t len = 0;
  while (entry[len]) {
    len++;
  }

  if (len <= PLUGIN_EMBEDDED_EXT_LEN || len - PLUGIN_EMBEDDED_EXT_LEN >= cap) {
    return 0;
  }

  const char *e = PLUGIN_EMBEDDED_EXT;
  for (size_t i = 0; i < PLUGIN_EMBEDDED_EXT_LEN; i++) {
    if (entry[len - PLUGIN_EMBEDDED_EXT_LEN + i] != e[i]) {
      return 0;
    }
  }

  size_t sl = len - PLUGIN_EMBEDDED_EXT_LEN;
  for (size_t i = 0; i < sl; i++) {
    out[i] = entry[i];
  }

  out[sl] = '\0';

  return plugin_embedded_name_ok(out, cap);
}

#endif
