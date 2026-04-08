# LisaEmu — Next Session Plan (April 8, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source
```

## Where We Are

**Past POOL_INIT parameter corruption — 3 major fixes this session.**
POOL_INIT now receives correct parameters ($CC0000, $6000, $CCA000, $8000, $CE0000, $4000).
Boot reaches INIT_FREEPOOL inside POOL_INIT body.

**Stops at:** SYSTEM_ERROR(10701) — "getspace failed during initialize".
INIT_FREEPOOL doesn't write pool headers because of a codegen bug in parameter access.

## What Was Fixed (this session: 3 fixes)

### 1. MMU stack segment SOR computation (lisa.c)
Lisa MMU computes SOR differently for stack segments (SLR type $600):
```
SOR = origin + length - hw_adjust  (hw_adjust = $100 pages = 128KB)
```
The emulator used `base >> 9` (data segment formula) for stack segments too.
Fix: `STACK_SOR(base, len) = (base >> 9) + (len >> 9) - 0x100` for segments 101, 123, 104.
This fixed MMU translation for all mapped stack accesses after REG_TO_MAPPED.

### 2. 32-bit multiplication (pascal_codegen.c)
M68000 MULS.W takes only low 16 bits of operands. For 32-bit values where
intermediate results overflow 16 bits (e.g., `mmu * 256 * 256 * 2`), the low
16 bits become zero → multiplication returns 0.
Fix: For `use_long` multiplication, emit inline 32×32→32 multiply using
partial products: `A_lo*B_lo + (A_hi*B_lo + A_lo*B_hi)<<16`.

### 3. 32-bit division (pascal_codegen.c)
Same issue as multiplication: DIVS.W uses low 16 bits of divisor. When
divisor > $FFFF (e.g., dividing by $20000), low 16 bits are 0 → Zero Divide.
Also: EXT.L before DIVS destroys 32-bit dividends.
Fix: For `use_long` division, emit inline binary long division loop
(shift-and-subtract, 32 iterations via DBRA).

## Current Blocker: INIT_FREEPOOL Codegen

INIT_FREEPOOL (at $BD034) is called from POOL_INIT to initialize the free pool.
Its generated code has two bugs:

### Bug 1: Parameter loaded as constant
The code for `hdr_ptr := pointer(fp_ptr)` generates `MOVE.L #$0C4B48,D0`
(loads a linker-resolved constant) instead of `MOVE.L 8(A6),D0` (reads
parameter from stack frame). The codegen treats fp_ptr as a compile-time
value instead of a runtime parameter.

This affects ALL pointer-from-parameter patterns in compiled Pascal code.

### Bug 2: Undersized LINK frame
LINK A6,#-4 allocates only 4 bytes for locals, but INIT_FREEPOOL has
two pointer variables (hdr_ptr: hdr_pool_ptr, ent_ptr: ent_pool_ptr)
needing 8 bytes. The types hdr_pool_ptr and ent_pool_ptr are not being
sized correctly by the type system.

### Root cause analysis
The `(* ... *)` parameter inheritance syntax in the implementation:
```pascal
procedure INIT_FREEPOOL(* fp_ptr: absptr; fp_size: int2 *);
```
may not properly register fp_ptr and fp_size as parameters in the
codegen's symbol table. Check:
1. How does the parser handle `(* inherited params *)` syntax?
2. Are INTERFACE declarations properly forwarded to implementation bodies?
3. Do typed pointers (hdr_pool_ptr, ent_pool_ptr) resolve to 4-byte size?

### Next steps
1. Check pascal_codegen.c parameter registration for INIT_FREEPOOL
2. Verify pointer type sizes for declared pointer types (hdr_pool_ptr etc.)
3. Fix parameter access to use A6+offset reads instead of constants
4. Fix LINK frame allocation to account for all local variable sizes

## Also Noted: TRAP #6 handler

TRAP #6 (DO_AN_MMU, MMU programming) handler is at $3F0 (vector table
data, not code). INIT_TRAPV hasn't run yet at POOL_INIT time — it runs
AFTER POOL_INIT. This means the OS can't program MMU during early boot.
For now, pre-programmed MMU segments in lisa.c handle this. Later, the
TRAP #6 vector needs to be set up before first use.

## Verified Working
```
✅ MMU stack segment SOR (hw_adjust applied)
✅ MMU write test: $CC0000 → physical $D6800 (PASS)
✅ 32-bit multiplication: MMU_BASE(102) = $CC0000
✅ 32-bit division: no Zero Divide for divisor $20000
✅ POOL_INIT params: $CC0000, $6000, $CCA000, $8000, $CE0000, $4000
✅ Local variables via mapped A6 (segment 123 SOR correct)
✅ GETLDMAP: copies 582 bytes, version=22, all fields correct
✅ REG_TO_MAPPED: A6/A7 transition to mapped space
```

## Boot Sequence
```
✅ ROM → SSP=$F0800, A6=$F0800
✅ PASCALINIT → %initstdio → returns param block ptr
✅ INITSYS → GETLDMAP → REG_TO_MAPPED
✅ POOL_INIT reached with correct parameters
💥 INIT_FREEPOOL: parameter access codegen bug → pool not initialized
❌ GETSPACE fails → SYSTEM_ERROR(10701)
❌ BOOT_IO_INIT → ProFile handshake
❌ FS_INIT → mount boot volume
❌ ENTER_SCHEDULER → Desktop
```

## Key Files
```
src/lisa_mmu.c              — MMU translation (stack SOR fix)
src/lisa.c                  — MMU pre-programming (STACK_SOR macro)
src/toolchain/pascal_codegen.c — 32-bit multiply, divide, parameter access
src/m68k.c                  — SP delta trace, exception trace
```

## Reference: Lisa MMU Stack Segments (from DO_AN_MMU in LDASM)
```
For stack segments (access type $600):
  SOR = origin_pages + length_pages - $100  (hw_adjust = $100 = 128KB)
For data segments (access type $700):
  SOR = origin_pages
Physical address = SOR * 512 + (logical_addr & $1FFFF)
```
