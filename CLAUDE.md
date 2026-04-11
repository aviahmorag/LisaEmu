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

## Current Status

**Prebuilt-image boot — WORKING (interactive Lisabug shell).** Running
the Xcode macOS app with `prebuilt/los_compilation_base.image` boots
the real Lisa OS far enough to drop into the Lisabug debugger prompt,
paints to the framebuffer, and accepts live keyboard input via native
`NSView.keyDown`. The CPU decoder bug that caused earlier Level 7
crashes on `BSET Dn,<ea>` has been fixed in `src/m68k.c:~2507` (the
group-0 dynamic-bit dispatch now accepts bits 7:6 == 11 for BSET).
Keyboard mapping uses authoritative Lisa keycodes from
`Lisa_Source/LISA_OS/LIBS/LIBHW/LIBHW-LEGENDS.TEXT` (Final US layout).
Key auto-repeat is filtered via `event.isARepeat` in
`LisaDisplayView.swift` — Lisa OS runs its own repeat timer from
`libhw-KEYBD` `RepeatTable`, so forwarding macOS repeats would
compound them.

**Toolchain (source → image pipeline)** — green but does NOT yet boot
end-to-end:

- Parser: **100%** (317/317 Pascal files)
- Assembler: **100%** (103/103)
- Linker: **Link OK: YES**, 8527/8527 symbols resolved, 2.2 MB output,
  97.2% of JSR abs.L targets point at real code
- `build/lisaemu Lisa_Source` runs the full pipeline: compiles from
  `Lisa_Source/`, writes `build/lisa_profile.image` (5.1 MB, 58 files,
  9728 blocks) + `build/lisa_boot.rom`, plus a raw
  `build/lisa_linked.bin` (870 KB) for offline disassembly, then starts
  executing the compiled 68000 code. Early-boot TRAPs 37/39 take the
  real handlers; TRAP #6 (MMU accessor) currently hits an RTE stub at
  `$3F8`. CPU then spins in `libfp-FPMODES` around `PC=$097A**` with
  a pattern that strongly suggests Pascal codegen is loading a VAR
  pointer as 16-bit (`MOVE.W 8(A6),D0 / MOVEA.L D0,A0 / MOVE.W
  (A0),D0`) and truncating it. Separate track from the prebuilt-boot
  work — many codegen bugs likely remain.

**Emulator core** — verified end-to-end against the prebuilt fixture:
CPU, MMU, VIA1/VIA2, COPS, keyboard, video, interrupts, exception
dispatch, TRAP #5 HW-interface dispatcher, Lisabug debugger shell all
work. First macOS-native interactive boot with live keyboard as of
this session.

See `NEXT_SESSION.md` (gitignored) for the live handoff and
`.claude-handoffs/` for per-session archives.

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
