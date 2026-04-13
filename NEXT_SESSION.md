# LisaEmu — Next Session Handoff (2026-04-13)

## TL;DR

Fixed two P6 codegen bugs (boolean size, const expr_size) and two
memory layout issues (himem, bothimem). Boot now passes GETLDMAP,
screen MMU setup, AVAIL_INIT, reaches INIT_CONFIG → GET_BOOTSPACE.
Currently **SYSTEM_ERROR(10701)** — `GETFREE` fails (no free pages).
Root cause under investigation: free pool may not be populated by
MAKE_FREE, or the pool data structures are corrupt.

## Accomplished this session

### P6: Boolean size fix (`pascal_codegen.c`)

**Line 150**: `boolean` type size changed from 1 to 2 bytes.

Lisa Pascal stores boolean variables as words (2 bytes) on the 68000.
The 1-byte size caused PARMS frame misalignment: the boolean array
`swappedin[1..48]` was 48 bytes instead of 96, shifting all
subsequent PARMS variables by 48+ bytes. This caused GETLDMAP's
word-by-word copy to misalign the data in INITSYS's frame.

### P6: Const expr_size ordering fix (`pascal_codegen.c`)

**Line ~570**: Swapped the `is_const` check to come BEFORE `sym->type`.

All constants are created with `type = integer` (size 2). The old code
checked `sym->type` first, returning `type_load_size(integer) = 2` for
ALL constants regardless of value. This meant `maxmmusize = 131072`
had expr_size = 2, causing a spurious `EXT.L D0` after `MOVE.L #$20000,D0`
which zeroed the upper word ($20000 → $0). The comparison
`l_scrdata > maxmmusize` then evaluated `$2000 > $0 = true`.

### Memory layout fixes (`lisa.c`)

1. **himem**: Changed from hardcoded `$0FF800` (1MB boundary) to
   `b_dbscreen` ($230000, below screen buffers). The OS binary at
   ~896KB left zero free space with the old 1MB boundary.

2. **bothimem**: Changed from `himem` to `lomem`. bothimem guards against
   overwriting the loader during allocation. Since we have no resident
   loader, setting it equal to himem blocked ALL allocations.

### Verification

- Error 10709 (screen MMU) is FIXED — boot passes the `l_scrdata > maxmmusize` check
- PARMS data correctly copied: l_scrdata = $2000 via static link through MMU
- Free memory: lomem=$FF800, himem=$230000 (~1.2MB free)
- Boot progresses to INIT_CONFIG → GET_BOOTSPACE → GETFREE

## In progress

### SYSTEM_ERROR(10701) — GETFREE fails

**Source**: GET_BOOTSPACE (STARTUP line 1171):
```pascal
if not GETFREE( (size + mempgsize - 1) div mempgsize, page_got) then
   SYSTEM_ERROR(10701);
```

**GETFREE** (source-MM4.TEXT): Gets the last free area from the free
memory list (`c_mmrb^.tail_sdb.freechain.bkwd_link`). Returns false
if `f_sdb^.memsize < size`.

**MAKE_FREE** (source-MM0.TEXT): Called by AVAIL_INIT to register
free pages. Creates an SDB at `ord4(maddr) * hmempgsize * 2 + logrealmem`
and inserts it into the free chain via INSERTSDB.

**Diagnostic data**:
- PARMS lomem/himem correct: $FF800 / $230000
- sg_free_pool_addr (A5-148) = $A0000000 — looks suspicious
- GETFREE at $A101E, MAKE_FREE in MMPRIM

**Possible causes**:
1. MAKE_FREE never called (AVAIL_INIT code path broken)
2. MAKE_FREE called but INSERTSDB doesn't work (free chain not linked)
3. BLDPGLEN computation wrong (freebase or freelen = 0)
4. The MMRB (memory manager resource block) not initialized properly
5. `mmrb_addr` global has wrong value

**Next steps**:
1. Add a trace at MAKE_FREE's address to verify it's called and
   inspect its parameters (maddr, msize)
2. After MAKE_FREE returns, dump the free chain to see if any
   free SDBs were created
3. Check if AVAIL_INIT actually computes freebase/freelen from
   lomem/himem correctly (these are accessed via static link)
4. Check if the `const expr_size` fix affected any MMPRIM computations
   (e.g., hmempgsize = 256 fits in 16 bits, so should be fine)

## Key file pointers

```
src/toolchain/pascal_codegen.c:150   boolean size fix
src/toolchain/pascal_codegen.c:570   const expr_size fix
src/lisa.c:1628-1631                 himem/bothimem fix
src/lisa.c:2835-2860                 SYSTEM_ERROR HLE handler

Lisa_Source/LISA_OS/OS/source-MM0.TEXT.unix.txt     MAKE_FREE
Lisa_Source/LISA_OS/OS/source-MM4.TEXT.unix.txt      GETFREE, BLDPGLEN
Lisa_Source/LISA_OS/OS/SOURCE-STARTUP.TEXT.unix.txt:309  AVAIL_INIT
Lisa_Source/LISA_OS/OS/SOURCE-STARTUP.TEXT.unix.txt:1158 GET_BOOTSPACE
Lisa_Source/LISA_OS/OS/SOURCE-STARTUP.TEXT.unix.txt:1103 INIT_CONFIG
```

## Pick up here

> Boot reaches error 10701 (GETFREE fails). MAKE_FREE should have
> populated the free pool from AVAIL_INIT with pages from $FF800
> to $230000 (~1.2MB). Trace MAKE_FREE execution: (1) add PC trace
> at MAKE_FREE's linked address, (2) check that freebase/freelen
> are non-zero when MAKE_FREE is called, (3) dump the free chain
> after MAKE_FREE returns, (4) check MMRB initialization.
