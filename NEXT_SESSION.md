# LisaEmu — Next Session Plan (April 7, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source   # Cross-compiled OS (the main path)
```

## Where We Are

**Past the stack corruption and driver jump table crashes.** The OS now
reaches INITSYS, calls GETLDMAP, REG_TO_MAPPED, and POOL_INIT. 
POOL_INIT receives wrong parameters → GETSPACE fails → SYSTEM_ERROR(10701).

**Stops at:** SYSTEM_ERROR(10701) — "no sysglobal space during startup"
POOL_INIT gets garbage parameters because the GETLDMAP copy from the
loader parameter block to INITSYS local variables has a layout mismatch.

## What Was Fixed (this session)

### 1. Variant record sizing (parser)
Variant records (`CASE ... OF ...`) had zero size because the parser
skipped all variant fields. Functions like `getexcepset` in FPMODES had
`LINK A6,#0` (no locals), so local variable writes overwrote the saved
frame pointer → UNLK A6 corrupted SP → SYSTEM_ERROR(0).

Fix: Parse variant fields and add the largest arm's fields to the record.

### 2. DRIVERASM symbol priority (linker)
DRIVERASM exports thunks (GETSPACE, INTSOFF, SYSTEM_ERROR, etc.) that
indirect through a jump table at $210. Before INIT_JTDRIVER runs, $210=0,
so any call through these thunks crashes into the vector table.

Fix: DRIVERASM symbols have lowest linker priority. Non-DRIVERASM ENTRY
symbols always replace DRIVERASM ones.

### 3. Duplicate global symbols (codegen)
Pascal interface + external declarations for the same procedure created
two global symbols. The first (ENTRY, val=0) blocked the real assembly
implementation via "first ENTRY wins" in the linker.

Fix: `add_global_sym` returns existing symbols by name instead of always
creating new entries.

### 4. Procedure vs function distinction (codegen)
`INITSYS(PASCALINIT)` was wrongly treated: PASCALINIT (a function
returning ptr) was being called as a procedure. Added `is_function` flag
to `cg_proc_sig_t`. Procedures used in expressions push their ADDRESS;
functions are called and return values.

### 5. SP delta trace (emulator)
Added per-instruction SP delta check: catches when SP changes by >$1000
in one instruction. Identified the UNLK A6 crash, the GETSPACE/DRIVERASM
jump table crashes, and verified the fixes.

## Current Blocker: GETLDMAP → POOL_INIT Parameter Mismatch

GETLDMAP copies the loader parameter block (at $D3100, version=22) into
INITSYS local variables word-by-word. The copy works (version check
passes), but POOL_INIT still receives wrong values.

### Key observations
- PASCALINIT correctly returns $D3100 (from $218/adrparamptr)
- GETLDMAP entry: ldmapbase=$D3100, version=22 ✓
- GETLDMAP code at $45A: LINK A6,#-8, reads ldmapbase at 8(A6), checks version
- @parmend = parent_A6 - 684 ($FD54 offset)
- @version = parent_A6 - 102 ($FF9A offset)
- POOL_INIT params: mb_sysglob=$F0A7F4 (A6!), l_sysglob=$4B7A (code addr)

### Likely cause
The parameter block in lisa.c has correct data at $D3100 (version=22,
fields below). GETLDMAP copies to INITSYS locals. But after REG_TO_MAPPED
transitions to mapped memory, the A6-relative local variables may be in
unmapped physical memory while the new mapped SP/A6 addresses refer to
different locations.

The physical-to-mapped transition in REG_TO_MAPPED changes the stack:
- Before: A6=$078FF4 (physical)
- After: A6=$CBFFE0 (mapped via MMU)

The GETLDMAP locals are written to the PHYSICAL stack frame ($078Fxx).
But after REG_TO_MAPPED, code accesses the MAPPED frame ($CBFFxx).
If the MMU doesn't map $078Fxx → $CBFFxx, the data is unreachable.

### Next step
Trace what REG_TO_MAPPED does. Verify that the MMU mapping preserves
access to the INITSYS locals after the transition. The issue may be:
1. The MMU segment for the stack doesn't cover the right physical range
2. The base addresses in the parameter block are wrong
3. The stack transition changes A6 but doesn't remap the old locals

### Parameter block layout (lisa.c)
```
version_addr ($D3100):  version = 22 (2 bytes)
version_addr - 4:       b_sysjt (4 bytes)
version_addr - 8:       l_sysjt
version_addr - 12:      b_sysglobal
version_addr - 16:      l_sysglobal
...
```

The layout matches parms.text variable order. The copy loop in GETLDMAP
reads 2-byte words from ldmapbase downward and writes to INITSYS locals.

## Boot Sequence
```
✅ ROM → PASCALINIT → %initstdio → GETLDMAP (version=22)
✅ INITSYS: GETLDMAP copies params, REG_TO_MAPPED transitions stack
✅ FP library (NEWFPSUB) — fixed via duplicate label resolution
✅ DRIVERASM reached (driver framework)  
✅ INTSOFF/INTSON resolve to mover.text (real implementations)
✅ SYSTEM_ERROR resolves to SYSGLOBAL (real implementation)
💥 POOL_INIT — wrong parameters from INITSYS locals
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
src/m68k.c                      — SP delta trace, crash function traces
src/lisa.c                      — parameter block layout
```
