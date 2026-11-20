#ifndef PLUGIN_SANDBOX_H
#define PLUGIN_SANDBOX_H

/*
 * Run plugins in a child process that can do almost nothing
 *
 * A plugin is native code from outside app: a bug in it crashes whole
 * program, and a bad one can do anything user can. With sandbox on,
 * a loaded plugin's process() and rgb hook run in a forked child that has
 *
 *   - frame in memory shared with app (copied in and out per call)
 *   - no file descriptors, and a seccomp filter (lib/nl_seccomp.h) allowing
 *     futex, munmap, exit_group and mmap *without* PROT_EXEC: enough for
 *     plugin's only imports (malloc, calloc, free, memcpy, memset, strlen) and
 *     nothing else. Any other system call kills child with SIGSYS
 *   - PR_SET_PDEATHSIG, so it never outlives app
 *
 * NOTE: App never trusts what child leaves behind beyond pixel values: it reads
 * back exactly bytes it sent, whatever child wrote
 *
 * A plugin that crashes, is killed by filter or does not answer within
 * plugin_sandbox_timeout_ms is switched off (pl->disabled is set,
 * pl->status_msg says why; plugin stays in its slot, so app can still
 * show its name) and app carries on. Loading a library into slot switches it
 * back on
 * Plugins keep their state between frames, since child lives as long as plugin
 * stays loaded; loading another library into slot restarts it
 *
 * Linux x86-64 (nolibc build) only. Elsewhere, or when kernel refuses filter, a
 * sandboxed run fails closed: plugin is switched off with a message and is
 * never run in-process behind user's back
 */

#include "plugins.h"

// 0 (default): plugin_run() calls plugins in-process, exactly as before
extern int plugin_sandbox_enabled;

// Longest a single call may take before child is killed
extern int plugin_sandbox_timeout_ms; // Default 1000

// 1 if this build can sandbox
int plugin_sandbox_supported(void);

// Run pl's luma hook (rgb == 0, buf = w*h bytes) or colour hook (rgb != 0,
// buf = w*h*3 bytes) on buf, in place
// *param is passed as plugin's ctx
// Does nothing and returns 0, when plugin is not loaded or has no such hook
//   0   done
//  -1   plugin was switched off just now, see pl->status_msg (later calls on a
//       switched-off plugin do nothing and return 0)
int plugin_run(plugin_loader_t *pl, int rgb, uint8_t *buf, int w, int h,
               int *param);

// End pl's sandbox if it has one
// Normally reached through plugin_load() and plugin_cleanup(); safe to call at
// any time
void plugin_sandbox_stop(plugin_loader_t *pl);

// Process id of pl's sandbox child, or -1 (for tests and diagnostics)
long plugin_sandbox_pid(const plugin_loader_t *pl);

#endif
