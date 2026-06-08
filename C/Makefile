CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude -Ilib

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CFLAGS += -fno-stack-protector -D__LINUX_NOLIBC__
    # LDFLAGS = -lm
    LDFLAGS += -nostdlib # drops crt startup files and default libs
    # LDFLAGS += -nodefaultlibs # drops default libs but keeps crt startup files
    LDFLAGS += -Wl,--no-as-needed
    LDLIBS := -ldl -lpthread -lc
    # LDFLAGS += -Wl,-rpath,/lib/x86_64-linux-gnu
    # LDFLAGS += /lib/x86_64-linux-gnu/libdl.so.2
    LDFLAGS += -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2
    LDFLAGS += /usr/lib/x86_64-linux-gnu/crti.o
    LDFLAGS += /usr/lib/x86_64-linux-gnu/crtn.o
    LDFLAGS += -ldl # Dynamic loading symbols
    LDFLAGS += -lpthread # Multi-threaded capture-render, pthreads producer/consumer
    LDFLAGS += -lc
    LDFLAGS += -msse4.1 # SIMD - SSE2 for the YUYV to gray conversion
    # LDFLAGS += -static
    LIBSRCS = $(wildcard lib/*.c)
else ifeq ($(UNAME_S),Darwin)
    CFLAGS +=
    LDFLAGS +=
    LDLIBS :=
    LIBSRCS =
endif

SRCDIR = src
INCDIR = include
LIBDIR = lib
BUILDDIR = build
OBJDIR = $(BUILDDIR)/obj

LIBSRCS        = $(wildcard $(LIBDIR)/*.c)
SOURCES        = $(wildcard $(SRCDIR)/*.c) $(LIBSRCS)
OBJECTS        = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
TARGET         = $(BUILDDIR)/webcam_ascii
PLUGIN_SRCS    = $(wildcard filters/*.c)
PLUGIN_C_SRCS  = $(filter-out filters/%.h, $(PLUGIN_SRCS))
PLUGIN_TARGETS = $(patsubst filters/%.c,$(BUILDDIR)/%.so,$(PLUGIN_C_SRCS))

# Security Testing Targets
TEST_TARGET = $(BUILDDIR)/security_tests
TEST_SRC = ../tests/security_tests.c
FILTER_SRCS = filters/edge_detect.c filters/invert.c filters/threshold.c

.PHONY: all clean plugins test

all: $(TARGET) plugins

# Main binary
$(TARGET): $(OBJECTS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

# Plugins compilation
plugins: $(PLUGIN_TARGETS)

$(BUILDDIR)/%.so: filters/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@.tmp
	mv $@.tmp $@

# Automated Unit Tests Execution
test: $(BUILDDIR)
	$(CC) -I.. $(CFLAGS) -DTESTING $(TEST_SRC) $(FILTER_SRCS) -o $(TEST_TARGET) -lcheck -lsubunit -lm -lrt -lpthread
	@echo "   RUNNING ALL PLUGINS INVARIANT TEST    "
	./$(TEST_TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Cleanup
clean:
	rm -rf $(BUILDDIR)
