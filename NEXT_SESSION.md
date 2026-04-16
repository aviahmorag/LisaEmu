# LisaEmu — Next Session Handoff (2026-04-16)

## Session: 20+ codegen fixes, BOTH processes created

### Key discoveries:
1. **8-char identifiers**, **27 record layouts**, **non-local goto/exit A6 restore**
2. **Boolean NOT for functions**, **enum/const priority**, **record repair generalization**
3. **MAKE_SYSDATASEG stack pop fix** (double-pop corrupted second call)
4. **exit(proc) non-local unwind** (same as goto — follows static link chain)
5. **Root cause identified**: *existing = *t struct copy creates dangling type pointers

### Result: 25/27 milestones, both processes created via HLE chain:
- MAKE_SYSDATASEG: 4 segments allocated (syslocal+stack × 2 processes)
- CreateProcess/ModifyProcess/FinishCreate: bypassed (6 calls total)
- Move_MemMgr: bypassed
- Scheduler runs in idle loop (Pause instruction)

## Current state: processes not dispatchable

Both MemMgr and Root are "created" (memory allocated) but PCBs are
uninitialized. The scheduler scans fwd_ReadyQ which is empty.

## Next priorities

### 1. HLE PCB initialization + Queue_Process
Minimum to make processes dispatchable:
- Set PCB.priority (250 MemMgr, 230 Root)
- Set PCB.blk_state = 0 (empty set = ready)
- Set PCB.proctype = sys
- Set up syslocal env_save_area (A5=sysA5, PC=start_PC, SR=0, etc.)
- Insert PCB into fwd_ReadyQ

The PCB address is from GETSPACE #41/#42. The syslocal/stack addresses
are from MAKE_SYSDATASEG HLE ($CCC000, $CCCC00, $CCDE00, $CCEA00).

### 2. Fix root cause: type pointer dangling
The *existing = *t struct copy in type-decl processing copies type
pointers from cg->types (freed after compilation) into shared_types.
These become dangling pointers when cg is reused. The proper fix:
after the copy, remap ALL internal type pointers (field types, base
types) from cg->types to shared_types, like the REMAP_TYPE_PTR logic
in the pre-pass export code. This would eliminate ALL record offset
corruption and allow CreateProcess to run natively.

### 3. Multi-target build pipeline
Once processes dispatch, Root calls CreateShell → Make_Process('SYSTEM.SHELL').
This requires SYSTEM.SHELL compiled from source and placed on disk image.

## Quick reference
- Build: `make`
- Run: `rm -f build/lisa_profile.image && ./build/lisaemu --headless Lisa_Source 5000`
- Commit: `d7be85e` (25/27, both processes created)
