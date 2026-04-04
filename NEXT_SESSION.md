# LisaEmu — Next Session Plan

## Current State (April 4, 2026)
- **Grey screen with artifacts** — OS writes to video RAM, gets further into boot
- 276/276 Pascal, 96/96 assembly compile, 5,627 symbols linked
- A5 initialized by PASCALINIT, ENTER_SCHEDULER resolved
- 8,811 broken JSR $000000 patched (linker relocation bug)
- **FIXED**: ADDQ stack cleanup bug (odd A7 → stack corruption)
- **FIXED**: Forward branch displacement off-by-2 in IF/WHILE/FOR
- OS now executes ~272M cycles before hitting data-as-code issue
- Line-F at $07773A is gone; new crash at $078FFE is CPU executing data table

## Bugs Fixed This Session

### 1. ADDQ Stack Cleanup (pascal_codegen.c:555)
**Was**: `emit16(cg, 0x508F | ((arg_bytes / 2) << 9));`
**Fix**: `emit16(cg, 0x508F | ((arg_bytes & 7) << 9));`
The ADDQ.L #n,A7 encoding puts the byte count (not word count) in bits 11-9.
Was generating ADDQ.L #1,A7 instead of ADDQ.L #2,A7 for 1-arg calls, making A7 odd.

### 2. Forward Branch Displacement (pascal_codegen.c: IF/WHILE/FOR patches)
**Was**: `patch16(cg, pos, (uint16_t)(cg->code_size - pos - 2));`
**Fix**: `patch16(cg, pos, (uint16_t)(cg->code_size - pos));`
BRA.W/Bcc.W displacement is relative to the displacement word's address.
The `pos` variable already captures that address, so no `-2` adjustment needed.
Backward branches (inline computed) were correct; only forward patches were wrong.

## Priority 1: CPU Executing Data Table at $078FFE

**Problem**: CPU walks through data at $078F86+ (small values $0000, $0007, $000B being
executed as ORI instructions), eventually hitting $FFE6 (Line-F).

**Likely causes** (investigate in order):
1. CASE statement codegen is a stub — only executes first case body, falls through into
   subsequent case data. Many OS routines use CASE heavily.
2. Unresolved function stubs return 0 in D0; callers use this as a pointer/jump target,
   landing in garbage memory.
3. Some other codegen issue causing execution to land on data.

**Fix approach**:
1. Implement proper CASE statement codegen with if-else chains (simpler than jump tables)
2. Check if the data at $078F86 matches a known CASE dispatch table or data section

## Priority 2: Resolve Remaining Unresolved Symbols

Same as before — still 8,811 JSR $000000 patched to stub.

**HAllocate/FreeH/HzInit** (heap functions):
- Defined in `Lisa_Source/LISA_OS/LIBS/LIBSM/LIBSM-UNITHZ.TEXT.unix.txt`
- NOT FOUND in symbol table — parser likely fails partway through this file

**LogCall** (debug trace):
- Never defined in source — called behind `{$IFC fTraceXX}` conditional flags
- Fix: add LogCall as a built-in no-op in the toolchain

**SetRectRgn/TextWidth** (QuickDraw):
- Should be in LIBQD assembly. Check if GRAFASM exports them.

## Priority 3: Other codegen improvements
- CASE statement: implement proper if-else chain dispatch
- Verify SANE FP calls (Lisa uses JSR FP68K, NOT Line-F traps — FPBYTRAP=0)

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

### Important discovery: Lisa SANE uses JSR, not Line-F traps
`FPBYTRAP .EQU 0` in sanemacs.text means JSRFP expands to `JSR FP68K`.
Line-F exceptions during execution are NOT SANE calls — they indicate
the CPU is executing data or hitting corrupted code.
