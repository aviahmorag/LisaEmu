# LisaEmu — Next Session Handoff (2026-04-12)

## TL;DR

Fixed compilation order (PRIM before consumers) and store-width overflow
(non-pointer assignments no longer use MOVE.L). POOL_INIT now writes to
the sysglobal heap, but pool header values are corrupt → GETSPACE still
fails with SYSTEM_ERROR(10701).

## Accomplished this session

### P4 compile-order fix
- `src/toolchain/toolchain_bridge.c:485-502` — 3-tier compilation sort:
  tier 0 = GLOBAL/DEFS/SYSCALL, tier 1 = PRIM, tier 2 = everything else.
  Previously MM0 compiled before MMPRIM, so types like `sdb_ptr`,
  `mmrb_ptr` were unresolved → all field offsets were 0.
- Same fix in `src/toolchain/audit_toolchain.c:641-658`

### P4 store-width overflow fix
- `src/toolchain/pascal_codegen.c:1757-1762` — the P3 post-hoc widening
  `if (rhs_sz > sz) sz = rhs_sz` now only fires for pointer/longint LHS.
  Previously, `size_sglobal := expr` (int2 = 2 bytes) with a 32-bit
  RHS used MOVE.L, overwriting the adjacent `sg_free_pool_addr` global's
  high word ($00CCA000 → $0000A000).

### P4 global offset reuse
- `src/toolchain/pascal_codegen.c:2181-2202` — before allocating a new
  A5 offset for a global, check `imported_globals` for an existing
  assignment. Prevents double-allocation in the shared A5 data area.

## In progress

### SYSTEM_ERROR(10701) — GETSPACE fails

POOL_INIT now executes and INIT_FREEPOOL writes to the sgheap at $CCA000.
But the pool header values are wrong: `3FFC 00CC 0008 3FFC` instead of
properly computed pool_size, firstfree, freecount.

**Root cause hypothesis**: INIT_FREEPOOL receives correct parameters
(fp_ptr=$CCA000, fp_size=32768) but the codegen for INIT_FREEPOOL
itself may have store-width or type issues. The pool header record
(`hdr_freepool`) has `int2` fields that might be stored as MOVE.L.

**INIT_FREEPOOL source** (source-SYSG1.TEXT, line 32-46):
```pascal
hdr_ptr := pointer(fp_ptr);
with hdr_ptr^ do begin
  pool_size := (fp_size - sizeof(hdr_freepool)) DIV 2;
  firstfree := sizeof(hdr_freepool);
  freecount := pool_size;
  ent_ptr := pointer(fp_ptr + firstfree);
  with ent_ptr^ do begin
    size := pool_size;
    next := stopper;
  end;
end;
```

**Next steps**:
1. Check `hdr_freepool` record layout — what are the field types and
   offsets for pool_size, firstfree, freecount?
2. Disassemble INIT_FREEPOOL at $5834 to verify field access offsets
3. The `3FFC 00CC 0008 3FFC` pattern in the pool header suggests fields
   are written at wrong offsets or with wrong widths
4. The store-width fix may need to also apply to WITH-statement field
   assignments (gen_with_base path), not just simple identifier assignments

**Also check**: the `stopper` constant used in INIT_FREEPOOL — if it's
defined in SYSGLOBAL, does the codegen resolve it correctly?

## Key file pointers

```
src/toolchain/pascal_codegen.c     P4 fixes (store-width, global reuse)
src/toolchain/toolchain_bridge.c   P4 compile-order fix
src/toolchain/audit_toolchain.c    P4 compile-order fix (audit tool)
src/lisa.c:~2800                   SYSTEM_ERROR HLE handler

Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt:5-46
                                   INIT_FREEPOOL — current issue
Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt:729-772
                                   POOL_INIT — calls INIT_FREEPOOL
Lisa_Source/LISA_OS/OS/source-SYSGLOBAL.TEXT.unix.txt
                                   hdr_freepool, ent_freepool types
```

## Pick up here

> Source boot reaches POOL_INIT which calls INIT_FREEPOOL. The pool
> header at $CCA000 now has data (not zeros) but the values are corrupt.
> Disassemble INIT_FREEPOOL ($5834) and check: (1) are hdr_freepool
> field offsets correct? (2) are the WITH-statement field stores using
> the right widths? (3) does `sizeof(hdr_freepool)` resolve to the
> correct value? The store-width fix may need extending to cover
> WITH-block field assignments.
