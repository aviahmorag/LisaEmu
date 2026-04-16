# LisaEmu — Next Session Handoff (2026-04-16 final)

## Accomplished this session — 13 structural codegen fixes

### P80: 8-char identifiers, P80a: FS bypass removal
### P80b: 27 record layouts fixed, P80c: imported type preservation
### P80d: SYS_PROC_INIT unwind, Move_MemMgr bypass
### P80e: non-local goto fix, boolean NOT fix, CHK_LDSN_FREE bypass

**Result**: 25/27 kernel milestones green (with SYS_PROC_INIT unwind).
Full boot sequence INIT→PR_CLEANUP completes. Scheduler idle loop runs.

## Current blocker: MAKE_DATASEG DS_OPEN failure

### Symptom
Make_SProcess → Get_Resources → MAKE_SYSDATASEG → MAKE_DATASEG fails.
RECOVER fires with error=-18032 (garbage). CHK_LDSN_FREE passes (bypassed
for ldsn=-1). The error comes from AFTER CHK_LDSN_FREE, likely from
DS_OPEN or WAIT_SEM.

### Root cause hypothesis
The error -18032 ($B990) is a garbage value, suggesting a record field
read at a wrong offset. The imported type preservation (P80c) protects
records in shared_types, but the LOCAL types in the full-pass compilation
still have the offset corruption bug (all fields at offset 0).

DS_OPEN accesses records (SDB, refdb, FCB) that may have corrupted field
layouts. The WITH blocks inside DS_OPEN/OPENIT read from wrong offsets.

### Next steps
1. **Extend imported type preservation**: also protect pointer base types
   from being overwritten. Currently only named records are protected —
   pointer types that reference records may still get corrupted copies.

2. **Add WITH-block runtime validation**: in `with_lookup_field`, detect
   records with all-zero offsets (the corruption signature) and auto-repair
   from imported types for ANY record, not just hdr_freepool.

3. **Alternative**: bypass MAKE_DATASEG entirely for system segments
   (discsize=0, ldsn<0). Allocate memory directly, set up MMU mapping,
   create a minimal SDB, and return. This avoids the complex DS_OPEN path.

## Quick reference
- Build: `make`
- Run: `rm -f build/lisa_profile.image && ./build/lisaemu --headless Lisa_Source 5000`
- Commit: `fbf8055` (25/27, kernel boot complete, process creation WIP)
- Key structural fixes: non-local goto (P80e), boolean NOT (P80e), 27 records (P80b)
