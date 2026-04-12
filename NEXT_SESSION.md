# LisaEmu — Next Session Handoff (2026-04-12)

## TL;DR

Fixed the stale-upper-word class of codegen bugs (P5): integer literals,
SIZEOF, ORD4, binary ops, and store-width guards. Boot now passes
POOL_INIT → MM_INIT → GETSPACE (succeeds!) → continues into display
setup. Currently **SYSTEM_ERROR(10709)** — "screen MMU is too long"
(`l_scrdata > maxmmusize`). Root cause: GETLDMAP copy loop misaligns
PARMS data into INITSYS's frame.

## Accomplished this session

### P5: Stale-upper-word fixes (`pascal_codegen.c`)

All fixes address the same 68000 pattern: `MOVE.W` only sets `D0[15:0]`,
leaving `D0[31:16]` stale. When the value is used in a 32-bit operation
(ADD.L, SUB.L, CMP.L, MULS), the stale bits corrupt the result.

1. **SIZEOF codegen** (line ~1251): MOVEQ for 0-127, MOVE.L for >127
   (was MOVE.W). Fixes pool header `firstfree` field corruption.

2. **Integer literals** (line ~860): MOVEQ for -128..127, MOVE.L for
   everything else (was MOVE.W for 128-32767). Fixes `b_sysglobal_ptr`
   computation: `ord(@sg_free_pool_addr) + 24575` no longer corrupted
   by stale upper word in the literal.

3. **ORD4 intrinsic** (line ~1192): EXT.L D0 when argument is 16-bit.
   Fixes GETSPACE's `nextfree` computation where `ord4(newsize) * 2`
   was corrupted by the stale upper word from reading newsize (local).

4. **Binary ops** (line ~1054): EXT.L on each operand when use_long=true
   but operand expr_size ≤ 2. General fix for mixed-width arithmetic.

5. **Store-width guards** (lines ~1694, ~1726, ~1788): Only widen stores
   for pointer/longint types. Prevents MOVE.L overwriting adjacent
   fields in records (WITH-block, var-param, complex-LHS paths).

6. **Constant expr_size** (line ~571): Returns 4 for values outside
   -32768..32767 (was always 2). Fixes comparisons involving large
   constants like `maxmmusize = 131072`.

### Verification

- `b_sysglobal_ptr`: was $0198BF67 (wrong), now $00CCBF67 (correct)
- Pool header: was corrupt (`3FFC 00CC 0008 3FFC`), now correct
  (`3FFC 0000 0008 3FFC`) immediately after INIT_FREEPOOL
- GETSPACE: was SYSTEM_ERROR(10701), now succeeds
- MM_INIT: completes, boot progresses to screen setup

## In progress

### SYSTEM_ERROR(10709) — screen MMU too long

**Source**: STARTUP line 431-432:
```pascal
if l_scrdata > maxmmusize then
    SYSTEM_ERROR(stup_cantmapscreen);
```

**Emulator writes**: l_scrdata = $2000 (8192), maxmmusize = $20000 (131072).
8192 < 131072, so the check should NOT trigger. But it does.

**Root cause**: GETLDMAP (STARTUP line 250-257) copies the loader's PARMS
data word-by-word into INITSYS's local frame. The frame layout must
exactly match the PARMS block layout. But the frame layout depends on:

1. INITSYS's own locals (8 vars × 2 bytes = 16 bytes)
2. The `(*$i source/parms.text*)` include variables
3. Nested procedure activation records (GETLDMAP, INIT_PE, etc.)

**Diagnosis approach**: The frame dump at error time shows l_scrdata is
at A6-98. The value there is $000D4514 (garbage, > 131072). This
confirms the PARMS copy is misaligned — the emulator's l_scrdata ($2000)
ended up at a different frame offset than the compiler expects.

**Next steps**:
1. Trace the GETLDMAP copy: dump `@version` address and `@parmend` address
   at runtime to see how big the frame region is
2. Compare against the emulator's PARMS block size
3. Check if nested procedure declarations affect frame layout (they
   shouldn't, but verify)
4. Check if `maxsegments = 48` is properly resolved for the array
   declarations in parms.text (array size mismatches would shift offsets)
5. The most likely issue: the emulator's PARMS writer puts fields at
   fixed offsets, but the codegen allocates frame space differently.
   The fix is to either adjust the PARMS writer to match the codegen's
   frame layout, or vice versa.

## Key file pointers

```
src/toolchain/pascal_codegen.c     P5 fixes (stale-upper-word)
src/lisa.c:~1580                   PARMS writer (emulator)
src/lisa.c:~2811                   SYSTEM_ERROR HLE handler

Lisa_Source/LISA_OS/OS/SOURCE-STARTUP.TEXT.unix.txt:239-258
                                   GETLDMAP — PARMS copy loop
Lisa_Source/LISA_OS/OS/source-parms.text.unix.txt
                                   PARMS variable declarations
```

## Pick up here

> Boot reaches error 10709 (screen MMU). The GETLDMAP copy from the
> emulator's PARMS block into INITSYS's frame is misaligned. Trace the
> copy: (1) add logging in GETLDMAP to show `@version` and `@parmend`
> addresses, (2) compare the frame span against the emulator's PARMS
> span, (3) identify which variable(s) have size mismatches causing
> the alignment shift, (4) fix either the emulator or the codegen.
