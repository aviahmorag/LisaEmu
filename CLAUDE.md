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

## Current Status (2026-04-13)

### Prebuilt image (`prebuilt/los_compilation_base.image`)

Boots through MMU init (280+ TRAP #6), COPS handshake, ProFile driver,
Lisabug DB_INIT bypass (IPL-gated `$234` HLE), `$4FBC` NOP-skip.
After `G` at Lisabug prompt, reaches OS LOADER → **SYSTEM ERROR 10100**
(`Setup_IUInfo` can't open IU directory file). **Disk I/O / HLE issue.**

### Source compile (`Lisa_Source/` → disk image)

Toolchain: 317 Pascal + 103 ASM files → linked binary → disk image.
Boot reaches PASCALINIT → INITSYS → GETLDMAP → REG_TO_MAPPED →
POOL_INIT → MM_INIT. Runs 2000+ frames cleanly, no SYSTEM_ERROR,
then hangs in a tight loop at **$0A17BC-$0A17F6 with IPL=7**.

**Corrected diagnosis (2026-04-15):** the hang is NOT the scheduler
and NOT a MODEMA hardware poll (despite the PC landing inside the
MODEMA segment, which just contains Pascal runtime math helpers
that got linked there). One-shot trace (src/m68k.c, fires once
when PC enters $A17BC-$A17F6) confirmed the call chain matches
STARTUP.TEXT line 392-396 (`AVAIL_INIT`):

```
STARTUP $0008C0 → PMMAKE $0A6F3A (×2 calls)
STARTUP $000934↔$000928 (×30) — small loop
STARTUP $000950↔$00095E (×30) — 32-bit DIVIDE-by-subtraction:
                                 adjust_index := membase div maxmmusize
STARTUP $000962 → MODEMA-seg $0A18E6 — JSR to multiply helper
                                       (i-realmemmmu)*maxmmusize
MODEMA-seg $0A1996 → $0A17A0 — nested JSR (stuck here)
```

The `$A17A0` loop walks memory at A0=$B9F608 in 8-byte strides,
reading word and comparing against D2. iospacemmu=126 → $FC0000
(not $B9F608), so it's NOT an I/O poll. $B9F608 = seg 92 offset
$1F608 — looks like garbage/uninitialized, not a real data
structure. Suspect: the SETMMU inline code inside the multiply
helper is walking a corrupt MMU descriptor list.

Also suspicious: `sg_free_pool_addr = $A0000000` (non-canonical).

Inspiration projects: `_inspiration/lisaem-master/src/lisa/` has
full Z8530 SCC, VIA, COPS, floppy emulation — worth mining if we
later need SCC. Not needed for this bug.

Next: (a) add a trace at the START of AVAIL_INIT to confirm the
procedure identity; (b) log the actual parameters passed into
$A17A0 (the stuck sub) — specifically the pointer at -4(A6) of
$A18E6's frame, which becomes A0=$B9F608; (c) find where that
pointer originates, likely a miscompiled MMU-descriptor-base
computation.

**Key progress (2026-04-13):**
- P6: Boolean size (1→2 bytes) — Lisa Pascal stores booleans as words.
  Fixed PARMS frame misalignment (swappedin[1..48] was half-sized).
- P6: expr_size const ordering — check is_const BEFORE type.
  Fixes large constants like maxmmusize=131072 treated as 16-bit.
- P7: Interface declarations no longer export linker symbols.
  Fixed GETFREE/BLDPGLEN/MAKE_REGION all resolving to module base
  (offset 0) instead of their actual code offsets.
- P8: Procedure-local const declarations now processed in gen_proc_or_func.
  Fixes nospace=610 (imported) overriding local nospace=10701.
- P8: No EXT.L on function call results in binary ops.
  Function calls return full 32-bit values; EXT.L zeroed MMU_BASE's
  $CC0000 return value, corrupting POOL_INIT's mb_sgheap parameter.
- P9: Boolean NOT — bitwise NOT.W (1→$FFFE) replaced with logical
  TST/SEQ/ANDI (1→0, 0→1). ALL `if not func(...)` patterns were
  always-true due to this bug.
- P9: l_sgheap $8000→$7E00 (page-aligned, fits int2).
- P10: Function result variables — create local for return value,
  load into D0 before RTS.
- P11: Nested binary ops save D2 on stack — when the right operand
  is complex (binary op, func call, unary), use MOVE.L D0,-(SP)
  instead of D2 to prevent clobbering.
- P12: true/false/nil inside WITH blocks — the WITH fallthrough
  handler emitted MOVE.W #0 instead of checking for built-in
  constants. Every `x := true` inside a WITH block stored false.
- Memory layout: himem = b_dbscreen, bothimem = lomem.

**Key progress (2026-04-12):**
- P4: Fixed compile order, field offsets, store-width, global offset reuse
- P5: Fixed stale-upper-word bugs throughout codegen:
  - SIZEOF/integer literals now use MOVEQ/MOVE.L (never MOVE.W)
  - ORD4 sign-extends 16-bit args to 32-bit
  - Binary ops sign-extend operands when use_long=true
  - Store-width guards on WITH-field, var-param, complex-LHS paths
  - Result: GETSPACE works, MM_INIT completes, boot progresses

### Toolchain metrics
- Parser: **100%** (317/317), Assembler: **100%** (103/103)
- Linker: 8527 symbols, output ~2.2 MB
- Codegen: P1-P5 ptr/store, P6 bool+const, P7 iface, P8 local-const, P9 bool-NOT, P10 func-result, P11 D2-stack, P12 WITH-true

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
