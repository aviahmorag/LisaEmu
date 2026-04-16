# LisaEmu — Next Session Handoff (2026-04-16 session 2, updated)

## Accomplished this session

### P80: 8-char significant identifiers
`str_eq_nocase` now matches after 8 characters, like the original Lisa Pascal compiler.

### P80a: FS bypass removal
DecompPath/parse_pathname run natively (no HLE bypass needed with corrected codegen).

### P80b: iterative pre-pass record fixup (27 records)
After types-only pre-pass, re-resolve NULL field types and recompute layouts
until stable. Fixed MMRB, PCB, SDB and 24 other kernel records.

### P80c: imported type preservation + pool repair
The full-pass type-decl `*existing = *t` struct copy corrupted record field
offsets to 0. Fix: skip the copy when the imported type already has valid
offsets. Also added INIT_FREEPOOL HLE as safety net.

**Result**: SYS_PROC_INIT runs real process creation code without crashes.
GETSPACE allocates from both sysglobal and syslocal pools. No VEC-GUARD
writes, no Signal_sem corruption, no UNMAPPED-WRITEs.

## Current state — 21/27 milestones, clean 5000 frames

All kernel init through SYS_PROC_INIT succeeds. The system enters the
scheduler idle loop. Missing milestones: INIT_DRIVER_SPACE, FS_CLEANUP,
MEM_CLEANUP, PR_CLEANUP.

## Next priorities

### 1. Investigate why INIT_DRIVER_SPACE doesn't fire
INIT_DRIVER_SPACE ($01BDD2) should be called after SYS_PROC_INIT.
Check if the boot flow reaches it or if SYS_PROC_INIT is still
incomplete (e.g., Make_SProcess for Root fails silently).

### 2. Check if processes are actually created
Add diagnostics to verify the PCBs for MemMgr and Root are valid
and queued in the scheduler's ready queue. The scheduler should
dispatch them after PR_CLEANUP's Enter_Scheduler.

### 3. Investigate scheduler dispatch
If processes ARE created, the scheduler should dispatch Root (which
calls CreateShell → Make_Process('SYSTEM.SHELL')). This is where
the multi-target build pipeline is needed.

## Quick reference
- Build: `make`
- Run: `rm -f build/lisa_profile.image && ./build/lisaemu --headless Lisa_Source 5000`
- Commit: `a9188f8` (21/27, clean, no crashes)
