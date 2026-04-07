# LisaEmu — Next Session Plan (April 7, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source   # Cross-compiled OS (the main path)
```

## Where We Are

**Past stack corruption, jump table crashes, and stack location bugs.**
GETLDMAP copies loader parameters correctly. REG_TO_MAPPED transitions
from physical to mapped addresses correctly. The boot reaches POOL_INIT.

**Stops at:** SYSTEM_ERROR(0) — POOL_INIT gets wrong parameters.
The codegen pushes wrong values for the POOL_INIT call (frame pointer
values instead of actual memory region parameters). This is likely a
codegen issue with evaluating expressions like `MMU_BASE(sysglobmmu)`.

## What Was Fixed (this session)

### 1. Variant record sizing (parser)
Variant records had zero size → LINK A6,#0 → stack corruption.
Fix: Parse variant fields and use largest arm for sizing.

### 2. DRIVERASM symbol priority (linker)
DRIVERASM thunks shadowed real implementations → jump table crashes.
Fix: DRIVERASM entries have lowest linker priority.

### 3. Duplicate global symbols (codegen)
Interface + external declarations created two entries per procedure.
Fix: add_global_sym returns existing symbols by name.

### 4. Procedure vs function distinction (codegen)
Added is_function flag to proc signatures. Procedures in expressions
push their address; functions are called.

### 5. Boot stack in user stack area (lisa.c + bootrom)
The boot ROM set SP/A6 to $079000 (low memory), but REG_TO_MAPPED
expects the stack in the user stack segment ($EA800-$EE800).
Fix: Patch ROM SSP, A7, and A6 to user stack top before reset.
GETLDMAP now writes to the correct physical addresses, and
REG_TO_MAPPED correctly translates them to mapped addresses.

## Current Blocker: POOL_INIT Parameter Expressions

POOL_INIT call in INITSYS:
```pascal
POOL_INIT(MMU_BASE(sysglobmmu), l_sys_global,
          MMU_BASE(sysglobmmu)+b_sgheap-b_sys_global, l_sgheap,
          MMU_BASE(syslocmmu), l_opsyslocal);
```

The codegen pushes wrong values:
- mb_sysglob=$F7FFF4 (frame pointer, should be MMU_BASE result)
- l_sysglob=$4B7A (code address, should be $6000)

MMU_BASE is likely a function/macro: `function MMU_BASE(seg: integer): absptr`
that computes `seg * $20000`. The codegen needs to correctly evaluate
this function call and the arithmetic expressions.

### Verified working
- Parameter block at $D3100: version=22, all fields correct ✓
- GETLDMAP copies to INITSYS locals: version=22, b_sys_global=$D4800 ✓
- REG_TO_MAPPED: physical $EE7F4 → mapped $CBFFE0 ✓
- INTSOFF/INTSON resolve to mover.text implementations ✓

### Next step
Check how the codegen generates the POOL_INIT arguments. The issue is
likely that `MMU_BASE(sysglobmmu)` or `l_sys_global` are not being
evaluated correctly — the codegen may be emitting wrong A6-relative
offsets for these local variables.

## Boot Sequence
```
✅ ROM → SSP=$EE800, A6=$EE800 (user stack area)
✅ PASCALINIT → %initstdio → returns param block ptr ($D3100)
✅ INITSYS → GETLDMAP copies 582 bytes of boot parameters
✅ REG_TO_MAPPED: physical→mapped stack transition
✅ POOL_INIT reached (but wrong parameters)
💥 SYSTEM_ERROR(0) after stack overflow
❌ BOOT_IO_INIT → ProFile handshake
❌ FS_INIT → mount boot volume  
❌ ENTER_SCHEDULER → Desktop
```

## Key Files
```
src/toolchain/pascal_parser.c   — variant record field parsing
src/toolchain/pascal_codegen.c  — dedup globals, proc/func distinction
src/toolchain/pascal_codegen.h  — is_function flag in proc_sig
src/toolchain/linker.c          — DRIVERASM priority, first-entry-wins
src/m68k.c                      — SP delta trace, boot traces
src/lisa.c                      — boot stack relocation, parameter block
src/toolchain/bootrom.c         — ROM code (SP=$079000 patched at runtime)
```
