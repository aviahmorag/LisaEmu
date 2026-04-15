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
          $(SRCDIR)/profile.c \
          $(SRCDIR)/lisa.c \
          $(SRCDIR)/lisa_bridge.c \
          $(SRCDIR)/boot_progress.c \
          $(SRCDIR)/main_sdl.c

TOOL_SOURCES = $(TOOLDIR)/asm68k.c \
               $(TOOLDIR)/pascal_lexer.c \
               $(TOOLDIR)/pascal_parser.c \
               $(TOOLDIR)/pascal_codegen.c \
               $(TOOLDIR)/linker.c \
               $(TOOLDIR)/diskimage.c \
               $(TOOLDIR)/bootrom.c \
               $(TOOLDIR)/toolchain_fileset.c \
               $(TOOLDIR)/compile_targets.c \
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

# ======================================================================
# Toolchain Audit — run `make audit` to check all stages
# ======================================================================

AUDIT_SRC = $(TOOLDIR)/audit_toolchain.c \
            $(TOOLDIR)/pascal_parser.c \
            $(TOOLDIR)/pascal_lexer.c \
            $(TOOLDIR)/pascal_codegen.c \
            $(TOOLDIR)/asm68k.c \
            $(TOOLDIR)/linker.c \
            $(TOOLDIR)/toolchain_fileset.c

AUDIT = $(BUILDDIR)/audit_toolchain

$(AUDIT): $(AUDIT_SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(AUDIT_SRC)

# Full audit (all 4 stages)
audit: $(AUDIT)
	@echo "Running toolchain audit..."
	@./$(AUDIT) ./Lisa_Source 2>$(BUILDDIR)/audit_errors.txt
	@echo "Detailed errors in $(BUILDDIR)/audit_errors.txt"

# Individual stages
audit-parser: $(AUDIT)
	@./$(AUDIT) ./Lisa_Source parser 2>/dev/null

audit-codegen: $(AUDIT)
	@./$(AUDIT) ./Lisa_Source codegen 2>/dev/null

audit-asm: $(AUDIT)
	@./$(AUDIT) ./Lisa_Source asm 2>$(BUILDDIR)/audit_asm_errors.txt

audit-linker: $(AUDIT)
	@./$(AUDIT) ./Lisa_Source linker 2>$(BUILDDIR)/audit_linker_errors.txt

# Dump instruction at address from disk image
DUMP_ADDR = $(BUILDDIR)/dump_addr

$(DUMP_ADDR): $(TOOLDIR)/dump_addr.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $<

dump: $(DUMP_ADDR)
	@./$(DUMP_ADDR) $(BUILDDIR)/lisa_profile.image 3015C
