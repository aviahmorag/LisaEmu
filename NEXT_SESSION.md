# LisaEmu — Next Session Plan (April 7, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source   # Cross-compiled OS (the main path)
```

## Where We Are

**The OS draws on screen for the first time.** Asymmetric patterns visible —
real OS code executing through the display system. The duplicate label fix
unblocked the floating-point library and the OS now reaches driver init,
filesystem code, and display output.

**Stops at:** SYSTEM_ERROR(0) triggered by sudden stack corruption.
SP jumps from valid mapped address to $FFFFFFE6 (wraps through $0).

## What Was Fixed (this session total: 40+ commits)

### The breakthrough fix: duplicate labels across .include files
NEWFPSUB includes mathsub, fpunpack, and size — all define UNPNRM,
UNPZUN, UNP0, etc. with DIFFERENT code. Our assembler resolved
fpunpack's `BMI.S UNPNRM` to mathsub's version (wrong code),
causing the FP unpack loop to never terminate.

Fix: proximity-based label resolution. Multi-definition tracker
records all PCs for labels defined more than once. On pass 2,
get_symbol_value finds the nearest forward definition.

### Other fixes this session
- ORD() on pointers returned 2 not 4 (root cause of code overwrite)
- WITH statement implemented (206 OS instances)
- 30+ codegen type-size fixes
- Cross-unit type resolution (dangling pointers, aliases, forward params)
- smt_base and driver JT moved out of code space
- Standalone programs removed from kernel
- Negative global offsets from A5
- prof_entry PROM routine for pre-compiled image boot
- SYSTEM_ERROR HLE halts CPU instead of returning

## Current Blocker: Sudden Stack Corruption

SP jumps from valid mapped value (~$CBFFxx) to $FFFFFFE6 in ONE
instruction. Not a gradual stack leak — a single UNLK or MOVEA
sets SP to garbage, then exception frames push it down through
$000000 and wrap to $FFFFFF.

### Next step
Add trace that fires when SP changes by >$1000 in one instruction.
This catches the exact instruction that corrupts SP. Look for:
- UNLK with corrupted A6
- MOVEA.L loading garbage into A7
- RTE popping corrupt stack frame

### Pre-compiled image (side path, for validation)
- "AOS 3.0" image at _inspiration/LisaSourceCompilation-main/
- Decompress: gunzip .cpgz → cpio extract → 48MB raw ProFile
- prof_entry at $FE0090 reads blocks (24 reads successful)
- Needs more work (LDRLDR halts after spare table read)

## Boot Sequence
```
✅ ROM → PASCALINIT → %initstdio → GETLDMAP (clean, zero exceptions)
✅ INITSYS: POOL_INIT, INTSOFF, INIT_NMI_TRAPV, REG_TO_MAPPED
✅ FP library (NEWFPSUB) — fixed via duplicate label resolution
✅ DRIVERASM reached (driver framework)
✅ Filesystem code reached ($06227C)
✅ Display output — OS draws to screen (asymmetric patterns)
💥 SYSTEM_ERROR(0) — stack corruption, CPU halted
❌ BOOT_IO_INIT → ProFile handshake
❌ FS_INIT → mount boot volume  
❌ INTRINSIC.LIB loading
❌ ENTER_SCHEDULER → Desktop
```

## Key Files
```
src/toolchain/asm68k.c            — duplicate label resolution (multidef)
src/toolchain/pascal_codegen.c    — WITH, expr_size, ORD(), 30+ fixes
src/toolchain/toolchain_bridge.c  — type remapping, HLE export, program skip
src/toolchain/linker.c            — module map, vectors
src/toolchain/bootrom.c           — prof_entry, loader stub, PROM checksum
src/profile.c/h                   — ProFile protocol (ready, untested)
src/lisa.c                        — boot params, HLE handlers, loader fs
src/m68k.c                        — HLE callback, traces, SP watermark
```
