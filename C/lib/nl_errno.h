#ifndef NL_ERRNO_H
#define NL_ERRNO_H

#ifdef __LINUX_NOLIBC__

#include <stddef.h>

extern int errno;

const char *nl_strerror(int err);
void nl_perror(const char *msg);

#define perror(msg) nl_perror(msg)
#define strerror(e) nl_strerror(e)

#else

#include <errno.h>
#include <string.h>
#include <stdio.h>

#define nl_strerror strerror
#define nl_perror perror

#endif

#endif /* NL_ERRNO_H */
