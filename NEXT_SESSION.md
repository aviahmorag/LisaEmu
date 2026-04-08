# LisaEmu — Next Session Plan (April 8, 2026)

## Quick Start

```bash
make

# Cross-compiled (from source):
build/lisaemu Lisa_Source

# Pre-built (from Alex's LOS compilation):
build/lisaemu --image prebuilt/los_compilation_base.image
```

## Where We Are

### Cross-compiled path (the ultimate goal)
POOL_INIT receives correct parameters. Blocked on INIT_FREEPOOL codegen
(parameter loaded as constant instead of from stack frame). Error 10701.

### Pre-built image path (diagnostic tool for emulator bugs)
Pascal loader (LOADER.TEXT) is executing from boot track. Reaches BOOTINIT.
Blocked on TRAP #6 (DO_AN_MMU) — handler not loaded (SYSTEM.LLD not yet read).

**Both paths share the same emulator bugs.** Fixing the emulator for the
pre-built image directly helps the cross-compiled path.

## What Was Fixed (this session: 5 fixes, 4 commits)

### 1. MMU stack segment SOR (lisa.c)
Lisa MMU hw_adjust=$100 for stack segments. SOR = base/512 + len/512 - $100.
Without this, ALL mapped stack reads after REG_TO_MAPPED return garbage.

### 2. 32-bit multiplication (pascal_codegen.c)
Inline 32×32→32 using MULU.W partial products. Fixes MMU_BASE(102)=$CC0000.

### 3. 32-bit division (pascal_codegen.c)
Inline shift-and-subtract loop (32 iterations via DBRA). Fixes Zero Divide.

### 4. ProFile block read — no deinterleave needed (lisa.c)
LDPROF's interleave routine converts logical→physical before calling prof_entry.
The disk image stores blocks in physical order, so no reversal needed.

### 5. Pre-built image boot path (main_sdl.c)
`--image` flag bypasses cross-compilation. Boot ROM + ProFile HLE reads blocks.
The real Lisa boot loader loads, programs MMU, and enters Pascal execution.

## Current Blockers

### Blocker A: TRAP #6 handler (both paths)
TRAP #6 (DO_AN_MMU, vector $98) dispatches to $A84000 — uninitialized memory.
The Pascal loader calls TRAP #6 to program MMU segments before SYSTEM.LLD is loaded.
This is likely from BOOTINIT (memory initialization). The handler needs to be
either stubbed or the loader needs to program MMU directly via setup mode writes.

**Next step:** Check BOOTINIT source (source-LDUTIL.TEXT) to see what triggers
TRAP #6. Likely needs a minimal DO_AN_MMU implementation in the ROM or
intercept TRAP #6 in the HLE.

### Blocker B: INIT_FREEPOOL codegen (cross-compiled only)
Parameter `fp_ptr` loaded as constant ($C4B48) instead of from A6+offset.
Likely the `(* inherited params *)` syntax doesn't register params properly.
Also: LINK allocates only 4 bytes for 2 pointer locals (should be 8).

## Verified Working
```
✅ MMU stack segment SOR (hw_adjust for stack type $600)
✅ MMU write test: $CC0000 → physical (PASS)
✅ 32-bit multiply: MMU_BASE(102) = $CC0000
✅ 32-bit divide: no Zero Divide for $20000 divisor
✅ POOL_INIT params correct (cross-compiled path)
✅ ProFile block reads via HLE prof_entry
✅ Boot track loads correctly (pre-built image)
✅ Pascal loader enters execution (pre-built image)
✅ BOOTINIT runs, reaches MMU programming phase
```

## Boot Sequence (pre-built image)
```
✅ ROM → setup MMU seg 0-15 → exit setup mode
✅ ROM → JMP $20000 (boot track)
✅ LDPROF → copy to $100000, read blocks 1-30 via prof_entry
✅ LDPROF → start_pascal → push params → JMP to Pascal entry
✅ LOADER.TEXT → BOOTINIT starts executing
💥 TRAP #6 (DO_AN_MMU) → handler at $A84000 → Line-F garbage
❌ LOAD_LLD → read SYSTEM.LLD from disk
❌ LOAD_OS → read SYSTEM.OS, map segments
❌ ENTEROP → jump to OS entry point
```

## Boot Sequence (cross-compiled)
```
✅ ROM → SSP=$F0800 → REG_TO_MAPPED → mapped stack
✅ POOL_INIT with correct params
💥 INIT_FREEPOOL → codegen bug (param as constant)
❌ GETSPACE → SYSTEM_ERROR(10701)
❌ BOOT_IO_INIT → ProFile handshake
❌ FS_INIT → mount boot volume
❌ ENTER_SCHEDULER → Desktop
```

## Key Files
```
src/lisa.c                      — MMU pre-programming, ProFile HLE, boot paths
src/lisa_mmu.c                  — MMU translation, stack SOR fix
src/m68k.c                      — CPU, traces, exception handling
src/main_sdl.c                  — --image flag for pre-built boot
src/toolchain/pascal_codegen.c  — 32-bit multiply/divide
src/toolchain/bootrom.c         — Boot ROM (MMU setup, prof_entry stub)
prebuilt/                       — Pre-built disk image + extracted SYSTEM.OS
```
