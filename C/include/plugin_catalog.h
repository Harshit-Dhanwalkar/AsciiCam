#ifndef PLUGIN_CATALOG_H
#define PLUGIN_CATALOG_H

/*
 * Catalog of plugins that can be loaded while program is running
 *
 * NOTE: Lazy loading/evaluation: Catalog lists every plugin SOURCE (*.c with a
 * plugin_get() in it) found in a directory. Nothing is compiled until user asks
 * for it: a build runs as a child process (`make` on existing plugin rule) so
 * render loop never blocks
 *
 * Platform layer (src/plugin_catalog.c), five small primitives:
 *   Linux nolibc  raw getdents64 / fork / execve / wait4
 *   macOS         opendir / posix_spawnp / waitpid (system libc)
 *   Windows       FindFirstFileA / CreateProcessA / WaitForSingleObject
 * Built file is .so, .dylib or .dll depending on platform
 */

#include "nl_types.h"

#define CATALOG_MAX 64
#define CATALOG_NAME_LEN 48
#define CATALOG_PATH_LEN 256
#define CATALOG_ERR_LEN 96

// Build child: a pid on POSIX, a process HANDLE on Windows (64-bit wide)
typedef long long pc_proc_t;

typedef enum {
  PC_AVAILABLE = 0, // source found, never built this session, no .so cached
  PC_READY,         // a built .so exists in cache dir
  PC_COMPILING,     // build child running
  PC_FAILED,        // last build failed (see err)
  PC_ACTIVE         // loaded and running in filter chain
} pc_state_t;

typedef struct {
  char name[CATALOG_NAME_LEN];
  char src_path[CATALOG_PATH_LEN];
  char so_path[CATALOG_PATH_LEN];
  char log_path[CATALOG_PATH_LEN];
  pc_state_t state;
  pc_proc_t pid;             // build child, valid while PC_COMPILING
  int want_load;             // load as soon as build succeeds
  char err[CATALOG_ERR_LEN]; // first error line of last failed build
} catalog_entry_t;

typedef struct {
  catalog_entry_t e[CATALOG_MAX];
  int count;
  char src_dir[CATALOG_PATH_LEN];
  char cache_dir[CATALOG_PATH_LEN];
  char **envp; // build child environment: NULL = empty on Linux nolibc,
               // inherited on macOS/Windows
} plugin_catalog_t;

// 1 if this platform can build plugins at runtime, 0 otherwise (currently
// always 1; kept so callers don't have to know)
int catalog_supported(void);

void catalog_init(plugin_catalog_t *c, const char *src_dir,
                  const char *cache_dir, char **envp);

// (Re)scan src_dir. New sources are appended in alphabetical order; known
// entries keep their state and index. Returns number of entries
int catalog_scan(plugin_catalog_t *c);

// Index of entry called name, or -1
int catalog_find(const plugin_catalog_t *c, const char *name);

// Start a non-blocking build. 0 on success (or when already running/active),
// -1 on failure, with c->e[idx].err filled in
int catalog_start_build(plugin_catalog_t *c, int idx);

// Reap finished builds without blocking. Returns 1 and sets *idx when a build
// just finished (state is now PC_READY or PC_FAILED), 0 when none did
int catalog_poll(plugin_catalog_t *c, int *idx);

// Kill and reap outstanding build children
void catalog_cleanup(plugin_catalog_t *c);

const char *catalog_state_name(pc_state_t s);

#endif
