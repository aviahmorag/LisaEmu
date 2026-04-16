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

## Current Status (2026-04-16) — 25/27 reported (21 real + 4 bypass-fired)

Build and audit green. Two milestone labels lost to code layout shifts (cosmetic).

### P79 session fixes (6 structural codegen improvements)

1. **Record layouts** (P79): string word-padding, CONST pre-pass export. Fixed devrec (70→74), DCB (344→122).
2. **Push direction** (P79c): `is_callee_clean` prefers sig's is_external over symbol's.
3. **Proc sig pre-pass** (P79d): proc sigs exported during types pre-pass.
4. **Enum constants** (P79e): AST_TYPE_ENUM registers ordinal values. Previously ALL enums resolved to 0.
5. **Byte-subrange sizing** (P79f): range<=255 → natural size=1. Record fields widen to 2. Arrays stay at 1. Fixed sc_par_no overflow into b_syslocal_ptr.
6. **Record-field array stride** (P79f): resolve element type for FIELD_ACCESS array bases.

### Bypass-fired milestones (4):

- **SYS_PROC_INIT** (P35): entry PC reached, body skipped.
- **FS_CLEANUP** (P37): body spins in FIND_REFNCB_ENTRY.
- **MEM_CLEANUP** (P36): body spins in ADDTO_MMLIST.
- **PR_CLEANUP** (P38): idle scheduler loop — needs Shell.

### Next blocker: SYS_PROC_INIT body — process creation NULL pointers

P35 disabled: 21/27 milestones. SYS_PROC_INIT doesn't complete.
b_syslocal_ptr now correctly $CE0000 (was $040000 due to SCTAB2
overflow, fixed by P79f). BLD_SEG fixed (P79c/d). ENQUEUE fixed
(P79e). Remaining VEC-GUARD writes from GETSPACE/pool code and
CreateProcess — more codegen bugs to trace.

### Roadmap to fully bootable Lisa desktop

**Current: Kernel 90% complete, full desktop ~25-30%**

| Layer | Status | What's needed |
|-------|--------|---------------|
| OS Kernel (SYSTEM.OS) | 90% — 27/27 checkpoints | Fix SYS_PROC_INIT body |
| System Libraries (SYS1LIB, SYS2LIB) | 0% | New compile targets + linking |
| Graphics (LIBQD, LIBTK) | 0% | New compile targets |
| Drivers (SYSTEM.LLD, CD_*) | 0% | 13 new binaries |
| Shell (APDM = Desktop Manager) | 0% | Separate compile target |
| Apps (LisaWrite, LisaCalc, etc.) | 0% | 14 app targets |
| Intrinsic library loading | 0% | Dynamic linking support |
| Lisa filesystem (MDDF, catalog) | 0% | Disk image infrastructure |
| Boot ROM / bootloaders | Partial | BT_Profile, BT_Sony |

Full source code is available for ALL components. The toolchain (parser 100%, assembler 100%, linker working) can handle additional targets — the remaining work is systematic: define 26+ compile targets, fix per-target codegen issues, build the disk layout.

### Key structural codegen fixes (cumulative, P4–P78)

These are durable improvements to the Pascal cross-compiler, not one-off patches. All are in git history. Major classes:

- **Field layout**: subrange default word-size in unpacked records, variant records, PACKED propagation, PASCALDEFS pin table (29 A5-relative globals)
- **Pointer arithmetic**: EXT.L skip for wide operands (`rhs_has_wide_operand()`), narrow→wide sign-extension on stores
- **Type resolution**: two-pass compile (types pre-pass), cross-unit type propagation, proc-local TYPE/CONST registration
- **Control flow**: goto/numeric-label support, case multi-labels + selector preservation
- **String ops**: byte-compare for string equality
- **WITH blocks**: nested WITH field bases, double-deref, address-of fields, true/false/nil inside WITH
- **Calling conventions**: non-primitive value params as by-ref, prefer Pascal body sig over external, proc-sig type remap by name

### Active HLE bypasses

- **P33** REG_OPEN_LIST: mounttable chain walk
- **P34** excep_setup: wild `b_sloc_ptr`
- **P35** SYS_PROC_INIT: full system-process creation
- **P36** MEM_CLEANUP: fires milestone, bypasses body
- **P37** FS_CLEANUP: fires milestone, bypasses body
- **P38** PR_CLEANUP: fires milestone, bypasses body
- **P42** Dynamic HLE lookup via `boot_progress_lookup` (cached per `g_emu_generation`)
- **P78** Signal_sem HLE guard + RELSPACE guard

### Key HLE mechanisms

- `$234` fetch bypass: IPL=7→RTE (DB_INIT skip), IPL=0→execute (Lisabug)
- `$4FBC` NOP-skip: illegal opcode in Workshop init loop
- ProFile HLE: intercepts CALLDRIVER, reads from disk image
- INTSON/INTSOFF: manages IPL for compiled OS code
- Loader TRAP HLE: MMU-translated reads/writes for fake_parms
- ENTER_LOADER HLE: mode-switch bypass (supervisor→user A7 swap issue)
- Setup_IUInfo HLE: skips INTRINSIC.LIB read loop
- GETSPACE: zero-fills allocated blocks (calloc semantics)
- Unmapped segment writes dropped (P27 generic MMU safety net)

### Debug infrastructure

- `DBGSTATIC` macro + `g_emu_generation`: all debug statics reset on power cycle
- Bounded print budgets on all diagnostic output
- `VEC-FIRST` per-vector first-fire trace, periodic DIAG frame dumps
- 256-entry PC ring for crash analysis
- `boot_progress_lookup(name)` public accessor for linker symbol table

### Inspiration projects

- `_inspiration/LisaSourceCompilation-main/`: 2025 working compilation of LOS 3.0 on real Lisa hardware. `scripts/patch_files.py` catalogs source patches.
- `_inspiration/lisaem-master/`: Reference for SCC/VIA/COPS/ProFile/floppy emulation.

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
