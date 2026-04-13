# LisaEmu — Next Session Handoff (2026-04-13, session 3)

## TL;DR

Fixed 6 codegen bugs (P6-P9) + memory layout issues. Boot now passes
POOL_INIT, INIT_FREEPOOL, MM_INIT's first GETSPACE (mmrb allocation).
Currently **SYSTEM_ERROR(10701)** from MM_INIT's second GETSPACE call
(SIZEOF(mrbt) allocation at line 290). The boolean NOT fix (P9)
resolved the most critical issue: `NOT.W TRUE = $FFFE (non-zero)`,
making ALL `if not func(...)` patterns fail.

## Accomplished this session

### P6: Boolean size + const expr_size (`pascal_codegen.c`)
- Boolean: 1→2 bytes (Lisa Pascal word-sized)
- expr_size: check is_const BEFORE type for large constants

### P7: Interface symbol suppression (`pascal_codegen.c`)
- UNIT INTERFACE proc/func declarations no longer export linker
  symbols at offset 0 (was shadowing real IMPLEMENTATION addresses)

### P8: Local consts + function EXT.L (`pascal_codegen.c`)
- Procedure-local CONST declarations now processed in gen_proc_or_func
- Binary op EXT.L skipped for AST_FUNC_CALL operands

### P9: Boolean NOT + page-aligned sgheap (`pascal_codegen.c`, `lisa.c`)
- `NOT.W D0` (bitwise) → `TST/SEQ/ANDI` (logical) for boolean operands
- l_sgheap: $8000→$7E00 (page-aligned, fits int2)

### Memory layout (`lisa.c`)
- himem = b_dbscreen, bothimem = lomem (from earlier)

## In progress

### SYSTEM_ERROR(10701) — second GETSPACE in MM_INIT

MM_INIT line 290:
```pascal
if not GETSPACE(SIZEOF(mrbt), b_sysglobal_ptr, s_mrbt_addr) then
    SYSTEM_ERROR(nospace);
```

The first GETSPACE (SIZEOF(mmrb), line 211) now succeeds.
The pool has ~15977 words free after the first allocation.

**Possible causes for the second failure:**
1. SIZEOF(mrbt) returns wrong value — if too large, GETSPACE
   rejects with `amount > 32764` check
2. The pool header is corrupt after the first allocation
3. The second `not GETSPACE(...)` pattern isn't caught by the
   boolean NOT heuristic (but AST_FUNC_CALL should match)
4. GETSPACE internally calls another function that returns boolean
   and hits the same NOT bug (nested NOT pattern)
5. Parameter passing mismatch: SIZEOF returns int2, GETSPACE
   expects int2, but codegen pushes 4 bytes

**Next steps:**
1. Add trace at the GETSPACE function entry (dynamic address —
   find via linker debug output) to see the `amount` parameter
2. If amount is reasonable, trace through GETSPACE's search loop
3. Check if EXP_POOLSPACE is called and whether it fails
4. Check if there are nested boolean NOT patterns in the pool
   expansion code

## Key files

```
src/toolchain/pascal_codegen.c:1049   boolean NOT fix (P9)
src/toolchain/pascal_codegen.c:150    boolean size (P6)
src/toolchain/pascal_codegen.c:570    const expr_size (P6)
src/toolchain/pascal_codegen.c:2362   interface symbol suppression (P7)
src/toolchain/pascal_codegen.c:2461   procedure-local consts (P8)
src/toolchain/pascal_codegen.c:1064   function call EXT.L skip (P8)
src/lisa.c:1628-1633                  himem/bothimem/sgheap fixes

Lisa_Source/LISA_OS/OS/source-MM4.TEXT.unix.txt:193    MM_INIT
Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt:158  GETSPACE
Lisa_Source/LISA_OS/OS/source-SYSG1.TEXT.unix.txt:5    INIT_FREEPOOL
```

## Pick up here

> Boot passes first GETSPACE in MM_INIT but fails on second (SIZEOF(mrbt)).
> Trace GETSPACE entry to see the amount parameter and pool state.
> The boolean NOT fix is the biggest single improvement — it affects
> every `if not func(...)` pattern throughout the OS. Check if nested
> boolean patterns (NOT inside function results) are also handled.
