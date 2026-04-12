# LisaEmu - Apple Lisa Emulator

## Project Vision

A native macOS app that takes Apple's officially released Lisa OS source code (`Lisa_Source/`) as input, compiles/assembles it into runnable binaries, and runs a complete Apple Lisa system — like Parallels, but for the Lisa.

The source code (Lisa OS version 3.1) was released by Apple in 2018 via the Computer History Museum:
https://info.computerhistory.org/apple-lisa-code

It is freely available under an Apple Academic License but **cannot be redistributed**. Users supply their own copy.

## Architecture

### Two layers:

1. **Toolchain** — Cross-assembler (68000) + Lisa Pascal cross-compiler + linker that processes `Lisa_Source/` into bootable disk images and ROM
2. **Emulator** — Motorola 68000 CPU + Lisa hardware emulation (memory, VIAs, display, keyboard, mouse, disks)

### Directory Structure

```
/
├── Lisa_Source/           # Apple's source (NOT in git, user-supplied)
├── src/                   # C emulator core + toolchain (canonical source)
│   ├── m68k.h/c           # Motorola 68000 CPU emulator
│   ├── lisa_mmu.h/c       # Memory controller + MMU
│   ├── via6522.h/c        # VIA 6522 chip emulation (x2)
│   ├── lisa.h/c           # Main machine integration
│   ├── lisa_bridge.h/c    # C-to-Swift bridge API
│   ├── main_sdl.c         # Standalone SDL2 frontend (for testing)
│   └── toolchain/         # Cross-compilation pipeline
│       ├── pascal_lexer.h/c      # Lisa Pascal tokenizer
│       ├── pascal_parser.h/c     # Recursive descent parser → AST
│       ├── pascal_codegen.h/c    # AST → 68000 machine code
│       ├── asm68k.h/c            # Two-pass 68000 cross-assembler
│       ├── linker.h/c            # Multi-module linker
│       ├── bootrom.c             # Boot ROM generator
│       ├── diskimage.h/c         # Disk image builder
│       ├── toolchain_bridge.h/c  # Orchestrates full compile pipeline
│       ├── audit_toolchain.c     # Diagnostic tool (make audit)
│       └── test_*.c              # Per-component test tools
├── lisaOS/                # Xcode macOS app (SwiftUI, Swift 6)
│   └── lisaOS/
│       ├── Emulator/      # SYMLINKS to src/ (not copies!)
│       ├── ContentView.swift
│       ├── EmulatorViewModel.swift
│       ├── LisaDisplayView.swift
│       └── lisaOSApp.swift
├── docs/                  # Documentation (Lisa_Source reference, hardware specs)
├── build/                 # Build output (gitignored)
├── Makefile               # Standalone SDL2 build + audit targets
├── CLAUDE.md              # This file
└── NEXT_SESSION.md        # Current status and prioritized fix list
```

## Key Commands

```bash
# Build standalone emulator (SDL2)
make

# Run toolchain audit — the primary diagnostic tool
make audit              # Full report (all 4 stages)
make audit-parser       # Stage 1: Parser only
make audit-codegen      # Stage 2: Codegen only
make audit-asm          # Stage 3: Assembler only
make audit-linker       # Stage 4: Full pipeline + linker

# Xcode build (or just open in Xcode)
cd lisaOS && xcodebuild -scheme lisaOS -destination 'generic/platform=macOS' build 2>&1 | grep -E "(error:|BUILD)"
```

## Current Status (2026-04-12)

### Prebuilt image (`prebuilt/los_compilation_base.image`)

Boots through MMU init (280+ TRAP #6), COPS handshake, ProFile driver,
Lisabug DB_INIT bypass (IPL-gated `$234` HLE), `$4FBC` NOP-skip.
After `G` at Lisabug prompt, reaches OS LOADER → **SYSTEM ERROR 10100**
(`Setup_IUInfo` can't open IU directory file). **Disk I/O / HLE issue.**

### Source compile (`Lisa_Source/` → disk image)

Toolchain: 317 Pascal + 103 ASM files → linked binary → disk image.
Boot reaches PASCALINIT → INITSYS → POOL_INIT → INIT_FREEPOOL (writes
pool header to sysglobal heap at $CCA000) → MM_INIT → GETSPACE.
Currently **SYSTEM_ERROR(10701)** — GETSPACE fails because the pool
header written by INIT_FREEPOOL has corrupt values. The pool_size and
firstfree fields contain wrong data, likely from another store-width
overflow or parameter computation bug in POOL_INIT.

**Key progress (2026-04-12):**
- Fixed compile order: PRIM files now compile before consumers (MM0, DS0)
- Fixed field offsets: sdb.memaddr, mmrb.head_sdb now correct (INSERTSDB works)
- Fixed store-width: non-pointer assignments no longer overflow (MOVE.W for int2)
- Fixed global offset reuse: imported globals keep their assigned A5 offsets

### Toolchain metrics
- Parser: **100%** (317/317), Assembler: **100%** (103/103)
- Linker: 8527 symbols, output ~2.2 MB
- Codegen: P1 ptr-trunc-load fixed, P2 ptr-trunc-store fixed,
  P3 ptr-deref-field fixed, P4 compile-order + store-width fixed

### Key HLE mechanisms
- `$234` fetch bypass: IPL=7→RTE (DB_INIT skip), IPL=0→execute (Lisabug)
- `$4FBC` NOP-skip: illegal opcode in Workshop init loop (13 hits)
- ProFile HLE: intercepts CALLDRIVER, reads from disk image
- INTSON/INTSOFF: manages IPL for compiled OS code

### Debug infrastructure
- `DBGSTATIC` macro + `g_emu_generation`: all debug statics reset on power cycle
- Bounded print budgets on all diagnostic output
- `VEC-FIRST` per-vector first-fire trace, periodic DIAG frame dumps
- 256-entry PC ring for crash analysis

### TODO
- **XPC process isolation**: run emulator in child process so power
  cycle = process kill + restart (eliminates static-state leaks)

See `.claude-handoffs/` for per-session handoffs. Run `make audit` for
toolchain metrics.


## Lisa_Source Reference

See `docs/LISA_SOURCE_MAP.md` for the complete catalog (~1,280 files).
See `docs/HARDWARE_SPECS.md` for hardware specifications derived from source.
See `docs/TOOLCHAIN.md` for the compilation pipeline needed to build from source.

Key facts:
- **Version**: Lisa OS 3.1 (Office System), circa 1983-1984
- **Languages**: Motorola 68000 assembly + Lisa Pascal (Apple's custom Pascal dialect)
- **~1,280 files** across OS kernel, 21 libraries, 13 applications, fonts, toolkit
- Contains 8 pre-compiled .OBJ files (68000 binaries) and 57 binary font files
- Build scripts in `LISA_OS/BUILD/` and `LISA_OS/OS exec files/` describe the full build process
- Linkmaps in `LISA_OS/Linkmaps 3.0/` show exact segment layout of every linked binary
- No pre-built ROM images or bootable disk images — everything must be compiled from source

## Hardware Specs (from source analysis)

| Component | Specification |
|-----------|---------------|
| CPU | Motorola 68000, 5 MHz |
| RAM | 1 MB (24-bit address bus) |
| Display | 720 x 364, monochrome bitmap |
| ROM | 16 KB at $FE0000 |
| I/O Base | $FC0000 |
| VIA1 | $FCD801 — Parallel port / ProFile hard disk |
| VIA2 | $FCDD81 — Keyboard / COPS (mouse, clock, power) |
| Video | Dual page, base at $7A000, contrast latch at $FCD01C |
| Interrupts | 7 levels (M68000 standard), VIA-based |
| Storage | Twiggy floppy, Sony 3.5" floppy, ProFile hard disk (5/10 MB) |
| Keyboard | 128 keys via COPS microcontroller, event queue |
| Mouse | Delta tracking via COPS, hardware cursor |

## Code Conventions

- **Swift**: Swift 6, `@Observable` (not ObservableObject), `@State` (not @StateObject), modern SwiftUI APIs (`.foregroundStyle`, `fileImporter`, etc.)
- **C**: C17, `-Wall -Wextra`, no external dependencies beyond SDL2 (standalone) or AppKit (Xcode)
- **Target**: Apple Silicon (arm64-apple-darwin), macOS 15+
- **Emulator/ files are SYMLINKS**: `lisaOS/lisaOS/Emulator/` contains symlinks to `src/`. No copying needed — edit `src/` and Xcode picks it up automatically.

## Git Conventions

- No Claude attribution in commit messages
- Lisa_Source/ is gitignored (Apple license prohibits redistribution)
- .claude/ directory is gitignored
