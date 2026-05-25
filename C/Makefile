CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude
LDFLAGS = -lm
LDFLAGS += -lpthread # Multi-threaded capture-render, pthreads producer/consumer
LDFLAGS += -ldl # Dynamic loading symbols
LDFLAGS += -msse4.1 # SIMD - SSE2 for the YUYV to gray conversion
# LDFLAGS += -fvisibility=hidden

SRCDIR = src
INCDIR = include
BUILDDIR = build
OBJDIR = $(BUILDDIR)/obj

SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
TARGET = $(BUILDDIR)/webcam_ascii
PLUGIN_SRCS   = $(wildcard filters/*.c)
PLUGIN_TARGETS = $(patsubst filters/%.c,$(BUILDDIR)/%.so,$(PLUGIN_SRCS))


.PHONY: all clean plugins

all: $(TARGET) plugins

# Main binary
$(TARGET): $(OBJECTS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

# Plugins
plugins: $(PLUGIN_TARGET)

$(BUILDDIR)/%.so: filters/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@.tmp
	mv $@.tmp $@

plugins: $(PLUGIN_TARGETS)


$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Cleanup
clean:
	rm -rf $(BUILDDIR)
