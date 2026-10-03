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
 * WARN: Only for Linux: other platforms get an empty catalog and
 * catalog_start_build() fails with a message
 */

#include "nl_types.h"

#define CATALOG_MAX 64
#define CATALOG_NAME_LEN 48
#define CATALOG_PATH_LEN 256
#define CATALOG_ERR_LEN 96

typedef enum {
  PC_AVAILABLE = 0, // source found, never built this session, no .so cached
  PC_READY,         // a built .so exists in the cache dir
  PC_COMPILING,     // build child running
  PC_FAILED,        // last build failed (see err)
  PC_ACTIVE         // loaded and running in the filter chain
} pc_state_t;

typedef struct {
  char name[CATALOG_NAME_LEN];
  char src_path[CATALOG_PATH_LEN];
  char so_path[CATALOG_PATH_LEN];
  char log_path[CATALOG_PATH_LEN];
  pc_state_t state;
  long pid;                  // build child, valid while PC_COMPILING
  int want_load;             // load as soon as the build succeeds
  char err[CATALOG_ERR_LEN]; // first error line of the last failed build
} catalog_entry_t;

typedef struct {
  catalog_entry_t e[CATALOG_MAX];
  int count;
  char src_dir[CATALOG_PATH_LEN];
  char cache_dir[CATALOG_PATH_LEN];
  char **envp; // environment for build children (NULL = empty)
} plugin_catalog_t;

// 1 if this platform can build plugins at runtime, 0 otherwise
int catalog_supported(void);

void catalog_init(plugin_catalog_t *c, const char *src_dir,
                  const char *cache_dir, char **envp);

// (Re)scan src_dir. New sources are appended in alphabetical order; known
// entries keep their state and index. Returns the number of entries
int catalog_scan(plugin_catalog_t *c);

// Index of the entry called name, or -1
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
