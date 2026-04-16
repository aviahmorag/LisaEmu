# LisaEmu — Next Session Handoff (2026-04-16 session 2)

## Accomplished this session

### P80: 8-char significant identifiers (structural codegen fix)
`str_eq_nocase` now returns true after 8 matching characters. Resolved
whole class of variable mismatches (e.g., `ordrefncbptr` vs `ordrefncb`).

### P80a: FS bypass removal
DecompPath/parse_pathname HLE bypasses disabled — FS functions run natively.

### P80b: iterative pre-pass record fixup (27 records corrected)
After the types-only pre-pass, re-resolve NULL field types and recompute
record layouts until stable. Fixed 27 records including MMRB, PCB, SDB.
This eliminated the Signal_sem bogus, VEC-GUARD from e_badrefnum, and
UNMAPPED-WRITE crashes that plagued SYS_PROC_INIT.

### P80c: diagnosed freepool field offset corruption
The full-pass compilation creates a LOCAL hdr_freepool type with ALL
field offsets = 0. The type is created with correct offsets (@2 firstfree)
but they get zeroed between creation and the WITH-block lookup.

## Current blocker: hdr_freepool field offset corruption

**Symptom**: SYSTEM_ERROR(10701) "no space in sysglobal" at MM_INIT+$3E.
**Root cause**: INIT_FREEPOOL writes firstfree (int4, 4 bytes) to offset 0
of the pool header, overwriting pool_size. Result: pool_size=0, so
GETSPACE fails on the very first allocation.

**Why**: The full-pass local hdr_freepool type has all fields at offset 0:
```
field[0] 'pool_size' @0 sz=2 kind=1
field[1] 'firstfree' @0 sz=4 kind=2  ← should be @2!
field[2] 'freecount' @0 sz=2 kind=1  ← should be @6!
```

The type is CREATED with correct offsets (RECORD-LAYOUT trace confirms @2),
but by the time the WITH block looks it up, all offsets are 0.

**Hypotheses**:
1. Memory corruption: `shared_types[8192]` is ~45MB of static data.
   The full-pass calloc'd `cg` (another ~22MB) may overlap or the
   static array may overflow into heap.
2. The `shared_types` fixup accidentally modifies the local type
   through aliased pointers.
3. A struct copy somewhere zeros the offset fields.

**Next steps**:
1. **Check for memory overlap**: print addresses of shared_types array
   boundaries vs cg heap allocation to verify no overlap.
2. **Add write watchpoint**: use mprotect or a sentinel value to detect
   when the field offsets change from 2 to 0.
3. **Alternative fix**: instead of fixing the corruption, make the
   full-pass use the IMPORTED (pre-pass) type for WITH lookups.
   Modify `with_lookup_field` to prefer imported types when the
   local type has suspicious all-zero offsets.

## Quick reference
- Build: `make`
- Run: `rm -f build/lisa_profile.image && ./build/lisaemu --headless Lisa_Source 5000`
- Commit: `cb970a3` (clean build, 10701 error at MM_INIT)
- Previous working state (with P35 bypass): commit `dfe8e68`
