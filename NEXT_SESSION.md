# LisaEmu — Next Session Plan (April 6, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source   # Takes ~15s to compile+link+boot
```

## Where We Are

The Apple Lisa OS cross-compiles from 420+ source files and boots
cleanly through PASCALINIT, %initstdio, and deep into INITSYS.
No runtime patches — the code runs natively.

**What you see**: SDL window with white screen and a black box in
the lower-left corner. The OS has cleared the display and drawn
a cursor or dialog element. No artifacts, no crashes.

**CPU state**: Stable at PC=$D0A6A (waiting for I/O or interrupt).
Only 4 total exceptions during boot, all normal TRAPs. SR=$2719
(supervisor mode, IPL=7).

## What Was Fixed This Session

### 1. MOVEM register list parsing (assembler)
`D0-D7/A0-A6` was parsed as single register D0. Generated $4CC0 $0000
instead of $48E7 $FFFE. Fix: check register lists before single registers.

### 2. @-label scoping (assembler)
Local labels (@1, @3) were global instead of scoped per major label.
Fix: scope counter incremented at .PROC/.FUNC/major labels, @-labels
mangled with scope ID.

### 3. Branch size consistency (assembler — ROOT CAUSE OF CRASH)
Bcc/BRA/BSR auto-optimized to byte displacement when target fit in
-128..+127. On pass 1, forward @-label refs → 0 → 4-byte branch.
On pass 2, real value → fits byte → 2-byte branch. This caused ALL
subsequent labels to drift by 2+ bytes between passes.

The `initio` label pointed 2 bytes past `move.l (SP)+,a1`, so the
return address was never saved. Stack corruption → vector table
execution → Line-F/Trace exception loop → total crash.

Fix: only use byte displacement with explicit `.S` suffix. Otherwise
always use word displacement (4 bytes).

### 4. Line-F exception handler
OS's LINE1111_TRAP handler calls system_error (not ready). Overridden
with ROM skip handler during early boot. Also added recursion guard
in take_exception for safety.

### 5. ProFile protocol rewrite
Five critical fixes: proper two-phase handshake, block $FFFFFF spare
table, correct state transitions, PORTA direction, VIA CA1 BSY edges.

## What Still Needs Fixing (in priority order)

### 1. CPU waiting at $D0A6A — identify what it's waiting for

The boot reaches deep into INITSYS (POOL_INIT confirmed). The CPU
is now in a stable wait loop. Likely causes:
- BOOT_IO_INIT trying to access ProFile (VIA handshake not triggering)
- Waiting for a VIA interrupt that never fires
- STOP #$2000 instruction waiting for scheduler interrupt

**To investigate**: Decode what's at $D0A6A. Check if it's a BTST
loop polling a VIA register or a STOP instruction.

### 2. ProFile disk I/O testing

The rewritten ProFile protocol hasn't been exercised yet. Once the
wait loop is resolved, BOOT_IO_INIT should try to read from ProFile.
The VIA handshake and spare table read need real testing.

### 3. INTRINSIC.LIB missing from disk image

INITSYS calls Setup_IUInfo which opens INTRINSIC.LIB. Without it,
SYSTEM_ERROR(10100) will halt the boot. Need to:
- Compile library sources (LIBQD, LIBWM, LIBSM, etc.)
- Link them per BUILD/ALEX-LINK-*.TEXT scripts
- Package into INTRINSIC.LIB with proper directory structure
- Add to disk image

### 4. MDDF / Filesystem Mount

MDDF layout verified correct (fsversion=17, MDDFaddr=0, all fields
match OS validation). Should work once ProFile reads succeed.

### 5. VIA Timer Interrupts / Scheduler

Vretrace disabled during first 200 frames. After INIT_TRAPV installs
real handlers, vretrace should be enabled. The scheduler uses
STOP #$2000 to wait for level-1 interrupts.

## Architecture Summary

### Toolchain
```
Parser:    420+ files, 0 parse errors (except BUILDLLD which isn't code)
Assembler: MOVEM fixed, @-labels scoped, branch sizes consistent, PC-relative *+N
Codegen:   Multi-param, proc sigs, nested scope, FOR MOVE.W, CONST propagation
Linker:    3106 kernel-resolved, 46-module kernel, LINE1111_TRAP excluded
Output:    ~919KB system.os (slightly larger with word-size branches)
```

### Emulator
```
CPU:       68000 with TRAP handling, exception frames, Line-F recursion guard
MMU:       5 contexts, segments 0-20, 85-105, 123 pre-programmed
VIA:       Timer interrupts, ProFile parallel port, CA1 BSY edge detection
ProFile:   Protocol-accurate state machine with two-phase handshake
Display:   720x364 monochrome at $1F8000 (top of 2MB RAM)
Boot ROM:  Identity MMU, TRAP #5/#7/#8, Line-A/F skip handlers, screen clear
```

### Boot Sequence (how far we get)
```
 ROM ($FE0400) -> set SP/A5/A6/SR
 Program 16 MMU identity segments + I/O + ROM + OS segments
 Exit setup mode -> JMP $400
 BRA.W to main body
 MOVE.L D0,-(SP); JSR PASCALINIT
 PASCALINIT: copy runtime data, set A5, TRAP #7 handler
 %initstdio: console init, cursor setup (FULLY WORKING)
 BSR.S mapiospace: install TRAP #8 handler
 PASCALINIT return -> JMP main body
 MOVE.L D0,-(SP); JSR INITSYS
 INITSYS: INTSOFF, GETLDMAP, REG_TO_MAPPED
 INIT_PE, POOL_INIT (confirmed)
 Screen cleared, cursor box drawn
 Waiting at $D0A6A (I/O or interrupt wait)
--- not yet reached ---
 INIT_TRAPV, DB_INIT, AVAIL_INIT
 INIT_PROCESS, INIT_EM, INIT_EC
 INIT_SCTAB, INIT_MEASINFO
 BOOT_IO_INIT -> FS_INIT -> mount ProFile
 SYS_PROC_INIT -> ENTER_SCHEDULER
 Desktop drawing
```

## Key Files

```
src/toolchain/asm68k.c           -- 3 bug fixes: MOVEM, @-labels, branch sizes
src/toolchain/asm68k.h           -- local_scope field for @-label scoping
src/toolchain/linker.c           -- LINE1111_TRAP excluded from pre-install
src/toolchain/bootrom.c          -- MMU mapping, TRAP #5/#7/#8, Line-A/F handlers
src/toolchain/diskimage.c        -- MDDF fsversion=17 (verified correct)
src/profile.c/h                  -- Rewritten: two-phase handshake, spare table
src/lisa.c                       -- Line-A/F vector overrides, VIA CA1 BSY edges
src/m68k.c                       -- Line-F recursion guard, trace infrastructure
```
