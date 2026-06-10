#ifndef PLATFORM_H
#define PLATFORM_H

#if defined(__linux__)
#define PLATFORM_LINUX 1
#elif defined(__APPLE__) && defined(__MACH__)
#define PLATFORM_MACOS 1
#else
#error "Unsupported platform (only Linux and macOS are supported)"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ARCH_ARM64 1
#else
#error "Unsupported architecture (only x86_64 and ARM64 are supported)"
#endif

#endif
