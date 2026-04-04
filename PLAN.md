# LisaEmu — Development Plan

## Goal

A native macOS app that takes Apple's Lisa OS 3.1 source code (`Lisa_Source/`, freely available from https://info.computerhistory.org/apple-lisa-code) as its sole input, compiles everything from source, and boots a fully working Apple Lisa emulator. No external ROM dumps or disk images needed.

## Current State (as of commit 9637327)

### What's Built and Working

#### Emulator Core (`src/`)
- **m68k.h/c** — Complete Motorola 68000 CPU emulator. Full instruction set (MOVE, ADD, SUB, CMP, AND, OR, EOR, shifts, rotates, BCD, MUL, DIV, all branch types), all addressing modes (Dn, An, (An), (An)+, -(An), d(An), d(An,Xn), abs, d(PC), #imm), supervisor/user mode, exceptions, interrupts, trace mode.
- **lisa_mmu.h/c** — Memory controller with MMU. 1MB RAM, 16KB ROM at $FE0000, I/O space at $FC0000, setup mode (ROM at $0 for boot), segment translation with 128 segments × 4 contexts.
- **via6522.h/c** — Two MOS 6522 VIA chips. VIA1 ($FCD801) for ProFile hard disk parallel interface, VIA2 ($FCDD81) for keyboard/COPS. Timers, shift register, interrupt flags, port I/O with callbacks.
- **lisa.h/c** — Main machine integration. Wires CPU ↔ memory ↔ VIAs ↔ keyboard ↔ mouse ↔ disk. COPS queue for keyboard/mouse events. ProFile and Sony floppy image loading. 720×364 monochrome display rendering to ARGB framebuffer. IRQ management. 60fps frame loop at 5MHz.
- **lisa_bridge.h/c** — Clean C API for Swift bridging (emu_init, emu_run_frame, emu_get_framebuffer, etc.)
- **main_sdl.c** — Standalone SDL2 frontend for testing. Keyboard/mouse mapping, display rendering.

#### macOS App (`lisaOS/`)
- SwiftUI app using Swift 6 conventions (`@Observable`, `@State`, modern APIs)
- `EmulatorViewModel.swift` — Drives emulation at 60fps via Timer, renders CGImage from framebuffer
- `LisaDisplayView.swift` — NSView subclass for keyboard/mouse capture, SwiftUI wrapper
- `ContentView.swift` — Toolbar (Load ROM, Mount ProFile, Mount Floppy, Power, Reset, Debug), file importers, CPU debugger sheet
- `lisaOSApp.swift` — App entry point with menu commands
- `Emulator/` — Copy of C files with bridging header. **Must stay in sync with `src/`.**
- Xcode project builds successfully (`xcodebuild -scheme lisaOS -destination 'generic/platform=macOS' build`)

#### Toolchain (`src/toolchain/`)
- **asm68k.h/c** — Two-pass 68000 cross-assembler for Lisa assembly syntax. Handles `.PROC`, `.FUNC`, `.DEF`, `.REF`, `.INCLUDE`, `.EQU`, `.MACRO/.ENDM`, `.IF/.ELSE/.ENDC`, `DC.x`, `DS.x`. Expression evaluator with $hex, @octal, %binary. Full MC68000 instruction set encoding. Tested on real Lisa OS assembly (PROCASM.TEXT) — all instructions parse, remaining errors are from unresolved `.INCLUDE` files.
- **pascal_lexer.h/c** — Tokenizer for Lisa Pascal. Keywords, identifiers, integers (decimal/$hex), reals, strings, operators, compiler directives (`{$...}` and `(*$...*)`), comments. Tested on real source: 0-1 errors per file across OS, libraries, and applications.
- **pascal_parser.h/c** — Recursive descent parser producing AST. UNIT/PROGRAM with INTERFACE/IMPLEMENTATION, USES with `{$U}` directives, CONST/TYPE/VAR/LABEL declarations, all type forms (simple, subrange, array, record, set, file, pointer, string, packed, enum), PROCEDURE/FUNCTION with params and EXTERNAL/FORWARD, all statement types (assignment, if, while, repeat, for, case, with, goto, begin/end), full expression precedence. Tested on real Lisa OS source with 0-1 errors per file.
- **pascal_codegen.h/c** — Code generator: AST → 68000 machine code. Lisa Pascal calling convention (A5=globals, A6=frame, LINK/UNLK). Local/global/param variable access, integer arithmetic/comparisons, if/while/repeat/for code generation, function calls with arg pushing, string literals, type system, symbol table, relocation entries, Lisa object file output. Tested on simple units — generates valid 68000 code.

#### Tests (`src/toolchain/test_*.c`)
- `test_asm.c` — Tests assembler on built-in source or file argument
- `test_lexer.c` — Tests lexer, prints token stream
- `test_parser.c` — Tests parser, prints AST
- `test_codegen.c` — Tests full pipeline (lex → parse → codegen), prints code hex dump + symbols

#### Documentation (`docs/`)
- **LISA_SOURCE_MAP.md** — Complete catalog of all 1,280 files in Lisa_Source. Every directory, every library, every application, every OS subsystem documented. File counts, descriptions, build scripts, linkmaps.
- **HARDWARE_SPECS.md** — Full Lisa hardware spec derived from source: CPU, memory map, I/O registers, VIA addresses, display, keyboard, mouse, disk controllers, interrupts, MMU, timing.
- **TOOLCHAIN.md** — What the original build tools were, what we need to build, build order, object file format, pragmatic bootstrap strategy.

### Build Commands
```bash
cd /Volumes/Projects/Repos/LisaEmu
make                                    # Build SDL2 standalone emulator
./build/lisaemu [rom] [profile] [floppy] # Run standalone

# Build and run toolchain tests:
cc -Wall -O2 -o build/test_asm src/toolchain/asm68k.c src/toolchain/test_asm.c
cc -Wall -O2 -o build/test_lexer src/toolchain/pascal_lexer.c src/toolchain/test_lexer.c
cc -Wall -O2 -o build/test_parser src/toolchain/pascal_lexer.c src/toolchain/pascal_parser.c src/toolchain/test_parser.c
cc -Wall -O2 -o build/test_codegen src/toolchain/pascal_lexer.c src/toolchain/pascal_parser.c src/toolchain/pascal_codegen.c src/toolchain/test_codegen.c

# Test on real Lisa source:
./build/test_asm "Lisa_Source/LISA_OS/OS/source-PROCASM.TEXT.unix.txt"
./build/test_parser "Lisa_Source/LISA_OS/OS/source-SCHED.TEXT.unix.txt"
./build/test_codegen "Lisa_Source/LISA_OS/LIBS/LIBSM/LIBSM-UNITSTD.TEXT.unix.txt"

# Xcode build:
cd lisaOS && xcodebuild -scheme lisaOS -destination 'generic/platform=macOS' build 2>&1 | grep -E "(error:|BUILD)"
```

### Git / GitHub
- Repo: `github.com/aviahmorag/LisaEmu` (under aviahmorag personal account)
- Branch: `main`
- `Lisa_Source/` is gitignored (Apple license prohibits redistribution)
- `.claude/` is gitignored
- Commit messages: no Claude attribution (per user preference)

---

## What Needs To Be Done (in order)

### Phase 1: Error-Free Toolchain

The toolchain only needs to work for THIS specific set of ~1,280 files. Every edge case is knowable and fixable.

#### 1a. Fix all parser errors
- Run `test_parser` on every Pascal source file in Lisa_Source
- Catalog every error (expected: a handful of edge cases)
- Fix each one — likely issues:
  - Lisa Pascal has `(* parameter comment *)` syntax in implementation sections (e.g., `procedure AddTime (* counter : int2 *);` where params are in comments)
  - Variant records with CASE inside RECORD
  - Inline assembly (`INLINE` directive)
  - Conditional compilation (`{$IFC}` / `{$ENDC}`) affecting parsing
- **Acceptance: zero parser errors across all Pascal source files**

#### 1b. Fix assembler include resolution
- The `.INCLUDE source/pascaldefs.text` directive needs to resolve to actual files
- Map Lisa-style paths (e.g., `source/pascaldefs.text`) to the archive paths (e.g., `LISA_OS/OS/source-PASCALDEFS.TEXT.unix.txt`)
- Build a path mapping table for the source tree
- **Acceptance: zero assembler errors on all assembly files when includes are resolved**

#### 1c. Fix all codegen errors
- Run full pipeline on every Pascal source file
- Fix crashes (likely from large structs on stack — use heap allocation)
- Handle missing code patterns (WITH statement full implementation, CASE jump tables, set operations, string operations, real arithmetic)
- **Acceptance: every Pascal unit compiles to a .OBJ file without errors**

### Phase 2: Linker

**File: `src/toolchain/linker.h` and `linker.c`**

The linker takes multiple .OBJ files and produces a linked executable.

Key inputs:
- Lisa object files (.OBJ) with segments, modules, entries, and relocations
- Linklist files (from `LISA_OS/BUILD/`) that specify which objects to link
- Intrinsic library format (.lib) — container for runtime library units

Key outputs:
- Linked executables (system.os, application binaries)
- Linkmap output (can be compared against `LISA_OS/Linkmaps 3.0/` for validation)

What it must handle:
- Segment assignment and layout (24 segments for OS kernel)
- Symbol resolution across modules
- Relocation patching (absolute and PC-relative)
- Jump table generation (segment #1 in the OS kernel)
- Global data area layout
- The IUManager functionality (building intrinsic.lib)

Build order (from `LISA_OS/BUILD/BUILD-LIBRARY.TEXT`):
1. `iospaslib` — I/O and Pascal runtime
2. `iosfplib` — Floating point (196 modules)
3. `sys1lib` — QuickDraw, Window Manager, Font Manager (1069 modules, 854 entries)
4. `sys2lib` — Additional system support
5. `prlib` — Printing
6. `tklib` — Toolkit
7. `system.os` — OS kernel (46 objects → 871 modules, 25 segments)
8. Each application (Filer, LisaCalc, LisaDraw, LisaWrite, etc.)

**Validation: compare our linkmaps against the reference linkmaps in `LISA_OS/Linkmaps 3.0/`**

### Phase 3: Disk Image Builder

**File: `src/toolchain/diskimage.h` and `diskimage.c`**

Creates a bootable Lisa disk image from linked executables.

Must implement:
- Lisa filesystem format (directory structure, file catalog)
- ProFile disk image format (532 bytes/block: 512 data + 20 tag)
- Boot track writing (device-specific: `system.bt_Profile`, `system.bt_Sony`)
- File installation layout (from `LISA_OS/BUILD/BUILD-NEWRELEASE.TEXT`):
  - `system.os` — linked kernel
  - `system.shell` — environment shell
  - `intrinsic.lib` — runtime libraries
  - Device configs (`cd_*`, `ch_*`, `cdconfig`, `cdchar`)
  - System calls (`psyscall.obj`, `syscall.obj`)
  - Application binaries (Filer, LisaCalc, etc.)
  - Font files (57 .F files from `LISA_OS/FONTS/`)

### Phase 4: App Integration

Wire the entire toolchain into the Xcode macOS app:

- Add a "Build from Source" workflow in the UI
- User selects their Lisa_Source directory
- App runs: assemble → compile → link → build disk image → boot
- Progress UI showing which file is being compiled
- Error reporting if any step fails
- Once built, the disk image is cached so subsequent launches are instant

### Phase 5: Emulator Hardening

Boot the compiled OS and fix issues iteratively:

- Each crash or hang reveals a missing CPU instruction, wrong I/O behavior, or timing issue
- Compare against known Lisa behavior (documented in source comments)
- Key milestones:
  1. ROM code executes (startup sequence begins)
  2. MMU initializes, memory test passes
  3. Disk driver loads OS kernel
  4. OS initializes, process scheduler starts
  5. Desktop Manager (Filer) launches
  6. Display shows Lisa desktop with icons
  7. Mouse and keyboard work
  8. Applications launch and function

---

## Key Files in Lisa_Source

### For understanding the build:
- `LISA_OS/BUILD/BUILD-LIBRARY.TEXT.unix.txt` — Library build script
- `LISA_OS/BUILD/BUILD-MAKE-ASYS1LIB.TEXT.unix.txt` — sys1lib make script (lists all source files)
- `LISA_OS/OS exec files/BUILD-LINKLIST.TEXT.unix.txt` — OS kernel link list (46 objects)
- `LISA_OS/OS exec files/BUILD-LINKMAP.TEXT.unix.txt` — OS kernel linkmap output
- `LISA_OS/OS exec files/BUILD-NEWRELEASE.TEXT.unix.txt` — Release disk creation
- `LISA_OS/Linkmaps 3.0/` — Reference linkmaps for all binaries

### For understanding the hardware (emulator reference):
- `LISA_OS/LIBS/LIBHW/` — 14 files defining ALL hardware interfaces
- `LISA_OS/OS/source-PASCALDEFS.TEXT.unix.txt` — Assembly constants (memory map, MMU, I/O)
- `LISA_OS/OS/source-DRIVERDEFS.TEXT.unix.txt` — Driver data structures
- `LISA_OS/OS/source-SYSGLOBAL.TEXT.unix.txt` — System global data

### Counts:
- 121 OS kernel source files
- 425 library source files (21 libraries)
- 271 application source files (13 apps)
- 57 binary font files (ready to use)
- 8 pre-compiled .OBJ files (Toolkit utilities)
- 63 build scripts and linkmaps
- **Total: ~1,280 files, ALL source except 65 binaries (fonts + toolkit OBJs)**

---

## Architecture Notes

### Lisa Pascal Calling Convention
- Parameters pushed right-to-left on stack
- A5 = global data pointer
- A6 = frame pointer (LINK A6,#-localsize / UNLK A6)
- A7 = stack pointer
- Function results: byte/word in D0, long in D0, real on stack, records/strings via hidden pointer
- Caller cleans up parameters after call
- VAR parameters passed by reference (pointer on stack)

### Lisa Object File Format (our format, `LOBJ`)
Currently a simple format: magic + version + code_size + num_symbols + code + symbols + relocations. Will need to evolve to match the real Lisa object format for linker compatibility.

### Important: Keep C files in sync
The C emulator/toolchain code lives in `src/` (canonical) and is copied to `lisaOS/lisaOS/Emulator/` for the Xcode build. After editing `src/`, copy to Xcode:
```bash
cp src/*.h src/*.c lisaOS/lisaOS/Emulator/
```

### Swift conventions
- Swift 6, `@Observable` (not ObservableObject), `@State` (not @StateObject)
- `.foregroundStyle` not `.foregroundColor`
- `fileImporter` not custom picker sheets
- Target: arm64 Apple Silicon (M4 Mac)
