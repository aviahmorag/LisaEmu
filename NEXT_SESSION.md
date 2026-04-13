# LisaEmu — Next Session Handoff (2026-04-13, final)

## TL;DR

Fixed 8 codegen bugs (P6-P10) + 3 memory layout issues this session.
Boot now passes POOL_INIT + INIT_FREEPOOL (pool correctly created at
$CCA000 with 16124 words). Still **SYSTEM_ERROR(10701)** — GETSPACE
returns false despite valid pool. The search loop doesn't find free
space. Root cause: likely a `WITH c_pool_ptr^` field offset mismatch
in GETSPACE (the `hdr_freepool` record fields may have wrong offsets
in the codegen, causing `firstfree` to read the wrong word from the
pool header).

## Accomplished this session

### Codegen fixes (pascal_codegen.c)
| Fix | Bug | Impact |
|-----|-----|--------|
| P6 | Boolean 1→2 bytes | PARMS frame misaligned 48+ bytes |
| P6 | expr_size: is_const before type | Large consts ($20000) zeroed |
| P7 | Interface symbol suppression | GETFREE etc. at wrong address |
| P8 | Procedure-local consts | nospace=610 instead of 10701 |
| P8 | No EXT.L on func call results | MMU_BASE $CC0000→$0 |
| P9 | Boolean NOT (bitwise→logical) | ALL `if not func()` always TRUE |
| P10 | Function result local + D0 load | Functions never returned results |

### Memory layout fixes (lisa.c)
- himem = b_dbscreen (below screen buffers, ~$230000)
- bothimem = lomem (no loader to protect)
- l_sgheap = $7E00 (page-aligned, fits int2)

## In progress

### GETSPACE returns false despite valid pool

**Verified correct:**
- INIT_FREEPOOL receives fp_ptr=$CCA000, fp_size=$7E00 ✓
- Pool header: pool_size=16124, firstfree=8, freecount=16124 ✓
- First free entry: size=16124, next=0 (stopper) ✓
- GETSPACE params: amount=292, b_area=$CCBF65 ✓
- b_sysglobal_ptr comparison: b_area = b_sysglobal_ptr → adjust ✓
- c_pool_ptr via x^ = $CCA000 ✓
- Function result at A6-32, loaded into D0 before RTS ✓

**What fails:**
D0 = $CC0000 on return (low word $0000 = false). The function
result local was never set to TRUE, meaning the search loop's
"free space found" branch never executed.

**Root cause hypothesis:**
The search loop uses `WITH c_pool_ptr^ DO ... currfree := pointer(
ord(c_pool_ptr) + firstfree)`. The `firstfree` field access through
WITH uses the `hdr_freepool` record's field offsets. If the codegen
doesn't know the correct field layout (field offsets imported from
SYSGLOBAL), it reads the wrong word.

Pool header: `3EFC 0000 0008 3EFC` (pool_size, ???, firstfree, freecount)
If firstfree offset is wrong (e.g., offset 0 instead of 4), the code
reads $3EFC (pool_size) as firstfree. Then currfree = $CCA000 + $3EFC
= $CCE3FC — pointing beyond the pool. The loop test
`ord(currfree) <> ord(c_pool_ptr)` would be TRUE, and `currfree^.size`
at $CCE3FC would read garbage < newsize → loop continues until
currfree wraps to c_pool_ptr → "no free space".

**Next steps:**
1. Dump the WITH block's base pointer and field offsets for
   hdr_freepool in GETSPACE's compiled code
2. Compare against the actual pool header layout
3. Fix the record field offset resolution for imported types
4. Alternatively: check if the `stopper` constant (used for the
   free list terminator) has the right value — if stopper = 0
   and next = 0, the loop might terminate correctly

## Key file pointers

```
src/toolchain/pascal_codegen.c   All codegen fixes (P6-P10)
src/lisa.c:1608-1635             Memory layout fixes

Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt:158  GETSPACE
Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt:5    INIT_FREEPOOL
Lisa_Source/LISA_OS/OS/source-SYSGLOBAL.TEXT.unix.txt  hdr_freepool type def
Lisa_Source/LISA_OS/OS/source-MM4.TEXT.unix.txt:193    MM_INIT
```

## Pick up here

> GETSPACE search loop doesn't find free space. Dump the WITH block
> field access code for c_pool_ptr^ in GETSPACE to check if firstfree
> is read from the correct offset (should be +4 based on the pool
> header: word 0=pool_size, word 1=???, word 2=firstfree). If the
> offset is wrong, fix the hdr_freepool record type resolution.
