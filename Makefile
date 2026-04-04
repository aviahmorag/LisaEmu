# LisaEm - Apple Lisa Emulator
# Standalone SDL2 build (for testing outside Xcode)

CC = cc
CFLAGS = -Wall -Wextra -O2 -arch arm64
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs)

SRCDIR = src
TOOLDIR = src/toolchain
BUILDDIR = build

SOURCES = $(SRCDIR)/m68k.c \
          $(SRCDIR)/lisa_mmu.c \
          $(SRCDIR)/via6522.c \
          $(SRCDIR)/lisa.c \
          $(SRCDIR)/lisa_bridge.c \
          $(SRCDIR)/main_sdl.c

TOOL_SOURCES = $(TOOLDIR)/asm68k.c \
               $(TOOLDIR)/pascal_lexer.c \
               $(TOOLDIR)/pascal_parser.c \
               $(TOOLDIR)/pascal_codegen.c \
               $(TOOLDIR)/linker.c \
               $(TOOLDIR)/diskimage.c \
               $(TOOLDIR)/bootrom.c \
               $(TOOLDIR)/toolchain_bridge.c

OBJECTS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
TOOL_OBJECTS = $(patsubst $(TOOLDIR)/%.c,$(BUILDDIR)/%.o,$(TOOL_SOURCES))

TARGET = $(BUILDDIR)/lisaemu

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS) $(TOOL_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(SDL_LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c -o $@ $<

$(BUILDDIR)/%.o: $(TOOLDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

# Build without SDL (library only, for testing compilation)
lib: $(OBJECTS) $(TOOL_OBJECTS)

# Run
run: $(TARGET)
	./$(TARGET)
