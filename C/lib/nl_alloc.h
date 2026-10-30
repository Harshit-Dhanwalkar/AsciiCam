#ifndef NL_ALLOC_H
#define NL_ALLOC_H

#ifdef __LINUX_NOLIBC__

#include "nl_gc.h"
#include "nl_types.h"

void *nl_malloc(size_t n);
void *nl_calloc(size_t nmemb, size_t size);
void nl_free(void *ptr);

#define malloc(n) nl_malloc(n)
#define calloc(nm, sz) nl_calloc(nm, sz)
#define free(p) nl_free(p)

#else // MACOS / Windows
#include <stdlib.h>

#define nl_malloc malloc
#define nl_calloc calloc
#define nl_free free

/* GC API (nl_gc_alloc, nl_gc_alloc_atomic, nl_gc_release, ...) lives in nl_gc.h
 * only
 * On non-Linux they are zeroed-malloc macros, and redeclaring them as functions
 * in nl_gc.h would make preprocessor expand declarations */
#endif

#endif
