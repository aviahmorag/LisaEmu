# LisaEmu — Next Session Plan

## Current State (April 4, 2026)
- **OS running continuously** — scheduler loop at $09705E, 625M+ cycles
- 276/276 Pascal, 96/96 assembly, **6,621 symbols** linked
- Binary: 1.66MB, **HAllocate RESOLVED** at $8E9BE
- No crashes, no Line-F loops, no stack overflow
- Display: white (OS hasn't drawn yet — needs interrupts)
- 10,630 JSR $000000 still patched to stub (mostly LogCall)

## What was fixed this session
1. ADDQ stack cleanup encoding (A7 odd → stack corruption)
2. Forward branch displacement off-by-2 in IF/WHILE/FOR
3. CASE statement codegen (was stub → if-else chains)
4. INTRINSIC SHARED parsing (lost entire LIBWM, LIBFM, LIBSM units)
5. INTERFACE functions as FORWARD (stopped body parsing cascade)
6. Variant record depth counting (nested CASE inside variants)
7. Linker per-module limits 4096 → 16384
8. Parser error recovery and bailout limits

## Priority 1: VIA Timer Interrupts

The OS is stuck in the scheduler loop because no interrupts fire.
The Lisa VIA1/VIA2 chips generate periodic timer interrupts that
drive the scheduler. The emulator has VIA chip code (src/via6522.c)
but it may not be generating interrupts or they may not be connected
to the CPU's interrupt system.

**Check**: Does via6522_step() assert interrupt lines? Does lisa_step()
check for pending interrupts and deliver them to the CPU?

## Priority 2: TRAP #1 System Call Handler

The OS uses TRAP #1 for all system calls. Currently they go to the
boot ROM's RTE handler (no-op). The OS's own TRAP handler is in
EXCEPASM (already assembled). The boot startup code should install
these vectors via INIT_TRAPV, but if the installation itself uses
system calls, it won't work.

**Check**: Does INIT_TRAPV write to the vector table directly?
Look at SOURCE-INITRAP.TEXT for how it installs handlers.

## Priority 3: Remaining LogCall Stubs

LogCall appears ~40+ times. It's a debug trace function never defined
in source (called behind {$IFC fTraceSM} which evaluates to true
because fTraceSM is undefined). Fix: either add LogCall as a built-in
no-op symbol in the linker, or fix the conditional compilation so
fTraceSM defaults to FALSE.

## Key metrics
- Symbols: 5,627 → 6,621 (+994)
- Line-F exceptions: 7,270,254 → 0 in current build
- Binary: 1.5MB → 1.66MB
- CPU cycles to crash: ~150M → runs indefinitely (625M+)
