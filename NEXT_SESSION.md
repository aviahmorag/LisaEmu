# LisaEmu — Next Session Plan (April 5, 2026)

## Quick Start

```bash
make                        # Build standalone emulator
build/lisaemu Lisa_Source   # Run from source directory (builds + boots)
```

## Current State

The Lisa OS cross-compiles from source and boots through INITSYS with
zero SYSTEM_ERROR calls. The OS runs stably at PC=$0D9242 for 480+ frames.
One zero-divide exception at the end. Screen buffer is blank (OS hasn't
reached display code yet — needs ProFile disk I/O to complete FS_INIT).

```
Linker: 8701 symbols, 3106 kernel-resolved, 5595 non-kernel (correct)
Kernel: 46 OS modules + LIBPL + LIBHW + LIBFP + LIBOS (matching real Lisa)
DIAG: PC=$0D9242 SR=$2218 stopped=0 — stable, no crashes
Exceptions: 1 zero-divide (non-fatal)
Screen: blank — init chain blocked before display drawing code
ProFile: zero disk reads — OS hasn't reached FS_INIT disk I/O yet
```

## Session 3 Summary (40 commits)

### Critical bugs fixed:
1. **Multi-parameter parsing** — `(a, b, c: type)` created 1 param named "a,b,c"
   instead of 3 separate params. Affected EVERY multi-param function. POOL_INIT
   got 1 param instead of 6 → SYSTEM_ERROR(610) "no space". FIXED.
2. **FOR loop MOVE.L→MOVE.W** — 4-byte write into 2-byte slot corrupted saved A6.
   Caused cascade failure after 260 frames. FIXED.
3. **Linker non-kernel symbol collision** — non-kernel module symbols at base_addr=0
   resolved to addresses inside kernel code. Root cause of original A6 corruption. FIXED.
4. **LIBHW directory skip** — should_skip_dir("LIBHW") always returned true because
   it checked if "LIBHW" contains "LIBS" (it doesn't). 74 COPS symbols missing. FIXED.

### Other fixes:
- Symbol pre-resolution in add_global_symbol (resolved=true set too early)
- Stub moved from end-of-code to $3F0 (safe from OS overwrite)
- VAR param nested scope in 3 codegen paths
- Cross-library {$I} include search across all LIBS/ subdirs
- MATHLIB proc-param semicolon tolerance
- CONST section semicolon tolerance (Lisa Pascal quirk)
- Array bounds default to 64 for unresolved CONST
- Screen buffer moved to $1F8000 (top of 2MB RAM)
- All screen pointers updated ($110/$160/$170/$174)
- I/O board type ($FCC031=$80=Pepsi Lisa 2)
- Internal disk type ($FCC015=1=Sony/ProFile)
- Boot device ($1B3=2=ProFile)
- SGLOBAL at $200 = b_sysglobal
- DRIVRJT at $210 = 32-entry driver jump table
- MMU: 5 contexts, register writes, identity mapping + OS segments
- Boot ROM: 16 identity segments + I/O + ROM before setup exit
- TRAP #6 vector installed for MMU programming
- Vretrace interrupt at level 2

### Architecture (matching real Lisa from LisaSourceCompilation):
- Kernel = 46 OS modules from SOURCE/ + HWINTL (from ALEX-LINK-SYSTEMOS.TEXT)
- Runtime libs: LIBPL, LIBHW, LIBFP, LIBOS (normally loaded as IOSPASLIB etc.)
- UI libs (LIBWM, LIBTK, LIBQD, etc.) = non-kernel (would be intrinsic libs on disk)
- Apps = non-kernel (separate files on disk)

## What's Blocking: ProFile Disk I/O

The OS init chain goes: INITSYS → ... → BOOT_IO_INIT → INIT_BOOT_CDS → ProFile
driver init. The ProFile driver does a VIA1 handshake to initialize the disk.

From the FS_INIT research agent, three issues remain:

### 1. ProFile VIA handshake protocol
The real ProFile protocol (SOURCE-PROFILEASM.TEXT) is a multi-step VIA handshake:
- Set CMD via ORB, wait for BSY on CA1 interrupt
- Send $55 response, wait for not-busy
- Send 6 command bytes on ORA
- Read 4 status bytes + 20-byte header + 512 data bytes

Our emulator uses a simplified command-buffer approach that doesn't match this
protocol. The OS driver will time out or get wrong responses.

### 2. MDDF format
Our disk image's MDDF may have wrong filesystem version. The OS checks fsversion
against REL1_VERSION, PEPSI_VERSION, or CUR_VERSION.

### 3. After disk I/O works
FS_INIT → FS_Mount → read MDDF → mount filesystem → SYS_PROC_INIT → create
system processes → PR_CLEANUP → ENTER_SCHEDULER → OS draws to screen

## Key Files

```
src/toolchain/pascal_parser.c   — Multi-param fix, semicolon tolerance
src/toolchain/pascal_codegen.c  — FOR loop MOVE.W, VAR nested scope, array bounds
src/toolchain/pascal_lexer.c    — Cross-library {$I} search, %_ identifiers
src/toolchain/linker.c          — Non-kernel isolation, stub at $3F0, builtins
src/toolchain/toolchain_bridge.c — Kernel module selection (OS + LIBPL/HW/FP/OS)
src/toolchain/bootrom.c         — Identity MMU mapping, screen clear
src/lisa_mmu.h/c                — 5 contexts, register writes, translation
src/lisa.c                      — Boot env, SGLOBAL, DRIVRJT, I/O regs, ProFile
src/m68k.c                      — SYSTEM_ERROR trace, code escape trace
scripts/patch_source.sh         — Source preprocessing (10 patches)
```

## Toolchain Metrics
```
Parser:    420 source files, 1 parse error (BUILDLLD — not compilable)
Assembler: 100+ files, 100% success
Codegen:   Multi-param, proc sigs, CONST, nested scope, VAR params, FOR loop
Linker:    3106 kernel-resolved, zero SYSTEM_ERROR calls
Output:    ~918KB system.os, boots through INITSYS, 480+ stable frames
```
