#ifndef NL_ERRNO_H
#define NL_ERRNO_H

#include <stddef.h>

extern int errno;

void nl_perror(const char *msg);

#define perror(msg) nl_perror(msg)

#endif
