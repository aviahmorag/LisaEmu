# LisaEmu — Next Session Handoff (2026-04-16)

## Accomplished this session

### P80: 8-char significant identifiers (structural codegen fix)
Lisa Pascal identifiers are significant to only 8 characters. Our compiler
was doing full-length comparison, causing `ordrefncbptr` (declared) and
`ordrefncb` (used) to be treated as different variables. This caused
GETSPACE to be called with a NULL output pointer, crashing process creation.

Fix: `str_eq_nocase` now returns true after 8 matching characters,
matching the original Lisa Pascal compiler behavior. This affects ALL
identifier comparisons in the codegen — resolving a whole class of
potential mismatches throughout the codebase.

### P80a: FS bypass removal
Disabled DecompPath and parse_pathname HLE bypasses. With the 8-char fix,
these FS functions can run natively without the codegen bugs that originally
required bypasses. Find_it bypass still active (data structure init issue).

### P35 disabled: SYS_PROC_INIT runs for real
The body of SYS_PROC_INIT now executes instead of being HLE-bypassed.
Process creation code (Make_SProcess, Get_Resources, MAKE_DATASEG) runs.

### PASCALDEFS offsets corrected
The diagnostic dump code was using wrong A5 offsets (small numbers like -148)
instead of the real PASCALDEFS offsets (-24575 for sg_free_pool_addr, -24785
for b_syslocal_ptr, -25691 for mmrb_addr, etc.). Fixed in both m68k.c and
lisa.c. Globals are confirmed valid at SYS_PROC_INIT entry.

## In progress

### SYS_PROC_INIT body — stalls during MAKE_DATASEG
21/27 milestones with P35 disabled. SYS_PROC_INIT enters and runs process
creation code but crashes before completing. The crash chain:

1. `Sys_Proc_Init` → `Make_SProcess` (for MemMgr, resident) →
   `Get_Resources` → `MAKE_SYSDATASEG` → `MAKE_DATASEG`
2. Inside MAKE_DATASEG, `WAIT_SEM(c_mmrb^.sds_sem)` encounters
   bogus sem_count=-12223 ($D041). The MMRB structure is corrupted.
3. `DS_OPEN` proceeds but segment 78 ($9C0000) is never mapped.
4. `Build_Syslocal` writes to unmapped seg 78 → writes dropped.
5. VEC-GUARD writes from `e_badrefnum` code to $000000/$000004.
6. SEG_IO pops bogus return address → crash to unmapped RAM.
7. System enters infinite loop at MMU-mapped address.

**Root cause hypothesis**: MMRB record layout is wrong. The sem_count
at the expected sds_sem offset reads as $D041 — not a valid semaphore
value. Either:
- The MMRB type's field layout is computed incorrectly (field sizes differ
  from what Apple's compiler produced), or
- MM_INIT's code for initializing the MMRB fields generates wrong
  instructions (wrong field offsets in MOVE instructions).

## Next priorities

### 1. Dump MMRB structure at SYS_PROC_INIT entry
Read memory at mmrb_addr ($CCB00A) and dump the first ~80 bytes.
Compare against the expected MMRB layout:
```
offset 0:  hd_qioreq_list (linkage: 4 bytes — fwd:int2, bkwd:int2)
offset 4:  seg_wait_sem (semaphore: 8 bytes — count:int2, owner:int2, wait_q:ptr_PCB(4))
offset 12: memmgr_sem (semaphore: 8 bytes)
offset 20: memmgr_busyF (boolean: 1-2 bytes)
offset 22: clr_mmbusy (boolean: 1-2 bytes)
offset 24: numbRelSegs (int2: 2 bytes)
offset 26: req_pcb_ptr (ptr_pcb: 4 bytes)
offset 30: hd_sdscb_list (linkage: 4 bytes)
offset 34: sds_sem (semaphore: 8 bytes) ← P78-SS fires here with bogus sem_count
```

Verify: is the sem_count at offset 34 actually $D041? If so, either the
offset is wrong (our codegen placed sds_sem at a different position) or
MM_INIT didn't initialize it.

Key types (from source):
- `relptr = int2` (2 bytes, NOT 4)
- `semaphore = {sem_count: int2, owner: relptr=int2, wait_queue: ptr_PCB=4}` = 8 bytes
- `linkage = {fwd_link: relptr=int2, bkwd_link: relptr=int2}` = 4 bytes

### 2. Check boolean field padding in records
The MMRB has adjacent boolean fields (memmgr_busyF, clr_mmbusy).
In unpacked records, our codegen may pad booleans differently than
Apple's compiler. Verify field layout with a focused test.

### 3. Once MMRB is valid, MAKE_DATASEG should complete
After fixing the MMRB layout, the semaphore operations will work,
DS_OPEN should succeed, and Build_Syslocal can write to a properly
mapped segment. This unblocks the entire SYS_PROC_INIT → Root →
CreateShell → SYSTEM.SHELL chain.

## Quick reference
- Build: `make`
- Run: `rm -f build/lisa_profile.image && ./build/lisaemu --headless Lisa_Source 5000`
- Commit: `dfe8e68` (clean, 21/27 milestones with P35 disabled)
- Xcode build fails due to signing/environment issues (not code errors)
