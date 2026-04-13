# LisaEmu — Next Session Handoff (2026-04-13, session 4)

## TL;DR

Fixed 10 codegen bugs (P6-P11) + memory layout fixes across 3 sessions.
Boot passes POOL_INIT, INIT_FREEPOOL, and the search loop in GETSPACE
correctly finds free space. But `getspace := true` in the "found"
branch is never reached at runtime. The allocation code in the nested
WITH blocks (with c_pool_ptr^ do / with currfree^ do) likely has
another codegen issue.

## Accomplished today

### Codegen fixes (pascal_codegen.c)
| Fix | Bug | Impact |
|-----|-----|--------|
| P6 | Boolean 1→2 bytes | PARMS frame misaligned 48+ bytes |
| P6 | expr_size: is_const before type | Large consts ($20000) zeroed by EXT.L |
| P7 | Interface symbol suppression | GETFREE etc. at wrong address |
| P8 | Procedure-local consts | nospace=610 instead of 10701 |
| P8 | No EXT.L on func call results | MMU_BASE $CC0000→$0 |
| P9 | Boolean NOT (bitwise→logical) | ALL `if not func()` always TRUE |
| P10 | Function result local + D0 load | Functions never returned results |
| P11 | Nested binary ops: D2→stack save | Compound conditions always TRUE |

### Memory layout (lisa.c)
- himem = b_dbscreen, bothimem = lomem, l_sgheap = $7E00

## In progress

### GETSPACE "found" branch never executes `getspace := true`

**Verified working:**
- Pool at $CCA000: pool_size=16124, firstfree=8, freecount=16124 ✓
- GETSPACE amount=292, b_area=$CCBF65, adjust works ✓
- `(amount > 32764) or (amount <= 0)` → FALSE (D2 fix works!) ✓
- Search loop: currfree=$CCA008, currfree^.size=16124 ≥ newsize=147 ✓
- Loop doesn't execute (first entry big enough) ✓
- currfree ≠ c_pool_ptr → "found" branch decision ✓
- Assignment codegen for `getspace := true`: correctly targets A6-32 ✓

**What fails:**
The function result at A6-32 is never written to 1 (TRUE). The
memory watchpoint shows only one write (initialization to 0 by LINK).
The `getspace := true` instruction at line 292 is never reached.

**Root cause hypothesis:**
The "found" branch has complex code: nested WITH blocks, pointer
arithmetic (`ord4(newsize)*2`), field assignments via WITH, and
`INTSON` call. One of these likely has a codegen bug that causes
an early exit or exception:

1. **`ord4(newsize)*2` in `pointer(ord(currfree)+ord4(newsize)*2)`:**
   The `*2` is a binary op. `ord4(newsize)` is a function call (intrinsic).
   The D2 stack-save fix should handle this, but verify.

2. **`leftfree^.next := ord(nextfree) - ord(c_pool_ptr)`:**
   Multiple pointer dereferences and WITH field accesses. Might
   generate a bus error on misaligned addresses.

3. **`size := -newsize` inside WITH currfree^:**
   This stores to currfree^.size. The WITH base is currfree.
   If the codegen writes to the wrong offset, it could corrupt
   the pool structure.

4. **INTSON call with wrong stack cleanup:** If INTSON is callee-clean
   but doesn't pop its params, SP is misaligned and subsequent code
   breaks.

**Next steps:**
1. Add a PC trace in the "found" branch to find where execution
   stops. Decode the code from the BEQ (if/else decision) forward
   to locate the exact failing instruction.
2. Check for bus errors / address errors during GETSPACE execution.
3. Look at the `ord4(newsize)*2` multiplication — on 68000, if
   newsize is in D0 as a word, `ord4` should EXT.L, then `*2`
   can be ADD.L D0,D0. But codegen might generate a full 32-bit
   multiply.

## Key files

```
src/toolchain/pascal_codegen.c:1091-1118  D2 stack-save fix (P11)
src/toolchain/pascal_codegen.c:1049       Boolean NOT fix (P9)
src/toolchain/pascal_codegen.c:2572-2588  Function result load (P10)
src/lisa.c:1608-1635                      Memory layout

Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt:158   GETSPACE
Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt:256   "found" branch
Lisa_Source/LISA_OS/OS/source-SYSGLOBAL.TEXT.unix.txt:540  hdr_freepool record
```
