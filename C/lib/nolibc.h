#ifndef NOLIBC_H
#define NOLIBC_H

#define _SYS_SELECT_H 1
#define __FD_SETSIZE 1024

// IWYU pragma: begin_exports
#include "platform.h"

#include "nl_io.h"
#include "nl_alloc.h"
#include "nl_checked.h"
#include "nl_crc.h"
#include "nl_defer.h"
#include "nl_dlfcn.h"
#include "nl_elf.h"
#include "nl_errno.h"
#include "nl_gc.h"
#include "nl_getopt.h"
#include "nl_hash.h"
#include "nl_inotify.h"
#include "nl_math.h"
#include "nl_printf.h"
#include "nl_prof.h"
#include "nl_ramfs.h"
#include "nl_signal.h"
#if defined(__LINUX_NOLIBC__) && defined(__x86_64__)
#include "nl_seccomp.h"
#endif
#include "nl_sgr.h"
#include "nl_signal.h"
#include "nl_slice.h"
#include "nl_string.h"
#include "nl_syscall.h"
#include "nl_termios.h"
#include "nl_thread.h"
#include "nl_time.h"
#include "nl_types.h"
#include "nl_v4l2.h"
// IWYU pragma: end_exports

#endif
