#ifndef NL_ALLOC_H
#define NL_ALLOC_H

#ifdef __LINUX_NOLIBC__
#include <stddef.h>

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
#endif

#endif
