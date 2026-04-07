# LisaEmu — Next Session Plan (April 7, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source   # SDL standalone
# OR open lisaOS/lisaOS.xcodeproj in Xcode and Run
```

## Where We Are

**Clean boot — zero crashes, zero exceptions.** The OS boots through
PASCALINIT, %initstdio, GETLDMAP, and deep into INITSYS. No code
overwrites, no vector corruption, no A5 corruption.

**Stuck at:** CALLDRIVER loop in BOOT_IO_INIT. The OS tries to init
the boot device but `config_ptr=NULL` — the device configuration was
never created by MAKE_BUILTIN/INIT_CONFIG.

**Screen:** White (bootrom clear). The OS hasn't drawn anything yet —
it hasn't gotten far enough past disk init.

## This Session: 30 commits

### Assembler (3 bugs)
1. MOVEM register list parsing
2. @-label scoping
3. Branch size consistency (pass-1/pass-2 mismatch)

### Codegen (30+ issues)
4. Record field offsets (hardcoded 0)
5. Array element sizes (hardcoded 2)
6. Type-aware MOVE sizes (VAR params, arrays, fields, derefs)
7. Binary/unary/comparison ops .W → .L for longint
8. Call parameter push sizes (signature-aware)
9. FOR/CASE .W → .L for longint
10. ABS/SUCC/PRED .W → .L
11. **WITH statement** — completely missing, now implemented (206 OS instances)
12. Cross-unit type resolution (dangling pointers, aliases, forward params)
13. **ORD() on pointers** — returned 2 instead of 4, truncating pointers.
    This was the ROOT CAUSE of the code overwrite crash.
14. expr_size() for intrinsic functions (ORD, ORD4, POINTER)

### Loader/Linker
15. smt_base moved above OS code (was in vector table at $280)
16. Driver JT at $210 → DRIVERASM base (was RTS stubs)
17. Global variable offsets: positive → negative (Lisa Pascal convention)
18. Standalone programs removed (KEYBOARD, STUNTS, DRVRMAIN, etc.)
19. LINE1111_TRAP not pre-installed

### Emulator
20. Line-F/Line-A ROM skip handlers + recursion guard
21. ProFile protocol rewrite (two-phase handshake, spare table)
22. Exception vector overrides
23. Code write watchpoints, A5 tracking, unmapped RAM trace, SP watermark

### Tools
24. Codegen verification tool (12+ test patterns)
25. Module map logging in linker

## Current Blocker: Device Config Not Created

CALLDRIVER in BOOT_IO_INIT loops with:
- `config_ptr = NULL` (no device config for boot device)
- `fnctn_code = 0` (dskunclamp — params uninitialized)
- No VIA1 port B writes (ProFile CMD never asserted)

The OS's INIT_CONFIG → MAKE_BUILTIN should create device configs
during init, but it's not creating one for the ProFile (bootdev=2).

### Possible causes:
1. **DRIVERASM JT offset mismatch** — CALLDRIVER does `JMP 90(A0)`.
   We write CANCEL_REQ (first DRIVERASM symbol) to $210, but offset
   90 might not land on the CALLDRIVER implementation. Need to verify
   the DRIVERASM internal layout.

2. **MAKE_BUILTIN codegen issue** — the function might have a codegen
   bug (wrong field offset, type size, etc.) causing it to fail silently.

3. **Boot device detection** — dev_type at $22E=1 (profile), but
   MAKE_BUILTIN might check hardware (VIA1 OCD bit) and get wrong result.

4. **Missing driver init sequence** — the real Lisa boot loader runs
   additional init steps before INITSYS that we skip.

### Two approaches:
**A. Fix MAKE_BUILTIN** — trace why it doesn't create a config. This
   is the "proper" approach that uses our cross-compiled OS code.

**B. HLE approach** — like LisaEm, patch OS code with F-line traps
   at known disk I/O addresses. This bypasses the driver/VIA path
   entirely. Simpler but specific to LOS 3.1.

### Reference: How LisaEm does it
LisaEm uses F-line HLE (High-Level Emulation):
- Patches OS code at hardcoded addresses with `$F33D` (Line-F trap)
- When CPU hits patched address, `hle_intercept()` performs disk I/O
- Skips entire driver → VIA → ProFile path
- Source: `_inspiration/lisaem-master/src/storage/hle.c`

## Boot Sequence
```
✅ ROM → PASCALINIT → %initstdio → GETLDMAP (all clean, zero exceptions)
✅ INITSYS: POOL_INIT, INTSOFF, INIT_NMI_TRAPV, REG_TO_MAPPED
✅ Screen cleared by bootrom
✅ A5 correctly set to mapped sysglobal ($CC5FFC)
✅ No code overwrites, no vector corruption
⏳ INIT_CONFIG / MAKE_BUILTIN — device config not created
⏳ CALLDRIVER loop (config_ptr=NULL, no driver dispatch)
❌ ProFile handshake (VIA1 CMD never asserted)
❌ FS_INIT → mount boot volume
❌ INTRINSIC.LIB loading
❌ ENTER_SCHEDULER → Desktop
```

## Key Files
```
src/toolchain/pascal_codegen.c    — WITH, expr_size, 30+ type fixes
src/toolchain/pascal_codegen.h    — WITH stack, param types
src/toolchain/toolchain_bridge.c  — type remapping, program skip list
src/toolchain/asm68k.c            — MOVEM, @-labels, branch sizes
src/toolchain/linker.c            — module map, DRIVERASM JT, vectors
src/profile.c/h                   — ProFile protocol (ready, untested)
src/lisa.c                        — boot params, smt_base, diagnostics
src/m68k.c                        — Line-F guard, traces, watchpoints
```
