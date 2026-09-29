#ifndef PLATFORM_H
#define PLATFORM_H

#if defined(__linux__)
#define PLATFORM_LINUX 1
#elif defined(__APPLE__) && defined(__MACH__)
#define PLATFORM_MACOS 1
#elif defined(_WIN32)
#define PLATFORM_WINDOWS 1
#else
#error "Unsupported platform (only Linux, macOS, and Windows are supported)"
#endif

/* Architecture */
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#define ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ARCH_ARM64 1
#else
#error "Supported architectures: x86-64, ARM64"
#endif

/* Endianness (compiler-provided on GCC/Clang, fallback below) */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define PLATFORM_LITTLE_ENDIAN 1
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PLATFORM_BIG_ENDIAN 1
#elif defined(_WIN32)
#define PLATFORM_LITTLE_ENDIAN 1
#endif

/* Compiler */
#if defined(__clang__)
#define COMPILER_CLANG 1
#define COMPILER_VERSION                                                       \
  (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)
#elif defined(__GNUC__)
#define COMPILER_GCC 1
#define COMPILER_VERSION                                                       \
  (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#elif defined(_MSC_VER)
#define COMPILER_MSVC 1
#define COMPILER_VERSION _MSC_VER
#else
#define COMPILER_UNKNOWN 1
#define COMPILER_VERSION 0
#endif

#endif
