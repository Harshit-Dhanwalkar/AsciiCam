#ifndef PLUGIN_BUILD_H
#define PLUGIN_BUILD_H

/*
 * How a plugin is compiled: one definition, used by both build program
 * (build.c, `./nb plugins`) and running app (plugin_catalog.c, picker builds on
 * demand), so a plugin compiled on fly is byte-for-byte what a normal build
 * would have produced. Only macros: it is safe to include from anywhere. Flags
 * are one space-separated string per group
 *
 * App needs no make: it runs C compiler directly (see PB_SCRIPT_*)
 * Compiler is $CC when set, else cc
 */

#define PB_COMMON                                                              \
  "-Wall -Wextra -O2 -fno-stack-protector -fno-builtin -ffreestanding"

// Host decides compiler flags, not PLATFORM_* define (which catalog test sets
// on a Linux host to exercise POSIX code)
#if defined(__APPLE__)
#if defined(__x86_64__)
#define PB_TARGET "-DPLATFORM_MACOS -msse4.1"
#else
#define PB_TARGET "-DPLATFORM_MACOS"
#endif
#define PB_SHARED "-dynamiclib -undefined dynamic_lookup"
// macOS: system headers are fine
#define PB_SYSTEM_HEADERS ""
#define PB_SO_EXT "dylib"
#else
// Linux (nolibc): no system headers at all, only compiler's own freestanding
// directory (immintrin.h ...), found with -print-file-name
#define PB_TARGET "-DPLATFORM_LINUX -D__LINUX_NOLIBC__ -msse4.1 -nostdinc"
#define PB_SHARED "-fPIC -shared -Wl,--hash-style=both"
#define PB_SYSTEM_HEADERS " -isystem \"$(\"$cc\" -print-file-name=include)\""
#define PB_SO_EXT "so"
#endif

/*
 * Shell script run as: sh -c PB_SCRIPT sh <source.c> <output> <sdk dir>
 * ($1 source, $2 output, $3 directory holding include/ and lib/). Writes to a
 * temporary name and renames, so a watcher never sees half a library
 */
#define PB_SCRIPT                                                              \
  "cc=\"${CC:-cc}\"; "                                                         \
  "\"$cc\" " PB_COMMON " " PB_TARGET                                           \
  " -I\"$3/include\" -I\"$3/lib\"" PB_SYSTEM_HEADERS " " PB_SHARED             \
  " \"$1\" -o \"$2.tmp\" && mv -f \"$2.tmp\" \"$2\""

#endif
