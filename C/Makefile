# AsciiCam cross-platform build
# Supports:
#   Linux  x86_64  (gcc, nolibc, V4L2)
#   macOS  ARM64   (clang, system libc, AVFoundation)
#   macOS  x86_64  (clang, system libc, AVFoundation)

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
    CC       := gcc
    LD       := gcc
else ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
    CC       := clang
    LD       := clang
    OBJC     := clang
else
    $(error Unsupported platform: $(UNAME_S))
endif

SRCDIR    := src
LIBDIR    := lib
FILTERDIR := filters
BUILDDIR  := build
OBJDIR    := $(BUILDDIR)/obj

CFLAGS_COMMON := -Wall -Wextra -O2 -Iinclude -I$(LIBDIR) -fno-stack-protector -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-strlen
CFLAGS_COMMON += 

ifeq ($(PLATFORM),linux)
    # Linux: nolibc, V4L2, SSE4.1
    CFLAGS   := $(CFLAGS_COMMON) -DPLATFORM_LINUX -D__LINUX_NOLIBC__
    CFLAGS   += -msse4.1

    LDFLAGS  := -nostdlib
    LDFLAGS  += -Wl,--no-as-needed
    LDFLAGS  += -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2
    LDFLAGS  += $(shell $(CC) --print-file-name=crti.o)
    LDFLAGS  += $(shell $(CC) --print-file-name=crtn.o)
    LDFLAGS  += -L/usr/lib/x86_64-linux-gnu # TEST: Explicit library paths
    # LDFLAGS  += -Wl,-rpath-link=/usr/lib/x86_64-linux-gnu
    LDFLAGS  += -ldl -lpthread -lc
    # LDFLAGS  += -ldl -lpthread
    LDFLAGS  += -msse4.1

    LIBSRCS  := $(LIBDIR)/nl_alloc.c \
                $(LIBDIR)/nl_errno.c \
                $(LIBDIR)/nl_getopt.c \
                $(LIBDIR)/nl_printf.c \
                $(LIBDIR)/nl_start.c

    PLAT_SRC := $(SRCDIR)/capture_linux.c

    # Plugin shared objects
    SO_EXT   := so
    SO_FLAGS := -fPIC -shared
else ifeq ($(PLATFORM),macos)
    # macOS: system libc + AVFoundation, supports ARM64 + x86_64
    CFLAGS   := $(CFLAGS_COMMON) -DPLATFORM_MACOS
    OBJCFLAGS:= $(CFLAGS) -x objective-c \
                -fobjc-arc \
                -fmodules

    ifeq ($(UNAME_M),arm64)
        CFLAGS   += -arch arm64
        LDFLAGS  := -arch arm64
    else
        CFLAGS   += -arch x86_64 -msse4.1
        LDFLAGS  := -arch x86_64
    endif

    LDFLAGS  += -framework AVFoundation \
                -framework CoreMedia \
                -framework CoreVideo \
                -framework Foundation \
                -lpthread

    # TODO: Update nolibc for macos support instead of system system libc, currently :
    # No nolibc on macOS, uses system libc
    LIBSRCS  :=

    PLAT_SRC := $(SRCDIR)/capture_macos.m

    # Plugin shared objects
    SO_EXT   := dylib
    SO_FLAGS := -dynamiclib
endif

CORE_SRCS := $(SRCDIR)/ascii.c \
             $(SRCDIR)/main.c  \
             $(SRCDIR)/plugins.c \
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

.PHONY: all clean plugins info

all: $(TARGET) plugins

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
else
	$(CC) $(CFLAGS) $(SO_FLAGS) $< -o $@ \
		-undefined dynamic_lookup
endif

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

info:
	@echo "Platform : $(PLATFORM)"
	@echo "Arch     : $(UNAME_M)"
	@echo "CC       : $(CC)"
	@echo "CFLAGS   : $(CFLAGS)"
	@echo "LDFLAGS  : $(LDFLAGS)"
	@echo "Objects  : $(OBJECTS)"
