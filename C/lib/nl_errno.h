#ifndef NL_ERRNO_H
#define NL_ERRNO_H

#include <stddef.h>

extern int errno;

const char *nl_strerror(int err);
void nl_perror(const char *msg);

#define perror(msg) nl_perror(msg)
#define strerror(e) nl_strerror(e)

#endif
