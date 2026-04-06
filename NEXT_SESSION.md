# LisaEmu — Next Session Plan (April 6, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source   # Takes ~15s to compile+link+boot
```

## Where We Are

Massive session: 18 commits fixing 3 assembler bugs, 21 codegen type-size
issues, 4 cross-unit type resolution bugs, standalone program removal, and
ProFile protocol rewrite. The OS boots cleanly through PASCALINIT and deep
into INITSYS.

**What you see**: SDL window with white screen and cursor box. No artifacts.

**Current state**: CPU at $2F4BA (LIBFP area) with Illegal Instruction
exceptions at $2A (vector table). The standalone program removal fixed the
Line-F crash but exposed this new issue. Binary is 840KB (325 compiled,
103 assembled).

## What Was Fixed This Session

### Assembler (3 bugs)
1. MOVEM register list parsing (D0-D7/A0-A6 parsed as D0)
2. @-label scoping (global → per-scope with counter)
3. Branch size consistency (pass-1/pass-2 PC mismatch — ROOT CAUSE)

### Pascal Codegen (21 issues, all addressed)
4. Record field offsets (hardcoded 0 → type-resolved)
5. Array element sizes (hardcoded 2 → type-resolved)
6. VAR param read/write sizes (MOVE.W → size-aware)
7. Array/field/deref read sizes (MOVE.W → size-aware)
8. Complex LHS assignment sizes (save/restore/store correct)
9. Binary ops .W → .L for longint/pointer operands
10. Comparison ops CMP.W → CMP.L for longint
11. Unary NEG/NOT .W → .L for longint
12. Call parameter push sizes (signature-aware)
13. Call stack cleanup (sum actual sizes, not assume 4)
14. FOR loop .W → .L for longint variables
15. CASE selector .W → .L for longint
16. ABS/SUCC/PRED .W → .L for longint

### Cross-Unit Type Resolution (4 bugs)
17. Dangling type pointers after codegen_free() (pointer remapping)
18. Type alias name overwriting (create copies instead)
19. Forward-declared procedure param reconstruction
20. 2-child array and string subscript handling

### Module Filtering
21. 13 standalone programs removed from kernel (KEYBOARD, STUNTS, etc.)

### Emulator
22. Line-F/Line-A ROM skip handlers
23. ProFile protocol rewrite (5 critical fixes)
24. Exception vector overrides
25. Line-F recursion guard
26. Unmapped RAM jump trace

### Verification
27. Codegen verification tool (12 test patterns, all pass)
28. Module map logging in linker

## What Still Needs Fixing

### 1. Illegal Instruction at $2A
CPU executes in vector table again. Different pattern from the Line-F
crash — now it's vector $2A (Line-A vector address). The OS handler
at $C5E66 catches it but the loop continues. Similar root cause:
execution reaches unmapped/incorrect code.

### 2. ProFile Disk I/O
Protocol rewritten and ready but never tested. Boot hasn't reached
BOOT_IO_INIT yet.

### 3. INTRINSIC.LIB
Missing from disk image. INITSYS will halt with SYSTEM_ERROR(10100).
Need to compile library sources and package into INTRINSIC.LIB.

### 4. Codegen Edge Cases
The verification tool passes 12 patterns but the boot still crashes.
Likely remaining issues:
- WITH statement record access
- String operations
- Procedure parameters as function pointers
- Nested unit type imports in specific patterns

## Key Files

```
src/toolchain/asm68k.c              — 3 assembler fixes
src/toolchain/pascal_codegen.c       — 21 codegen type-size fixes + expr_size()
src/toolchain/pascal_codegen.h       — param_name/param_type in proc sigs
src/toolchain/toolchain_bridge.c     — type pointer remapping, program skip list
src/toolchain/test_codegen_verify.c  — 12-pattern verification tool
src/toolchain/linker.c               — module map, LINE1111_TRAP exclusion
src/profile.c/h                      — two-phase handshake, spare table
src/lisa.c                           — vector overrides, VIA BSY edges
src/m68k.c                           — Line-F guard, unmapped RAM trace
```
