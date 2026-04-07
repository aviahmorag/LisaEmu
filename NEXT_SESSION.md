# LisaEmu — Next Session Plan (April 7, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source   # Takes ~15s to compile+link+boot
```

## Where We Are

**CLEAN BOOT — zero exceptions.** The OS boots through PASCALINIT,
%initstdio, and deep into INITSYS without any crashes, code overwrites,
or vector corruption. Screen shows white background with cursor box.

**CPU at $C120E** — stable wait loop, likely waiting for ProFile disk
I/O or VIA interrupt. VIA1 IER=$00 (no interrupts enabled).

**This session: 27 commits, 35+ fixes across assembler, codegen, linker,
emulator, and loader parameter setup.**

## Key Root Causes Found & Fixed

1. **MOVEM register list parsing** — D0-D7/A0-A6 parsed as D0
2. **@-label scoping** — local labels global instead of per-scope
3. **Branch size consistency** — pass-1/pass-2 PC mismatch
4. **Record field offsets** — hardcoded 0 → type-resolved
5. **Array element sizes** — hardcoded 2 → type-resolved
6. **21 type-size issues** — all ops now type-aware (.W vs .L)
7. **WITH statement** — completely missing, now implemented (206 instances)
8. **Cross-unit type resolution** — dangling pointers, aliases, forward params
9. **Standalone programs in kernel** — KEYBOARD, STUNTS, DRVRMAIN removed
10. **ORD() on pointers** — returned 2 instead of 4, truncating pointers
11. **smt_base in code space** — moved above OS code
12. **Global variable offsets** — positive → negative (Lisa Pascal convention)

## What's Next

### 1. Identify wait at $C120E
The CPU is in a stable loop. Check if it's:
- Polling for ProFile BSY (VIA1 CA1)
- Waiting for STOP #$2000 (scheduler interrupt)
- BOOT_IO_INIT trying disk access

### 2. ProFile disk I/O
Protocol rewritten with proper two-phase handshake. Once the wait
loop is resolved, ProFile reads should work. Test spare table read
(block $FFFFFF) and data block reads.

### 3. INTRINSIC.LIB
Missing from disk image. SYSTEM_ERROR(10100) will halt.

### 4. Filesystem mount
MDDF verified correct. Should work once ProFile I/O works.

## Boot Sequence
```
✅ ROM → PASCALINIT → %initstdio → INITSYS (all clean)
✅ POOL_INIT, INTSOFF, INIT_NMI_TRAPV, REG_TO_MAPPED
✅ Screen cleared, cursor box drawn
✅ No exceptions, no code overwrites, no vector corruption
⏳ Waiting at $C120E (ProFile/VIA interrupt?)
❌ BOOT_IO_INIT → ProFile handshake
❌ FS_INIT → mount boot volume
❌ INTRINSIC.LIB loading
❌ ENTER_SCHEDULER → Desktop
```
