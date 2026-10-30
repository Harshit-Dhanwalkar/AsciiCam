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

// No collector off Linux: nl_gc_alloc is a zeroed malloc and nl_gc_release
// really frees, so code written against nl_gc.h stays leak-free everywhere
#define nl_gc_alloc(n) calloc(1, (n))
#define nl_gc_alloc_atomic(n) calloc(1, (n))
#define nl_gc_release(p) free(p)
#endif

#endif
