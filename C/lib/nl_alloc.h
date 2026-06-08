#ifndef NL_ALLOC_H
#define NL_ALLOC_H

#include <stddef.h>

void *nl_malloc(size_t n);
void *nl_calloc(size_t nmemb, size_t size);
void nl_free(void *ptr);

#define malloc(n) nl_malloc(n)
#define calloc(nm, sz) nl_calloc(nm, sz)
#define free(p) nl_free(p)

#endif
