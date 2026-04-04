# LisaEm - Apple Lisa Emulator
# Standalone SDL2 build (for testing outside Xcode)

CC = cc
CFLAGS = -Wall -Wextra -O2 -arch arm64
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs)

SRCDIR = src
BUILDDIR = build

SOURCES = $(SRCDIR)/m68k.c \
          $(SRCDIR)/lisa_mmu.c \
          $(SRCDIR)/via6522.c \
          $(SRCDIR)/lisa.c \
          $(SRCDIR)/lisa_bridge.c \
          $(SRCDIR)/main_sdl.c

OBJECTS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))

TARGET = $(BUILDDIR)/lisaemu

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(SDL_LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

# Build without SDL (library only, for testing compilation)
lib: $(BUILDDIR)/m68k.o $(BUILDDIR)/lisa_mmu.o $(BUILDDIR)/via6522.o $(BUILDDIR)/lisa.o $(BUILDDIR)/lisa_bridge.o

# Run
run: $(TARGET)
	./$(TARGET)
