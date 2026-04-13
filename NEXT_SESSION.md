# LisaEmu — Next Session Handoff (2026-04-13, session 2)

## TL;DR

Fixed 4 codegen bugs (P6-P8) + 2 memory layout issues. Boot now reaches
POOL_INIT with correct parameters (mb_sgheap=$CCA000) but GETSPACE in
MM_INIT still fails (error 10701). INIT_FREEPOOL's setup of the sysglobal
free pool is suspect — the pool data structures may be corrupt or the
pool header size computation is wrong.

## Accomplished this session

### P6: Boolean size + const expr_size (pascal_codegen.c)
- Boolean: 1→2 bytes (Lisa Pascal word-sized)
- expr_size: check is_const BEFORE type for large constants

### P7: Interface symbol suppression (pascal_codegen.c)
- UNIT INTERFACE proc/func declarations no longer export linker symbols
  at offset 0. Previously GETFREE, BLDPGLEN, MAKE_REGION all jumped to
  the module base instead of their real code.

### P8: Local consts + function EXT.L (pascal_codegen.c)
- Procedure-local CONST declarations (nospace=10701 in MM_INIT) now
  processed in gen_proc_or_func
- Binary op EXT.L skipped for AST_FUNC_CALL operands — functions
  return correct 32-bit values, EXT.L was zeroing MMU_BASE's $CC0000

### Memory layout (lisa.c)
- himem = b_dbscreen (below screen buffers, ~$230000)
- bothimem = lomem (no loader to protect)

## In progress

### SYSTEM_ERROR(10701) — GETSPACE fails in MM_INIT

POOL_INIT receives correct parameters (verified):
- mb_sysglob = $CC0000 (MMU_BASE(102))
- l_sysglob = $6000 (24KB)
- mb_sgheap = $CCA000 (correctly computed now)
- l_sgheap = $8000 (32KB)
- mb_syslocal = $CE0000 (MMU_BASE(103))
- l_syslocal = $4000

But GETSPACE(SIZEOF(mmrb), b_sysglobal_ptr, mmrb_addr) fails.

**Key finding**: The codegen places `sg_free_pool_addr` at A5-150
(LEA -$96(A5)) but the earlier diagnostic dump read A5-148. The
hardcoded diagnostic offset was wrong. Need to verify the actual
value at A5-150 after POOL_INIT runs.

**Possible causes**:
1. INIT_FREEPOOL (at $5914) code has bugs — it sets up the free
   pool header at the mapped sgheap address ($CCA000). Pool header
   has `size` (negative for free), `pool_size`, etc. If the header
   is corrupt, GETSPACE can't find free space.
2. A5-relative offset mismatch: POOL_INIT stores to A5-150 but
   GETSPACE reads from a different offset (different compilation
   unit may have different global_offset counter state).
3. The free pool write goes to unmapped/wrong physical memory.
   Segment 102 (sysglobmmu) maps $CC0000→physical $DD800. The
   sgheap at $CCA000 maps to physical $DD800 + $A000 = $E7800.
   This IS within physical RAM.

**Next steps**:
1. Add a trace at INIT_FREEPOOL ($5914) to dump its parameters
   and verify the pool header after initialization
2. After POOL_INIT returns, read the pool header at the ACTUAL
   A5-150 mapped address and verify it contains valid data
3. Check if GETSPACE reads sg_free_pool_addr from the correct
   A5 offset (should match the one POOL_INIT writes to)
4. If the offsets don't match, investigate the global_offset
   counter consistency between compilation units

## Key file pointers

```
src/toolchain/pascal_codegen.c:150   boolean size (P6)
src/toolchain/pascal_codegen.c:570   const expr_size ordering (P6)
src/toolchain/pascal_codegen.c:2362  interface symbol suppression (P7)
src/toolchain/pascal_codegen.c:2461  procedure-local consts (P8)
src/toolchain/pascal_codegen.c:1064  function call EXT.L skip (P8)
src/lisa.c:1628-1633                 himem/bothimem fix

Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt:729  POOL_INIT
Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt       INIT_FREEPOOL, GETSPACE
Lisa_Source/LISA_OS/OS/source-MM4.TEXT.unix.txt:193     MM_INIT (calls GETSPACE)
Lisa_Source/LISA_OS/OS/source-MMPRIM.TEXT.unix.txt:773  MMU_BASE
```

## Pick up here

> GETSPACE fails in MM_INIT despite POOL_INIT receiving correct
> parameters. The sysglobal free pool (at $CCA000, 32KB) should be
> initialized by INIT_FREEPOOL. Trace INIT_FREEPOOL to verify the
> pool header is set up correctly. Then verify GETSPACE reads
> sg_free_pool_addr from the same A5 offset that POOL_INIT wrote to.
