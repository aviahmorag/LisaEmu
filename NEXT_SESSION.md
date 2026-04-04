# LisaEmu — Next Session Plan

## Current State (April 5, 2026)
- **Grey screen with artifacts** — OS writes to video RAM
- 276/276 Pascal, 96/96 assembly compile, 5,627 symbols linked
- A5 initialized by PASCALINIT, ENTER_SCHEDULER resolved
- 8,811 broken JSR $000000 patched (linker relocation bug)
- Stable CPU until SANE FP loop overflows stack

## Priority 1: Fix Line-F SANE FP Handler

**Problem**: OS hits Line-F at $07773A (SANE FP code) in infinite loop → stack overflow.  
**Why**: Handler skips the Line-F opcode but doesn't compute a result. Caller retries.

**Fix**:
1. Read `Lisa_Source/LISA_OS/LIBS/LIBFP/libfp-saneasm.text.unix.txt` — understand SANE dispatch format
2. SANE opcodes: `$Fxxx` where bits 11-0 = operation. Some ops have extra operand words.
3. Option A: Implement a minimal SANE handler that reads the opcode, performs the operation using host FP
4. Option B: At minimum, read the SANE opcode and skip the correct number of words (not just 2 bytes)
5. The handler is in `src/toolchain/bootrom.c` at $FE0310

## Priority 2: Fix 8,811 Unresolved Relocations Root Cause

**Problem**: 8,811 JSR targets were never patched by the linker. Binary scan catches them but the root cause needs fixing.

**Fix**:
1. Increase linker per-module reloc limit from 4096 → 16384 (line 244 in `src/toolchain/linker.c`)
2. Check `CODEGEN_MAX_SYMBOLS` (4096) — may also overflow for large files  
3. Verify the re-apply phase in linker correctly patches all relocations before module copy

## Priority 3: Resolve Remaining Symbols

**HAllocate/FreeH/HzInit** (heap functions):
- Defined in `Lisa_Source/LISA_OS/LIBS/LIBSM/LIBSM-UNITHZ.TEXT.unix.txt`
- Regular unit (not INTRINSIC), but HAllocate NOT FOUND in symbol table
- Check if the parser fails partway through this 2800-line file, losing HAllocate at line 2190

**LogCall** (debug trace):
- Never defined in source — called behind `{$IFC fTraceXX}` conditional flags
- Our compiler evaluates conditionals as true, so the calls exist but no definition
- Fix: add LogCall as a built-in no-op in the toolchain

**SetRectRgn/TextWidth** (QuickDraw):
- Should be in LIBQD assembly. Check if GRAFASM exports them.

## Key Architecture Reference

### Boot sequence
```
ROM $FE0400 → JMP $400 → BRA.W STARTUP_main → INITSYS(PASCALINIT) → 
GETLDMAP → INIT_PE → DB_INIT → AVAIL_INIT → INIT_PROCESS → INIT_EM → 
INIT_SCTAB → BOOT_IO_INIT → SYS_PROC_INIT → PR_CLEANUP → ENTER_SCHEDULER
```

### Key files
```
src/toolchain/pascal_codegen.c  — Pascal code generator
src/toolchain/linker.c          — Linker
src/toolchain/asm68k.c          — 68000 assembler  
src/toolchain/bootrom.c         — Boot ROM (Line-F handler at $FE0310)
src/m68k.c                      — 68000 CPU emulator
src/lisa.c                      — Lisa hardware emulation
src/lisa_mmu.c                  — Memory controller
```

### Loader parameters (in lisa.c lisa_reset)
```
$218: adrparamptr → $800
$800+28: esysglobal → $14000
A5 initialized to $14000 (PASCALINIT sets it to $F891FE)
```
