# LisaEmu — Next Session Plan (April 5, 2026)

## Quick Start

```bash
make                        # Build standalone emulator
build/lisaemu Lisa_Source   # Run from source directory (builds + boots)
make audit                  # Full toolchain report
```

## Current State: OS boots, screen draws, scheduler runs 250 frames

The Lisa OS boots through INITSYS, writes to the full screen buffer
(32760/32760 bytes non-zero), and runs its scheduler for ~250 display
frames before cascade failure from stack corruption.

### Runtime output:
```
MMU: pre-programmed sysglobmmu(102), realmemmmu(85-100), kernelmmu(17-20)
SGLOBAL@$200=$0006B000 (correctly initialized)
Screen: 32760 non-zero bytes of 32760 (full screen written!)
DIAG frame 120: PC=$019044 SR=$2100 stopped=0 pending_irq=0
Exception counts: v4=1 v11=12527 (cascade from stack corruption)
Linker: 1949 resolved, 6677 unresolved (2742 unique)
```

### How the boot works:
1. Boot ROM: sets SP/A5/A6/SR, programs 16 MMU identity segments, exits setup
2. JMP $400 → STARTUP → PASCALINIT → INITSYS
3. INITSYS: INTSOFF → GETLDMAP → REG_TO_MAPPED → POOL_INIT → INIT_TRAPV
4. → DB_INIT → AVAIL_INIT → INIT_PROCESS → INIT_EM → INIT_SCTAB
5. → BOOT_IO_INIT (identifies Lisa 2/10 Pepsi, ProFile boot)
6. → INTSON(0) → FS_INIT → scheduler loop
7. Force-unmask interrupts → vretrace drives scheduler → active OS code
8. OS writes to screen → runs 250 frames → stack corruption → cascade

## Session 3 Fixes (22 commits)

### Linker (4 fixes):
- Non-kernel symbol collision (root cause of original A6 corruption)
- Symbol pre-resolution bug in add_global_symbol
- Stub moved to $3F0 (vector table, safe from OS overwrite)
- TRAP #6 vector installed for MMU programming

### Codegen (2 fixes):
- VAR param nested scope in 3 code paths
- Cross-library {$I} include search across all LIBS/ subdirs

### MMU (5 fixes):
- 5 contexts with segment1/segment2 control
- Context latches at $FCE008-$FCE00E
- SOR/SLR register writes at $8000+seg*$20000
- Boot ROM identity mapping (16 RAM + I/O + ROM segments)
- Pre-programmed OS segments (102, 103, 104, 105, 85-100, 17-20)

### Boot environment (6 fixes):
- SGLOBAL at $200 = b_sysglobal address
- DRIVRJT at $210 = driver jump table (32 RTS entries)
- I/O board type ($FCC031 = $80 = Pepsi/Lisa 2)
- Internal disk type ($FCC015 = 1 = Sony/ProFile)
- 15+ kernel modules added (MM1-4, FSINIT, etc.)
- Source patch script (scripts/patch_source.sh)

## Cascade Failure Root Cause

At frame ~260, a FOR loop in compiled OS code writes to array elements
via `MOVE.W D0,(A0)` where A0 is computed from uninitialized data.
The write overflows into the stack frame, corrupting the saved A6.
Subsequent UNLK restores garbage A6, RTS returns to garbage address.

The uninitialized data comes from stubbed functions returning 0.
**Fix: resolve more symbols** to prevent uninitialized data structures.

## Immediate Task: Increase Resolved Symbols

### Most impactful targets:
1. **POOL_INIT / GETSPACE** — memory allocation. If these don't work,
   ALL data structures are garbage. Check if they resolve correctly.
2. **INIT_TRAPV** — installs real trap vectors. Without it, exception
   handlers may be wrong.
3. **Functions in the boot critical path** (lines 2146-2184 of STARTUP):
   INTSOFF, GETLDMAP, REG_TO_MAPPED, INIT_PE, POOL_INIT, INIT_TRAPV,
   DB_INIT, AVAIL_INIT, INIT_PROCESS, INIT_EM, INIT_EC, INIT_SCTAB,
   BOOT_IO_INIT, SYS_PROC_INIT

### Strategy:
- Check which of these functions resolve to real code vs stub
- For those going to stub, find what module defines them
- Add missing modules to kernel list or fix compilation

## Key Files

- `src/lisa_mmu.h/c` — MMU: 5 contexts, register writes, translation
- `src/toolchain/bootrom.c` — Identity mapping, I/O board detection
- `src/toolchain/linker.c` — Non-kernel isolation, stub at $3F0
- `src/toolchain/pascal_lexer.c` — Cross-library {$I} include search
- `src/toolchain/pascal_codegen.c` — VAR param nested scope
- `src/toolchain/toolchain_bridge.c` — Kernel module list
- `src/lisa.c` — SGLOBAL, DRIVRJT, MMU segments, I/O regs, interrupt hack
- `scripts/patch_source.sh` — Source preprocessing (10 patches)

## Reference Projects (in _inspiration/)

See memory files for detailed notes:
- LisaEm (Ray Arachelian, GPL v3) — hardware emulation reference
- LisaSourceCompilation (AlexTheCat123) — 56 patches, build system, boot chain

## Toolchain Metrics
```
Parser:    405 Pascal, 2 parse errors (MATHLIB, LCUT — non-kernel)
Assembler: 98 files, 100% success
Codegen:   Proc sigs, CONST, nested scope, VAR params, cross-lib includes
Linker:    Non-kernel isolation, 1949 resolved, 2742 unique stub symbols
Output:    ~433KB system.os (846 blocks)
Boot:      Reaches scheduler, screen draws, runs 250 frames actively
```
