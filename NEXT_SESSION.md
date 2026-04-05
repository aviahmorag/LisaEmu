# LisaEmu — Next Session Plan (April 5, 2026)

## Quick Start

```bash
make                        # Build standalone emulator
build/lisaemu Lisa_Source   # Run from source directory (builds + boots)
make audit                  # Full toolchain report
```

## Current State: MMU framework working, blocked on unresolved symbols

Boot path works through ROM → STARTUP → PASCALINIT → deep into INITSYS.
A6 frame pointer stays intact. MMU identity mapping confirmed via register
writes. OS execution eventually follows an uninitialized pointer to empty RAM.

### Runtime output:
```
MMU REG: ctx=1 seg=0..15 identity mapped (boot ROM)
CODE ESCAPE: PC=$180018 from $0627CC (MOVEA.L abs,A0; JMP d16(A0))
  → pointer was never initialized (function that sets it up was stubbed)
Exception: Line-F at $200000 (past 2MB RAM boundary → $FFFF = Line-F opcode)
Linker: 1956 resolved, 6678 unresolved (2746 unique stub symbols)
```

## Session 3 Fixes (11 commits)

### Linker fixes:
1. **Non-kernel symbol collision** — Non-kernel module symbols with base_addr=0
   resolved to addresses inside kernel code. JSR to MM_INIT landed inside
   GETLDMAP's body, past its LINK A6 → corrupt frame pointer.
   FIX: Only resolve symbols from is_kernel modules in Phase 2.
2. **Symbol pre-resolution** — add_global_symbol() marked LSYM_ENTRY as resolved
   immediately. Last-one-wins for duplicate symbols (non-kernel) kept resolved=true.
   FIX: Never pre-resolve; Phase 2 handles resolution.
3. **Stub location** — Stub at end of code got overwritten by OS init.
   FIX: Moved to $3F0 (unused vector table area).
4. **TRAP #6 vector** — Added do_an_mmu to vector table at $98.

### Codegen fixes:
5. **VAR param nested scope** — Three paths accessed outer-scope VAR params via
   A6 without walking the frame chain. Fixed gen_lvalue_addr, gen_expression,
   gen_statement(AST_ASSIGN).

### Kernel module expansion:
6. **15+ OS modules added** — MM1-MM4, MMPRIM2, DS2-DS3, DEVCONTROL, FSINIT1-2,
   LOAD1, LOADER, EXCEPRES, EXCEPNR1, PMCNTRL, PMSPROCS, GDATALIST, etc.

### MMU implementation:
7. **5 contexts** (0=start, 1-4=normal) with segment1/segment2 selection
8. **Context latches** at $FCE008-$FCE00E
9. **MMU register writes** detected at $8000+seg*$20000 during setup mode
10. **Boot ROM identity mapping** — 16 segments + I/O + ROM before setup exit
11. **Translation** enabled on setup mode exit, uses SOR-based page origin

## Root Cause of Current Failure

The OS at $0627CC loads a pointer via `MOVEA.L (abs),A0; JMP offset(A0)`.
The pointer ($180018) was never initialized because the function responsible
for setting it up resolves to the stub ($3F0) and returns 0.

**This is a resolved-symbol-count problem.** Only 1956 of 8634 symbols resolve
to real code. The rest go to a stub that returns 0. Critical data structures
like jump tables, vtables, and function pointers are never initialized.

## Immediate Task: Increase Resolved Symbols

### Strategy 1: Source preprocessing script
Apply the 56 patches from LisaSourceCompilation project's `scripts/patch_files.py`
automatically before compilation. Key patches:
- DRIVERDEFS.TEXT: DEBUG1=FALSE, TWIGGYBUILD=FALSE
- PASCALDEFS.TEXT: DEBUG1=0, TWIGGYBUILD=0
- LIBOS/SYSCALL.TEXT: replace with SOURCE/SYSCALL.TEXT
- Various typo and path fixes

### Strategy 2: Identify critical boot-path functions
The exact escape is at $0627CC. Find which function this is (by matching
the linked offset to a module) and trace what initialized the pointer.

### Strategy 3: More kernel modules
Check which OS modules export symbols used by the boot path. The linker
logs show 2746 unique unresolved symbols — filter to those called from
kernel modules only.

## Lisa MMU Architecture (from source + LisaEm analysis)

### Segment Layout (from MMPRIM.TEXT):
```
kernelmmu  = 17    → OS code at logical $220000 (seg 17 * $20000)
realmemmmu = 85    → physical RAM at logical $AA0000
sysglobmmu = 102   → system global at logical $CC0000
```

### Hardware Registers:
```
SLIM base = $8000 + seg * $20000   (Segment Limit Register)
SORG base = $8008 + seg * $20000   (Segment Origin Register)
SLR values: $0700=RW mem, $0900=I/O, $0F00=SIO/ROM
```

### Context Control (I/O latches):
```
$FCE008: segment1 = 0     $FCE00A: segment1 = 1
$FCE00C: segment2 = 0     $FCE00E: segment2 = 2
$FCE010: setup mode ON    $FCE012: setup mode OFF
Context = start ? 0 : (1 + segment1 | segment2)
```

### DO_AN_MMU (TRAP #6 handler):
- Reads SMT entries: origin(16) | access(8) | length(8)
- Toggles setup mode per segment write
- Writes SLIM then SORG for each segment
- Uses CXASEL (ignores start flag) for target context

## Key Files

- `src/lisa_mmu.h/c` — MMU with 5 contexts, register writes, translation
- `src/toolchain/bootrom.c` — Identity mapping, setup mode exit
- `src/toolchain/linker.c` — Non-kernel symbol isolation, stub at $3F0
- `src/toolchain/pascal_codegen.c` — VAR param nested scope fix
- `src/toolchain/toolchain_bridge.c` — Kernel module list
- `src/m68k.c` — Code escape trace, stub call trace

## Reference Projects (in _inspiration/)

### LisaEm (Ray Arachelian, GPL v3)
Full Lisa hardware emulator. Key files for us:
- `src/lisa/cpu_board/mmu.c` — MMU translation tables, context switching
- `src/lisa/cpu_board/memory.c` — Memory dispatch, SIO handlers
- `src/lisa/io_board/via6522.c` — VIA emulation
- `src/lisa/io_board/cops.c` — Keyboard/mouse
- `src/storage/profile.c` — ProFile hard disk

### LisaSourceCompilation (AlexTheCat123)
How to compile Lisa OS from source. Key files:
- `scripts/patch_files.py` — 56 source patches (typos, paths, flags)
- `src/LIBPL/*.TEXT` — Recreated missing assembly (PASMEM, PWRII)
- `src/LIBFP/STR2DEC.TEXT` — Recreated FP string conversion
- `src/MAKE/*.TEXT` — Build order scripts
- `glue.c` — LisaEm OS detection patch

## Toolchain Metrics
```
Parser:    405 Pascal, 360 OK (99.5%)
Assembler: 105 files, 100% success
Codegen:   Proc sigs, CONST, nested scope, VAR params, array bounds
Linker:    Non-kernel isolation, 1956 resolved, 2746 stub symbols
Output:    ~434KB system.os (848 blocks), boots with identity MMU
```
