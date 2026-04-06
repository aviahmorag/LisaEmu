# LisaEmu — Next Session Plan (April 6, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source   # Takes ~15s to compile+link+boot
```

## Where We Are

Massive session: 22 commits. The toolchain is fundamentally more correct.
The OS boots through PASCALINIT + %initstdio + deep into INITSYS. Screen
shows white background with cursor box. The CPU runs OS init code actively
but eventually hits stack overflow that corrupts the vector table.

**Binary**: 855KB, 319 compiled Pascal files, 103 assembled files.

## What Was Fixed This Session

### Assembler (3 bugs)
1. **MOVEM register list parsing** — D0-D7/A0-A6 parsed as D0
2. **@-label scoping** — global → per-scope with counter
3. **Branch size consistency** — pass-1/pass-2 PC mismatch (ROOT CAUSE of original crash)

### Pascal Codegen (25+ issues)
4. **Record field offsets** — hardcoded 0 → type-resolved
5. **Array element sizes** — hardcoded 2 → type-resolved
6. **Type-aware MOVE sizes** — VAR params, array/field/deref, complex LHS
7. **Binary/unary ops** — .W → .L for longint/pointer
8. **Call parameter sizes** — signature-aware push/pop
9. **FOR/CASE** — .W → .L for longint
10. **ABS/SUCC/PRED** — .W → .L for longint
11. **WITH statement** — COMPLETELY MISSING, now implemented (206 instances in OS)
12. **Cross-unit types** — dangling pointers, aliases, forward params
13. **expr_size()** — new function for expression type inference

### Module Filtering
14. **16 standalone programs removed** — KEYBOARD (caused crash), STUNTS, DRVRMAIN, etc.

### Emulator
15. **Line-F/Line-A ROM handlers** + recursion guard
16. **ProFile protocol rewrite** — two-phase handshake, spare table, state machine
17. **Exception vector overrides** + unmapped RAM trace

### Tools
18. **Codegen verification tool** — 12 test patterns, all pass
19. **Module map logging** in linker output

## Current Blocker: Stack Overflow → Vector Corruption

The OS init code runs actively but eventually the stack overflows past
address $0, overwriting the vector table. We can see the Illegal
Instruction vector change from $C9916 to $5A9916 mid-boot.

**Root cause**: unbalanced push/pop or wrong LINK frame sizes in
generated code. The stack grows without bound until it wraps.

**Evidence**:
- PC ring buffer shows normal function calls, then RTS returns to garbage
- Vector table bytes get overwritten with code-like patterns ($5Axxxx)
- SR shows supervisor mode throughout — no user-mode stack switch

**Most likely causes** (investigate next session):
1. **Remaining codegen edge cases** — the WITH implementation handles
   basic patterns but may miss nested WITH on complex expressions
   (WITH arr[i]^ DO, WITH rec.sub^ DO, etc.)
2. **LINK frame sizes** — local variable allocation might be wrong for
   procedures with many locals or complex types (records, arrays)
3. **Callee-clean vs caller-clean mismatch** — if an assembly routine
   expects callee-clean but our codegen generates caller-clean, each
   call leaks stack bytes
4. **String operations** — writeln/readln stub returns might not clean
   up the stack correctly (the stub CLR.L D0; RTS doesn't pop params)

**Debugging approach**:
- Add a stack watermark: log when SP goes below $70000 (getting close
  to the vector table at $0-$3FF)
- The PC ring buffer at that moment will show which function is causing
  the leak
- Fix the specific codegen pattern

## Architecture Summary

### Toolchain
```
Parser:    420+ files, 0 parse errors (except BUILDLLD)
Assembler: MOVEM, @-labels, branch sizes all fixed
Codegen:   WITH support, type-aware ops, expr_size(), cross-unit types
Linker:    319 kernel modules, module map, standalone program filtering
Output:    ~855KB system.os
```

### Emulator
```
CPU:       68000 with TRAP handling, Line-F guard, unmapped RAM trace
MMU:       5 contexts, translation verified correct
VIA:       Timer interrupts, ProFile parallel port, CA1 BSY edges
ProFile:   Protocol-accurate state machine (untested — boot doesn't reach it)
Display:   720x364 monochrome at $1F8000 (top of 2MB RAM)
Boot ROM:  Identity MMU, TRAP #5/#7/#8, Line-A/F skip handlers
```

### Boot Sequence
```
✅ ROM ($FE0400) → set SP/A5/A6/SR
✅ Program 16 MMU identity segments + I/O + ROM + OS segments
✅ Exit setup mode → JMP $400
✅ BRA.W to main body → JSR PASCALINIT
✅ PASCALINIT: copy runtime, set A5, TRAP #7 handler
✅ %initstdio: console init, cursor setup (FULLY WORKING)
✅ PASCALINIT return → JMP main body → JSR INITSYS
✅ INITSYS: deep init — POOL_INIT, INTSOFF, INIT_NMI_TRAPV, REG_TO_MAPPED
✅ Screen cleared, cursor box drawn
⚠️  Stack overflow during later init → vector table corruption → crash
--- not yet reached ---
❌ BOOT_IO_INIT → ProFile handshake
❌ FS_INIT → mount boot volume
❌ INTRINSIC.LIB loading
❌ SYS_PROC_INIT → ENTER_SCHEDULER
❌ Desktop drawing
```

## Key Files

```
src/toolchain/asm68k.c              — 3 assembler fixes
src/toolchain/pascal_codegen.c       — WITH support, 25+ type-size fixes, expr_size()
src/toolchain/pascal_codegen.h       — WITH stack, param types in proc sigs
src/toolchain/toolchain_bridge.c     — type pointer remapping, program skip list
src/toolchain/test_codegen_verify.c  — 12-pattern verification tool
src/toolchain/linker.c               — module map, LINE1111_TRAP exclusion
src/profile.c/h                      — two-phase handshake, spare table
src/lisa.c                           — vector overrides, VIA BSY edges, diagnostics
src/m68k.c                           — Line-F guard, unmapped RAM trace, PC ring
```
