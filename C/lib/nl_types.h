#ifndef NL_TYPES_H
#define NL_TYPES_H

/*
 * nl_types.h - freestanding base types for nolibc layer
 */

#ifdef __LINUX_NOLIBC__

/* Core sizes */
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef long ssize_t;
typedef long off_t;

/* Fixed-width integers */
typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __INTPTR_TYPE__ intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTMAX_TYPE__ intmax_t;
typedef __UINTMAX_TYPE__ uintmax_t;

typedef int sig_atomic_t;

/* Limits */
#define INT8_MAX __INT8_MAX__
#define INT16_MAX __INT16_MAX__
#define INT32_MAX __INT32_MAX__
#define INT64_MAX __INT64_MAX__
#define INT8_MIN (-INT8_MAX - 1)
#define INT16_MIN (-INT16_MAX - 1)
#define INT32_MIN (-INT32_MAX - 1)
#define INT64_MIN (-INT64_MAX - 1)
#define UINT8_MAX __UINT8_MAX__
#define UINT16_MAX __UINT16_MAX__
#define UINT32_MAX __UINT32_MAX__
#define UINT64_MAX __UINT64_MAX__
#define SIZE_MAX __SIZE_MAX__
#define INTPTR_MAX __INTPTR_MAX__
#define UINTPTR_MAX __UINTPTR_MAX__

#define INT_MAX __INT_MAX__
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX (__INT_MAX__ * 2U + 1U)
#define LONG_MAX __LONG_MAX__
#define LONG_MIN (-LONG_MAX - 1L)

/* NULL / offsetof */
#ifndef NULL
#define NULL ((void *)0)
#endif
#define offsetof(type, member) __builtin_offsetof(type, member)

/* Variadics: the compiler builtins */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap) __builtin_va_end(ap)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_copy(dst, src) __builtin_va_copy(dst, src)

#else /* macOS / Windows: system headers */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#endif /* __LINUX_NOLIBC__ */

#endif /* NL_TYPES_H */
