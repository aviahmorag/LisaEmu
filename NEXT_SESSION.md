# LisaEmu — Next Session Plan (April 6, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source   # Takes ~15s to compile+link+boot
```

## Where We Are

The Apple Lisa OS cross-compiles from 420+ source files and boots
through PASCALINIT (including %initstdio) and into INITSYS with
screen output visible. No runtime patches — the code runs natively.

**What you see**: SDL window with Lisa display. Screen is very active
with changing artifacts during boot — the OS is writing to the display
as init code runs.

**Boot chain progress** (this session):
- PASCALINIT + %initstdio: fully working (was bypassed, now fixed)
- INITSYS: enters and runs
- POOL_INIT: runs
- INTSOFF: runs  
- INIT_NMI_TRAPV: runs
- REG_TO_MAPPED: runs
- INIT_TRAPV: pending investigation
- BOOT_IO_INIT: not yet reached

## What Was Fixed This Session

### 1. MOVEM register list parsing (assembler bug)

**Root cause**: `parse_operand()` in asm68k.c checked for data registers
BEFORE register lists. When parsing `MOVEM.L D0-D7/A0-A6,-(SP)`,
`parse_data_reg("D0-D7/A0-A6")` matched `D0` (since `-` is not alphanumeric
and terminates the register name check). This caused the MOVEM to be encoded
as memory-to-register ($4CC0) instead of register-to-memory ($48E7), and with
an empty register mask ($0000 instead of $FFFE).

**Fix**: Moved the register list check (checking for `/` or `-` with register
names) BEFORE the single data/address register checks in `parse_operand()`.

### 2. @-label (local label) scoping (assembler bug)

**Root cause**: The Lisa assembler scopes @-labels (like @1, @3) between major
labels — each major label starts a new scope. Our assembler stored all @-labels
globally, so the LAST @3 in a file would overwrite all previous @3 definitions.

In `initio` (source-osintpaslib.text), `BEQ.S @3` was supposed to branch to
the @3 label 3 lines below. But there were 5 different @3 labels in the file,
and the branch resolved to the wrong one (122 bytes away instead of ~20).

**Fix**: Added `local_scope` counter to the assembler. It increments at each
major label (.PROC, .FUNC, non-@ labels). @-labels are mangled with the scope
counter (e.g., `@3` becomes `@3__42`) to make them unique per scope.

## What Still Needs Fixing (in priority order)

### 1. SYSTEM_ERROR(0) during init

**Status**: Called 5 times from $06C90C during early boot (during
%initstdio execution). Error code 0 may indicate a writeln/readln
to an uninitialized console channel, or a codegen issue.

**To investigate**: Find what function is at $06C90C. Check if it's
a SETCUR-related call trying to write cursor to screen. May be benign
(the OS continues executing despite these errors).

### 2. Boot chain continuation after REG_TO_MAPPED

The boot reaches REG_TO_MAPPED but we need to trace further:
- INIT_TRAPV should install real exception handlers
- BOOT_IO_INIT should initialize the ProFile driver
- FS_INIT should mount the boot volume

### 3. ProFile Disk I/O Protocol

**Status**: Protocol-accurate module written (`src/profile.c/h`)
but never tested — the OS hasn't reached BOOT_IO_INIT's ProFile
initialization code yet.

**What we have**: Full state machine matching SOURCE-PROFILEASM.TEXT:
BSY/CMD/DIR signaling, 6-byte command, 4-byte status, 532-byte
sector transfer. From LisaEm reference: exact VIA handshake protocol.

### 4. MDDF / Filesystem Mount

**Status**: MDDF fsversion changed from $1000 to 17 (correct).
Full field layout implemented in diskimage.c.

### 5. Intrinsic Library Loading

After FS_INIT mounts the boot volume, the OS needs to find library files
on the ProFile disk. Our disk image has system.os but no library files.

### 6. VIA Timer Interrupts / Scheduler

**Status**: Vretrace disabled during first 200 frames to prevent
SYSTEM_ERROR(10605) from uninitialized interrupt handlers.

## Architecture Summary

### Toolchain
```
Parser:    420+ files, 0 parse errors (except BUILDLLD which isn't code)
Assembler: 100+ files, MOVEM fixed, @-labels scoped, PC-relative *+N
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
Display:   720x364 monochrome at $1F8000 (top of 2MB RAM)
Boot ROM:  Identity MMU, TRAP #5/#7/#8 handlers, screen clear
```

### Boot Sequence (how far we get)
```
 ROM ($FE0400) -> set SP/A5/A6/SR
 Program 16 MMU identity segments + I/O + ROM + OS segments
 Exit setup mode -> JMP $400
 BRA.W to main body
 MOVE.L D0,-(SP); JSR PASCALINIT
 PASCALINIT: copy runtime data, set A5=$E7FFC, TRAP #7 handler
 %initstdio: WORKING (console init, cursor setup)
 BSR.S mapiospace: install TRAP #8 handler
 PASCALINIT return -> MOVE.L A2,(SP); JMP (A0)
 Main body: MOVE.L D0,-(SP); JSR INITSYS
 INITSYS entry: LINK A6,#-684
 POOL_INIT, INTSOFF, INIT_NMI_TRAPV, REG_TO_MAPPED
 SYSTEM_ERROR(0) x 5 -- init chain partially fails
 INIT_TRAPV -- pending
 BOOT_IO_INIT -- not reached yet
 FS_INIT -- not reached
 SYS_PROC_INIT -- not reached
 ENTER_SCHEDULER -- not reached
 Desktop drawing -- not reached
```

## Key Files

```
src/toolchain/asm68k.c           -- MOVEM fix, @-label scoping, PC-relative *+N
src/toolchain/pascal_parser.c    -- Multi-param fix, semicolon tolerance
src/toolchain/pascal_codegen.c   -- FOR MOVE.W, VAR nested scope, array bounds
src/toolchain/pascal_lexer.c     -- Cross-library {$I} search
src/toolchain/linker.c           -- Non-kernel isolation, $SELF handling
src/toolchain/toolchain_bridge.c -- Kernel selection, MAX_SHARED_GLOBALS=65536
src/toolchain/bootrom.c          -- MMU mapping, TRAP #5 handler
src/toolchain/diskimage.c        -- MDDF fsversion=17
src/profile.c/h                  -- ProFile VIA handshake protocol
src/lisa_mmu.h/c                 -- 5 contexts, register writes, translation
src/lisa.c                       -- Boot env, I/O regs
src/m68k.c                       -- CPU, exception handling, traces
scripts/patch_source.sh          -- Source preprocessing (10 patches)
```
