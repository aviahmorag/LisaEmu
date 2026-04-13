# LisaEmu — Next Session Handoff (2026-04-13, final)

## TL;DR

Fixed 10 codegen bugs (P6-P11) + memory layout. Boot passes POOL_INIT,
INIT_FREEPOOL. GETSPACE correctly enters "found" branch (currfree ≠
c_pool_ptr) but c_pool_ptr has wrong value: $CCA008 (pool_base+8)
instead of $CCA000 (pool_base). Root cause: A5-relative offset mismatch
for `sg_free_pool_addr` or `b_sysglobal_ptr` between compilation units.

## Key finding from this session's debugging

**GETSPACE c_pool_ptr is $CCA008 instead of $CCA000.**

The trace shows:
```
PC=$5E6E: MOVE.L -10(A6),D0  → D0=$CCA008 (c_pool_ptr)
```

$CCA008 = pool_base + sizeof(hdr_freepool) = first free entry address.
The correct value should be $CCA000 (pool base).

**How c_pool_ptr is derived:**
```pascal
x := pointer(b_area);      → x = pointer of sg_free_pool_addr address
c_pool_ptr := pointer(x^); → c_pool_ptr = VALUE of sg_free_pool_addr
```

b_area arrives as `b_sysglobal_ptr` = `ord(@sg_free_pool_addr) + 24575`.
After adjustment: `b_area - 24575` = `@sg_free_pool_addr` = A5-150.

If `sg_free_pool_addr` at A5-150 contains $CCA000, then `x^` = $CCA000.
But the actual read gives $CCA008.

**Possible causes:**
1. `sg_free_pool_addr` itself was modified after POOL_INIT wrote $CCA000
   (e.g., by `size_sglobal` store at an overlapping A5 offset)
2. The A5 offset for sg_free_pool_addr differs between the POOL_INIT
   store and the GETSPACE read (global offset counter desync)
3. The `b_sysglobal_ptr` computation has a 2-byte error, causing `x`
   to point to A5-148 instead of A5-150 — reading the bytes 2 positions
   higher gives $CCA008 instead of $CCA000 if there's an $0008 prefix

**Most likely:** Cause #3 — the `@sg_free_pool_addr + 24575` computation
uses `ord(@sg_free_pool_addr)` which should give A5-150. But if the
sg_free_pool_addr offset differs by 2 between POOL_INIT (which writes)
and MM_INIT (which passes b_sysglobal_ptr), the pointer is off by 2.

**Next steps:**
1. Verify b_sysglobal_ptr value at GETSPACE entry: should be
   `A5 - 150 + 24575`. If it's `A5 - 148 + 24575`, the offset is wrong.
2. Check if different compilation units assign different A5 offsets
   for `sg_free_pool_addr` and `b_sysglobal_ptr`.
3. Dump 8 bytes at the adjusted b_area to see what values are there.
4. If the offset is misaligned: check the `imported_globals` mechanism
   to see if A5 offsets are properly synchronized across units.

## Codegen fixes this session (P6-P11)

| Fix | Bug | Impact |
|-----|-----|--------|
| P6 | Boolean 1→2 bytes | PARMS frame misaligned |
| P6 | is_const before type in expr_size | Large const EXT.L corruption |
| P7 | Interface symbol suppression | Wrong function addresses |
| P8 | Procedure-local consts | Wrong error constants |
| P8 | No EXT.L on func call results | Function return values zeroed |
| P9 | Boolean NOT (bitwise→logical) | All `if not f()` always TRUE |
| P10 | Function result local + D0 load | Functions never returned results |
| P11 | D2 → stack save for nested binops | Compound conditions corrupted |

## Key files

```
src/toolchain/pascal_codegen.c     All P6-P11 codegen fixes
src/lisa.c                         Memory layout fixes
Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt   GETSPACE, POOL_INIT
Lisa_Source/LISA_OS/OS/source-SYSGLOBAL.TEXT.unix.txt  hdr_freepool, globals
```
