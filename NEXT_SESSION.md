# LisaEmu — Next Session Plan (April 6, 2026)

## Quick Start

```bash
make                        # Build standalone emulator
build/lisaemu Lisa_Source   # Run from source directory (builds + boots)
```

## Current State: PASCALINIT local label relocation fix needed

The OS cross-compiles with zero parse errors, zero SYSTEM_ERRORs,
and the correct 46-module kernel from the reference project. But
PASCALINIT never returns because `LEA trap7hand,A1` assembles with
a raw section offset ($44) instead of the linked address.

### The blocking bug:
Assembly local labels (like `trap7hand` in starasm1.text) use absolute
addressing with section-relative offsets. Without linker relocations,
`LEA trap7hand,A1` puts $44 in A1 instead of the real address. The
TRAP #7 handler is installed at vector $9C pointing to $44 (vector
table data), not the handler code. When %initstdio calls TRAP #7,
it jumps into garbage.

### Fix in progress (commit 3971cb3):
- Assembler generates relocations for ALL local label references
- Bridge code adds local symbols to the linker's symbol table
- Linker resolves them to base_addr + label_offset

### Issue with current fix:
DRIVERS.TEXT generates 173 exported symbols (was 1) due to every
local label getting a relocation. This makes linking extremely slow.
Need to optimize: only generate relocations for labels used in
absolute addressing mode (LEA/JSR/MOVE.L abs, not BSR/BRA/d16(PC)).

## Session 3 Summary (46 commits)

### Critical bugs fixed:
1. **Multi-parameter parsing** — `(a, b, c: type)` collapsed to 1 param.
   Affected EVERY function. POOL_INIT got 1 of 6 params. FIXED.
2. **FOR loop MOVE.L→MOVE.W** — 4-byte write into 2-byte slot. FIXED.
3. **Linker non-kernel symbol collision** — base_addr=0 collision. FIXED.
4. **LIBHW directory skip** — 74 COPS symbols missing. FIXED.
5. **Local label absolute addressing** — LEA/JSR to local labels wrong. IN PROGRESS.

### Architecture (from LisaSourceCompilation reference):
- Kernel = 46 OS modules (ALEX-LINK-SYSTEMOS.TEXT) + LIBPL/LIBHW/LIBFP/LIBOS
- Binary ~918KB, fits in 2MB RAM with screen at $1F8000
- MMU: 5 contexts, identity + OS segments pre-programmed
- ProFile: protocol-accurate module written (src/profile.c/h)

### Key metrics:
- 3106 kernel-resolved symbols (kernel complete)
- Zero SYSTEM_ERROR calls
- Zero parse errors (420 source files)
- PASCALINIT called but doesn't return (local label bug)

## What needs to happen (in order):

### 1. Fix local label relocations (IMMEDIATE)
Optimize the assembler to only generate relocations for labels used
in absolute addressing mode. Current approach adds ALL local labels
as symbols, making linking too slow.

Alternative: use PC-relative addressing for local labels in LEA/PEA
instructions (on pass 1, reserve 4 bytes for abs.L; on pass 2,
emit d16(PC) + 2 bytes padding if within range).

### 2. PASCALINIT returns → INITSYS runs
Once trap7hand is correctly addressed, %initstdio and mapiospace
will work. PASCALINIT returns, INITSYS is called, and the full
init chain runs: INTSOFF → GETLDMAP → REG_TO_MAPPED → POOL_INIT →
INIT_TRAPV → ... → BOOT_IO_INIT → FS_INIT → ENTER_SCHEDULER.

### 3. ProFile VIA handshake
Protocol-accurate module ready (src/profile.c). Needs testing once
BOOT_IO_INIT reaches the ProFile driver initialization code.
Key: BSY via CA1, CMD via ORB bit 4, data on PORTA.

### 4. MDDF filesystem format
Our diskimage.c sets fsversion=$1000. Must be 14, 15, or 17.
Full MDDF structure documented in memory file.

### 5. Intrinsic library loading
OS expects to load SYS1LIB, IOSFPLIB etc. from disk. Need either
virtual disk files or HLE stubs.

## Key Files

```
src/toolchain/asm68k.c          — Local label relocation fix (WIP)
src/toolchain/toolchain_bridge.c — Kernel selection, local symbol export
src/toolchain/pascal_parser.c    — Multi-param fix, semicolon tolerance
src/toolchain/pascal_codegen.c   — FOR loop MOVE.W, VAR nested scope
src/toolchain/pascal_lexer.c     — Cross-library {$I} search
src/toolchain/linker.c           — Non-kernel isolation, stub at $3F0
src/toolchain/bootrom.c          — MMU identity mapping, screen clear
src/toolchain/diskimage.c        — MDDF needs fsversion=17
src/profile.c/h                  — Protocol-accurate ProFile emulation
src/lisa_mmu.h/c                 — 5 contexts, register writes, translation
src/lisa.c                       — Boot env, SGLOBAL, DRIVRJT, I/O regs
src/m68k.c                       — TRAP trace, SYSTEM_ERROR trace
scripts/patch_source.sh          — Source preprocessing (10 patches)
```

## Reference Projects (in _inspiration/)
- LisaEm: ProFile protocol, Z8530 serial, MMU, COPS
- LisaSourceCompilation: 46-module kernel list, 56 patches, MDDF format
- See memory files for detailed notes
