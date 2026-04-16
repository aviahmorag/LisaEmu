# LisaEmu — Next Session Handoff (2026-04-16)

## Session: 20+ codegen fixes, SCHEDULER DISPATCHES

### Milestone: the scheduler dispatches processes!
Both MemMgr and Root are created, queued in fwd_ReadyQ, and the
scheduler's SelectProcess finds them and attempts Launch. They crash
immediately because the environment save area is uninitialized.

## Two issues blocking process execution

### 1. ord(@proc) generates wrong address
`ord(@MemMgr)` produces $CCB802 (A5-relative global var offset) instead
of $043F56 (code address). The codegen for `@proc_name` treats procedure
names as global variables instead of code symbols. This means start_PC
in Make_SProcess is wrong for both MemMgr and Root.

Fix: in gen_expression for `@ident`, check if the identifier is a
procedure/function (via proc sig lookup) and emit its code address
(from the linker relocation) instead of an A5-relative offset.

### 2. Environment save area not initialized
CreateProcess HLE sets PCB.priority and blk_state but doesn't set up
the syslocal's env_save_area. The scheduler's Launch loads:
- PC from env_save_area.PC (offset 0)
- SR from env_save_area.SR (offset 4)  
- A5 from env_save_area.A5 (offset 0 of the env block?)
- A6, A7 from env_save_area

The env_save_area is in the syslocal segment. For MemMgr:
- syslocal at $CCC000
- env_save_area offset = from syslocal record (check SYSGLOBAL type)

Fix: in CreateProcess HLE, write correct values to the env_save_area
at the right offset within the syslocal segment.

## Segment addresses
```
MemMgr: PCB=$CCB880, syslocal=$CCC000(3072B), stack=$CCCC00(4608B)
Root:   PCB=$CCB8C4, syslocal=$CCDE00(3072B), stack=$CCEA00(10752B)
```

## Quick reference
- Build: `make`
- Run: `rm -f build/lisa_profile.image && ./build/lisaemu --headless Lisa_Source 5000`
- Commit: `5e3562c` (scheduler dispatches, processes crash)
- MemMgr code: $043F56, Root code: check map for 'Root'
