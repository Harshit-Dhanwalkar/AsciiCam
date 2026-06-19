#ifndef NOLIBC_H
#define NOLIBC_H

#define _SYS_SELECT_H 1
#define __FD_SETSIZE 1024

// IWYU pragma: begin_exports
#include "nl_alloc.h"
#include "nl_errno.h"
#include "nl_getopt.h"
#include "nl_io.h"
#include "nl_printf.h"
#include "nl_signal.h"
#include "nl_string.h"
#include "nl_syscall.h"
// IWYU pragma: end_exports

#endif
