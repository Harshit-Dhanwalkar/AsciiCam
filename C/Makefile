CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude
LDFLAGS = -lm
LDFLAGS += -lpthread # Multi-threaded capture-render, pthreads producer/consumer
LDFLAGS += -msse4.1 # SIMD - SSE2 for the YUYV to gray conversion

SRCDIR = src
INCDIR = include
BUILDDIR = build
OBJDIR = $(BUILDDIR)/obj

SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
TARGET = $(BUILDDIR)/webcam_ascii

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(BUILDDIR)
