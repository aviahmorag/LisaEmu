# LisaEmu — Next Session Plan (April 6, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source   # Takes ~15s to compile+link+boot
```

## Where We Are

The Apple Lisa OS cross-compiles from 420+ source files and boots
through INITSYS with screen output visible. With a temporary bypass
of %initstdio (console init), the full init chain runs.

**What you see**: SDL window with Lisa display. Screen shows artifacts
during boot (the OS is writing to the display during SYSTEM_ERROR
handling and init code).

## What Still Needs Fixing (in priority order)

### 1. %initstdio not returning (HACK IN PLACE)

**Status**: Bypassed with `RTS` patch at runtime. Need proper fix.

**Root cause identified**: `%initstdio` (in source-osintpaslib.text)
calls `initio` (SCC serial init), then `SETCUR` (cursor display),
then `traptohw CursorDisplay` (TRAP #5). The function never returns
to PASCALINIT.

**What we know**:
- `initio` works — TRAP #7 correctly disables/restores interrupts
- `chan_select=0` so SCC init is skipped (correct for screen console)
- TRAP #5 handler at $FE0330 correctly returns screen address in A0
  for ScreenAddr ($18) and AltScreenAddr ($1A) functions
- SETCUR calls SETA1 which calls `traptohw AltScreenAddr` — this
  should work with our handler

**What needs investigation**:
- Does our TRAP #5 handler corrupt the stack? (6-byte exception frame)
- Does SETCUR's `NOT.B (A1)` write to a valid screen address?
- Does the MOVEM.L restore at the end of %initstdio execute?
- Is there a register save/restore mismatch in the traptohw macro?

**Where to look**: `source-osintpaslib.text.unix.txt` lines 1354-1371
(%initstdio), 2214-2247 (SETCUR/SETA1), and the TRAPTOHW macro
definition at lines 1259-1264.

**The bypass**: In `src/lisa.c`, after loading system.os, we read the
PASCALINIT address from the main body JSR, compute %initstdio's
address, and patch its first instruction to RTS. This is ~20 lines
of code near the "Boot device and low-memory parameters" comment.
Remove once the real fix is in.

### 2. SYSTEM_ERROR(0) during INITSYS

**Status**: Called 5 times from $6C90C during boot.

Error code 0 is unusual — might be:
- A bad parameter from corrupted data (the %initstdio bypass
  skips console init, so writeln/readln don't work)
- A codegen issue where an error number isn't set correctly
- A function returning 0 when it should return an error code

**To investigate**: Find what function is at $6C90C. Check if it's
related to console I/O (writeln/readln to uninitialized console).

### 3. ProFile Disk I/O Protocol

**Status**: Protocol-accurate module written (`src/profile.c/h`)
but never tested — the OS hasn't reached BOOT_IO_INIT's ProFile
initialization code yet.

**What we have**: Full state machine matching SOURCE-PROFILEASM.TEXT:
BSY/CMD/DIR signaling, 6-byte command, 4-byte status, 532-byte
sector transfer. From LisaEm reference: exact VIA handshake protocol.

**What needs to happen**:
1. Once %initstdio is fixed and INITSYS progresses past INIT_PE/
   POOL_INIT/INIT_TRAPV/INIT_PROCESS, it reaches BOOT_IO_INIT
2. BOOT_IO_INIT calls INIT_BOOT_CDS which initializes the ProFile
   driver via the VIA1 parallel port
3. The ProFile driver does PROF_INIT which reads block $FFFFFF
   (spare table) to identify the drive
4. Test and debug the VIA handshake protocol

### 4. MDDF / Filesystem Mount

**Status**: MDDF fsversion changed from $1000 to 17 (correct).
Full field layout implemented in diskimage.c.

**What needs to happen**:
1. After ProFile driver initializes, FS_INIT calls FS_MASTER_INIT
2. FS_MASTER_INIT calls real_mount which reads the MDDF from block 24
3. real_mount validates fsversion (14/15/17), MDDFaddr (=0), volname
4. If validation passes, the filesystem is mounted and INITSYS
   continues to SYS_PROC_INIT

**Risk**: The MDDF field offsets in diskimage.c might not be exactly
right. The real MDDFdb record has 67 fields totaling ~768 bytes.
Our implementation may have wrong offsets for some fields.

### 5. Intrinsic Library Loading

**Status**: Not implemented. The OS expects to find library files
on the ProFile disk.

After FS_INIT mounts the boot volume, the OS needs to:
1. Read INTRINSIC.LIB from disk
2. Load library segments (SYS1LIB, IOSFPLIB, IOSPASLIB, etc.)
3. Map them into MMU segments

Our disk image has system.os but no library files. We need to either:
- Add library file entries to the disk image builder
- OR implement HLE (High-Level Emulation) stubs that pretend
  the libraries are loaded

### 6. VIA Timer Interrupts / Scheduler

**Status**: Vretrace disabled during first 200 frames to prevent
SYSTEM_ERROR(10605) from uninitialized interrupt handlers.

After INIT_TRAPV installs real handlers, vretrace should be enabled.
The scheduler uses `STOP #$2000` to wait for interrupts. The
vretrace (level 1) needs to match what the scheduler expects.

### 7. Assembly `*+N` Expressions (PARTIALLY FIXED)

**Status**: PC-relative encoding works for `lea *+6,a0`. But this
changes the instruction from 6 bytes (abs.L) to 4 bytes (d16(PC)),
which affects all code that assumes `*+N` with specific byte counts.

**Risk**: Other `*+N` expressions in the codebase might have wrong
offsets due to the size change. Need to verify no other assembly
files are affected.

## Architecture Summary

### Toolchain
```
Parser:    420+ files, 0 parse errors (except BUILDLLD which isn't code)
Assembler: 100+ files, $SELF relocations for local labels, PC-relative *+N
Codegen:   Multi-param, proc sigs, nested scope, FOR MOVE.W, CONST propagation
Linker:    3106 kernel-resolved, 46-module kernel from ALEX-LINK-SYSTEMOS.TEXT
Output:    ~918KB system.os
```

### Emulator
```
CPU:       68000 with TRAP handling, exception frames
MMU:       5 contexts, segments 0-20, 85-105, 123 pre-programmed
VIA:       Timer interrupts, ProFile parallel port
ProFile:   Protocol-accurate state machine (untested)
Display:   720×364 monochrome at $1F8000 (top of 2MB RAM)
Boot ROM:  Identity MMU, TRAP #5/#7/#8 handlers, screen clear
```

### Boot Sequence (how far we get)
```
✅ ROM ($FE0400) → set SP/A5/A6/SR
✅ Program 16 MMU identity segments + I/O + ROM + OS segments
✅ Exit setup mode → JMP $400
✅ BRA.W to main body
✅ MOVE.L D0,-(SP); JSR PASCALINIT
✅ PASCALINIT: copy runtime data, set A5=$E7FFC, TRAP #7 handler
⚠️  %initstdio: BYPASSED (patched to RTS)
✅ BSR.S mapiospace: install TRAP #8 handler
✅ PASCALINIT return → MOVE.L A2,(SP); JMP (A0)
✅ Main body: MOVE.L D0,-(SP); JSR INITSYS
✅ INITSYS entry: LINK A6,#-684
⚠️  INITSYS body: INTSOFF, GETLDMAP, REG_TO_MAPPED...
❌ SYSTEM_ERROR(0) × 5 — init chain partially fails
❌ BOOT_IO_INIT — not reached yet (needs error investigation)
❌ FS_INIT — not reached
❌ SYS_PROC_INIT — not reached
❌ ENTER_SCHEDULER — not reached
❌ Desktop drawing — not reached
```

## Key Files

```
src/toolchain/pascal_parser.c    — Multi-param fix, semicolon tolerance
src/toolchain/pascal_codegen.c   — FOR MOVE.W, VAR nested scope, array bounds
src/toolchain/pascal_lexer.c     — Cross-library {$I} search
src/toolchain/asm68k.c           — $SELF relocations, PC-relative *+N
src/toolchain/linker.c           — Non-kernel isolation, $SELF handling
src/toolchain/toolchain_bridge.c — Kernel selection, MAX_SHARED_GLOBALS=65536
src/toolchain/bootrom.c          — MMU mapping, TRAP #5 handler
src/toolchain/diskimage.c        — MDDF fsversion=17
src/profile.c/h                  — ProFile VIA handshake protocol
src/lisa_mmu.h/c                 — 5 contexts, register writes, translation
src/lisa.c                       — Boot env, %initstdio bypass, I/O regs
src/m68k.c                       — CPU, exception handling, traces
scripts/patch_source.sh          — Source preprocessing (10 patches)
```

## Reference Projects (_inspiration/)

### LisaEm (Ray Arachelian, GPL v3)
- ProFile VIA protocol: `src/storage/profile.c`
- Z8530 serial: `src/lisa/io_board/z8530.c`
- MMU: `src/lisa/cpu_board/mmu.c`
- Key: how TRAP #5 (TRAPTOHW) dispatches to hardware functions

### LisaSourceCompilation (AlexTheCat123)
- 46-module kernel list: `src/LINK/ALEX-LINK-SYSTEMOS.TEXT`
- Build order: `src/MAKE/ALEX-MAKE-ALL.TEXT`
- Source patches: `scripts/patch_files.py` (56 patches, we apply 10)
- glue.c: OS version detection for custom-compiled OS
