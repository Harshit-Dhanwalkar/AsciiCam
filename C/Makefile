# AsciiCam cross-platform build
# Supports:
#   Linux    x86_64  (gcc,   nolibc,      V4L2)
#   macOS    ARM64   (clang, system libc, AVFoundation)
#   macOS    x86_64  (clang, system libc, AVFoundation)
#   Windows          (gcc,   ststem libc, MinGW-w64)

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# ARM64 macOS
TARGET_ARCH ?= $(UNAME_M)

ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
    CC       := gcc
    LD       := gcc
else ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
    CC       := clang
    LD       := clang
    OBJC     := clang
else ifneq (,$(filter MINGW% MSYS%,$(UNAME_S)))
    PLATFORM := windows
    CC       := gcc
    LD       := gcc
else
    $(error Unsupported platform: $(UNAME_S))
endif

SRCDIR    := src
LIBDIR    := lib
FILTERDIR := filters
BUILDDIR  := build
OBJDIR    := $(BUILDDIR)/obj

CFLAGS_COMMON := -Wall -Wextra -O2 -Iinclude -I$(LIBDIR) -fno-stack-protector -fno-builtin -ffreestanding
CFLAGS_EXTRA  ?=
LDFLAGS_EXTRA ?=

ifeq ($(PLATFORM),linux)
    # Linux: nolibc, V4L2, SSE4.1
    CFLAGS   := $(CFLAGS_COMMON) $(CFLAGS_EXTRA) -DPLATFORM_LINUX -D__LINUX_NOLIBC__
    CFLAGS   += -msse4.1
    # No system headers at all: only compiler's own freestanding/intrinsics directory
    # (immintrin.h etc.) stays on include path
    NOSTDINC := -nostdinc -isystem $(shell $(CC) -print-file-name=include)
    CFLAGS   += $(NOSTDINC)
    LDFLAGS  := $(LDFLAGS_EXTRA) -nostdlib -nostdinc \
                -Wl,--no-as-needed \
                -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
                -B/usr/bin/ \
                -L/usr/lib/x86_64-linux-gnu
    # LDFLAGS  += -Wl,-rpath-link=/usr/lib/x86_64-linux-gnu
    LDFLAGS  += -static
    LDFLAGS  += -msse4.1

    LIBSRCS  := $(LIBDIR)/nl_alloc.c \
                $(LIBDIR)/nl_dlfcn.c \
                $(LIBDIR)/nl_errno.c \
                $(LIBDIR)/nl_getopt.c \
                $(LIBDIR)/nl_printf.c \
                $(LIBDIR)/nl_start.c

    PLAT_SRC := $(SRCDIR)/capture_linux.c

    # Plugin shared objects
    SO_EXT   := so
    SO_FLAGS := -fPIC -shared -Wl,--hash-style=both
else ifeq ($(PLATFORM),macos)
    # macOS: system libc + AVFoundation, supports ARM64 + x86_64
    CFLAGS   := $(CFLAGS_COMMON) $(CFLAGS_EXTRA) -DPLATFORM_MACOS

    ifeq ($(TARGET_ARCH),arm64)
        CFLAGS   += -arch arm64
        LDFLAGS  := $(LDFLAGS_EXTRA) -arch arm64
    else
        CFLAGS   += -arch x86_64 -msse4.1
        LDFLAGS  := $(LDFLAGS_EXTRA) -arch x86_64
    endif

    OBJCFLAGS := $(CFLAGS) -x objective-c -fobjc-arc -fmodules

    LDFLAGS  += -framework AVFoundation \
                -framework CoreMedia \
                -framework CoreVideo \
                -framework Foundation \
                -lpthread

    # TODO: Update nolibc for macos support instead of system system libc, currently :
    # On macOS uses system libc
    LIBSRCS  :=

    PLAT_SRC := $(SRCDIR)/capture_macos.m

    # Plugin shared objects
    SO_EXT   := dylib
    SO_FLAGS := -dynamiclib
else ifeq ($(PLATFORM),windows)
    # Windows: system libc (MSVCRT via MinGW), Media Foundation
    CFLAGS   := $(CFLAGS_COMMON) $(CFLAGS_EXTRA) -DPLATFORM_WINDOWS -DWIN32_LEAN_AND_MEAN \
                -DUNICODE -D_UNICODE -D__USE_MINGW_ANSI_STDIO=1 \
                -msse4.1
    CFLAGS   := $(filter-out -ffreestanding -fno-builtin,$(CFLAGS))

    LDFLAGS  := $(LDFLAGS_EXTRA) -lmfplat -lmf -lmfreadwrite -lmfuuid \
                -lole32 -loleaut32 -luuid -lstrmiids -lwinpthread

    # TODO: Update nolibc for windows support instead of system system libc, currently :
    # No nolibc on Windows, uses system libc
    LIBSRCS  :=

    PLAT_SRC := $(SRCDIR)/capture_windows.c

    # Plugin shared objects
    SO_EXT   := dll
    SO_FLAGS := -shared
endif

CORE_SRCS := $(SRCDIR)/ascii.c \
             $(SRCDIR)/capture_reinit.c \
             $(SRCDIR)/main.c  \
             $(SRCDIR)/mouse.c \
             $(SRCDIR)/plugins.c \
             $(SRCDIR)/plugin_catalog.c \
             $(SRCDIR)/plugin_picker.c \
             $(SRCDIR)/thread_sharing.c \
             $(SRCDIR)/timing.c

ALL_C_SRCS := $(CORE_SRCS) $(PLAT_SRC) $(LIBSRCS)

OBJECTS := $(patsubst $(SRCDIR)/%.c,  $(OBJDIR)/%.o,   $(CORE_SRCS))
OBJECTS += $(patsubst $(SRCDIR)/%.c,  $(OBJDIR)/%.o,   $(filter $(SRCDIR)/%.c,  $(PLAT_SRC)))
OBJECTS += $(patsubst $(SRCDIR)/%.m,  $(OBJDIR)/%.o,   $(filter $(SRCDIR)/%.m,  $(PLAT_SRC)))
OBJECTS += $(patsubst $(LIBDIR)/%.c,  $(OBJDIR)/lib_%.o, $(LIBSRCS))

TARGET := $(BUILDDIR)/webcam_ascii

# Plugins
PLUGIN_SRCS    := $(wildcard $(FILTERDIR)/*.c)
PLUGIN_TARGETS := $(patsubst $(FILTERDIR)/%.c, $(BUILDDIR)/%.$(SO_EXT), $(PLUGIN_SRCS))

.PHONY: all run plugins clean sanitize sanitize-run info

all: $(TARGET) plugins

run: $(TARGET) plugins
	./$(TARGET) $(ARGS)

$(TARGET): $(OBJECTS) | $(BUILDDIR)
	$(LD) $^ -o $@ $(LDFLAGS)
	@echo "  LD  $@  ($(PLATFORM)/$(UNAME_M))"

# Compile .c
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile .m (Objective-C for macOS capture)
$(OBJDIR)/%.o: $(SRCDIR)/%.m | $(OBJDIR)
	$(OBJC) $(OBJCFLAGS) -c $< -o $@

# Compile nolibc (Linux only)
$(OBJDIR)/lib_%.o: $(LIBDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Plugins
plugins: $(PLUGIN_TARGETS)

$(BUILDDIR)/%.$(SO_EXT): $(FILTERDIR)/%.c | $(BUILDDIR)
ifeq ($(PLATFORM),linux)
	$(CC) $(CFLAGS) $(SO_FLAGS) $< -o $@.tmp && mv $@.tmp $@
else ifeq ($(PLATFORM),macos)
	$(CC) $(CFLAGS) $(SO_FLAGS) $< -o $@ -undefined dynamic_lookup
else
	$(CC) $(CFLAGS) $(SO_FLAGS) $< -o $@
endif

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

# make sanitize  (Linux x86_64 only)
# Builds with AddressSanitizer + UndefinedBehaviorSanitizer
ifeq ($(PLATFORM),linux)
ASAN_OBJDIR  := $(BUILDDIR)/obj_asan
ASAN_TARGET  := $(BUILDDIR)/webcam_ascii_asan

ASAN_CFLAGS  := $(CFLAGS_COMMON) \
                -DPLATFORM_LINUX -D__LINUX_NOLIBC__ -msse4.1 \
                -DNL_SANITIZE_BUILD \
                $(NOSTDINC) \
                -O1 -g -fno-omit-frame-pointer \
                -fsanitize=address,undefined \
                -fno-sanitize=vptr

ASAN_LDFLAGS := -nostdlib -nostdinc \
                -Wl,--no-as-needed \
                -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
                -L/usr/lib/x86_64-linux-gnu \
                -msse4.1 \
                -fsanitize=address,undefined \
                -fno-sanitize=vptr \
                -lasan -lubsan \
                -lc

ASAN_SRCS := $(CORE_SRCS) $(PLAT_SRC) $(LIBSRCS)

ASAN_OBJECTS := $(patsubst $(SRCDIR)/%.c, $(ASAN_OBJDIR)/%.o,     $(filter $(SRCDIR)/%.c, $(ASAN_SRCS)))
ASAN_OBJECTS += $(patsubst $(LIBDIR)/%.c, $(ASAN_OBJDIR)/lib_%.o, $(filter $(LIBDIR)/%.c, $(ASAN_SRCS)))

$(ASAN_OBJDIR):
	mkdir -p $(ASAN_OBJDIR)

$(ASAN_OBJDIR)/%.o: $(SRCDIR)/%.c | $(ASAN_OBJDIR)
	$(CC) $(ASAN_CFLAGS) -c $< -o $@

$(ASAN_OBJDIR)/lib_%.o: $(LIBDIR)/%.c | $(ASAN_OBJDIR)
	$(CC) $(ASAN_CFLAGS) -c $< -o $@

$(ASAN_TARGET): $(ASAN_OBJECTS) | $(BUILDDIR)
	# $(LD) $(ASAN_OBJECTS) -o $@ $(ASAN_LDFLAGS)
	PATH=/usr/bin:/bin $(LD) $(ASAN_OBJECTS) -o $@ $(ASAN_LDFLAGS)
	@echo "  LD [sanitize] $@"

sanitize: $(ASAN_TARGET)
	@echo "Sanitize build: $(ASAN_TARGET)"
	@echo "Run: ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./$(ASAN_TARGET) --help"

sanitize-run: $(ASAN_TARGET)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
	  ./$(ASAN_TARGET) $(ARGS)

.PHONY: sanitize sanitize-run
endif

info:
	@echo "Platform : $(PLATFORM)"
	@echo "Arch     : $(UNAME_M)"
	@echo "CC       : $(CC)"
	@echo "CFLAGS   : $(CFLAGS)"
	@echo "LDFLAGS  : $(LDFLAGS)"
	@echo "Objects  : $(OBJECTS)"
