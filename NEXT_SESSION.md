# Next Session Handoff (2026-04-18 late — P89 regression root-caused to PCB field-offset skew)

## TL;DR

The P89 codegen fix exposes a **cross-module record-layout inconsistency for
the PCB record**. `STARTUP.INIT_PROCESS` writes `slocal_sdbRP` at PCB
offset **+48** (correct, matches Apple's record order). `MMPRIM.MAP_SYSLOCAL`
reads it at offset **+20** (= glob_id). The reader gets $ABC (glob_id's magic
value) instead of $44FE, computes c_sysl_sdb = b_sysglobal_ptr + $ABC =
$CC7AB8 (uninitialized area with memaddr=memsize=0), then writes origin=0
to SMT[seg 103], zeroing the syslocmmu mapping and crashing Launch.

This is the same class as the long series of field-offset bugs already
fixed (P80b iterative record-pre-pass, P82 variant records, P87a/d
byte-array stride + packed bit-packing). **PCB is a known-complex record
and hasn't been fully stabilized across all modules that use it.**

State: **22/27**, FS_INIT reached. Baseline (pre-P89): 23/27 — works by
accident because P89's codegen fix is what made MAP_SYSLOCAL's
`with c_smt^[syslocmmu] do` actually emit writes. Before P89, the whole
body was a no-op, so the wrong offset was harmless.

## Accomplished this session

### Uncommitted (see `git diff`)

- `src/main_sdl.c` — bundle-path loading for ROM/map/disk. Previously the
  headless runner was loading **stale** `build/lisa_boot.rom`,
  `build/lisa_linked.map`, `build/lisa_profile.image` instead of the fresh
  `build/rom/lisa_boot.rom` / `build/LisaOS.lisa/{linked.map,profile.image}`.
  This meant "22/27 with P89" in the previous handoff was actually running
  STALE code (the one in the 18:11 build directory). With fresh artifacts:
  baseline = 23/27, P89+fixes = 22/27 — now reproducible and deterministic.

- `src/toolchain/linker.c` — LOADER-yields-to-non-LOADER rule in
  duplicate-ENTRY resolution. LOADER.TEXT and STARTUP.TEXT both export
  `SETMMU` with different signatures (4-arg vs 5-arg). Kernel callers
  pass 5 args, so STARTUP's version needs to win. Added alongside the
  existing DRIVERASM-yields and stub-yields rules. After this, linker
  map has `$000404 SETMMU` (STARTUP's) instead of LOADER's $03ED38.

- `src/m68k.c` — **P89c smt_adr prime**. On first SETMMU entry when A5
  is in sysglobal range ($CC0000-$CE0000), writes `g_hle_smt_base` into
  A5-6112 (= LOADER's `smt_adr` VAR). Redundant after the linker fix
  above (STARTUP uses `smt_addr` at A5-24887, not `smt_adr`), but kept
  as belt-and-suspenders for any remaining LOADER references.

- `NEXT_SESSION.md` — this file.

### Diagnostic findings

**MAKE_REGION args arrive correct.** Probed entry with right-to-left arg
decoding — MAKE_REGION#4 (syslocal) gets memaddr=$087600, memsize=$4000.
Calls 1–5 all have sensible args.

**The sdb the syslocal MAKE_REGION creates IS populated correctly.**
Its real address is $CCB4FA (per INIT_PROCESS probe). Dumping bytes:
`00CC B544 | 00CC B4B0 | 043B 0020 | 0102 0001 | ...`
- fwd_link=$CCB544, bkwd_link=$CCB4B0 (valid pointers)
- memaddr=$043B (= $087600/$200 ✓)
- memsize=$0020 (= $4000/$200 ✓)
- lockcount=$01, sdbtype=$02 (data)

So MAKE_REGION ran correctly and the sdb has the right data. The
regression is NOT a MAKE_REGION codegen bug.

**INIT_PROCESS (in STARTUP) writes slocal_sdbRP at PCB+48.** Observed:
c_pcb dump at MAP_SYSLOCAL entry shows `+48: 000044FE` — which is
`slsdb - b_sysglobal_ptr` = `$CCB4FA - $CC6FFC` = $44FE ✓.

**MAP_SYSLOCAL (in MMPRIM) reads from PCB+20.** The c_sysl_sdb computation
resolves to $CC6FFC + $ABC = $CC7AB8, which is glob_id's offset (+20)
holding Apple's magic value $ABC. So MMPRIM's compile of the PCB record
has slocal_sdbRP laid out at a DIFFERENT offset than STARTUP's compile.

PCB field order per `source-procprims.text.unix.txt:53-87`:
```
next_schedPtr, prev_schedPtr, semwait_queue,      { 12 bytes of ptrs }
priority, norm_pri, blk_state, domain, sems_owned, { mixed-size scalars }
glob_id, proctype, np_count,                       { more scalars }
gplist_ptr, softints, fatherptr, sonptr, brotherptr,
fam_sem,                                           { nested semaphore record }
terming, termcause,
slocal_sdbRP,                                      { ← here }
plcbRP, need_mem, excep_pending, pcbfreqptr
```

**Likely cause:** MMPRIM's compile doesn't see the full PCB type definition,
OR sees a truncated/stale version. Could be:
1. PCB is declared in procprims and imported into MMPRIM via `type` clause;
   if the import loses fields, offsets collapse.
2. Forward type declaration without proper back-patching.
3. Record pre-pass (`toolchain_bridge.c` Phase 2 fixup) handles PCB
   inconsistently — fixes STARTUP's copy but not MMPRIM's.
4. Some field type between glob_id and slocal_sdbRP is sized wrong in
   MMPRIM's view, shifting the later offsets.

**To pin down which:** add a debug dump in pascal_codegen.c's record
finalization (or toolchain_bridge Phase 2 fixup) printing the PCB layout
(all field names + offsets) for each module that references PCB. Compare
STARTUP's PCB layout vs MMPRIM's. Differences → fix the pre-pass to sync
them.

## Next step plan (prioritized)

### Option 1 — root-cause the PCB layout mismatch (the real fix)

1. Instrument pascal_codegen.c or toolchain_bridge Phase 2 pre-pass to
   dump PCB record layout per module. Find the offset of slocal_sdbRP
   in STARTUP.compile vs MMPRIM.compile.
2. Likely fix pattern (see P82/P80b precedents): extend the iterative
   pre-pass to ensure all cross-module references to PCB pick up the
   SAME offsets. Pin slocal_sdbRP if needed.

### Option 2 — PASCALDEFS-pin slocal_sdbRP (quick hammer)

If the layout is stable in STARTUP (+48), add a per-field pin in the
codegen (like the PASCALDEFS-pin table for A5 globals, but for record
fields). Force slocal_sdbRP to always be at offset 48. Narrow scope,
probably fast to implement.

### Option 3 — HLE-guard MAP_SYSLOCAL (fallback only; user dislikes)

If c_sysl_sdb.memaddr == 0 at MAP_SYSLOCAL entry, skip the SETMMU +
PROG_MMU calls and let bootrom's pre-programmed seg 103 stand. Restores
23/27 but masks the underlying bug. **Only use as a stopgap while
investigating Option 1/2** — user has strong preference for proper
structural fixes (memory `feedback_do_the_real_fix.md`).

## Verify-before-continuing

```bash
git status                      # should show 4 modified files + NEXT_SESSION.md
git stash
git checkout 3585952 -- src/toolchain/pascal_codegen.c
make && ./build/lisaemu --headless Lisa_Source 300 2>&1 | grep "Milestones reached:"
# Expect: 23/27  (pre-P89 baseline, with my infra fixes on top)
git checkout HEAD -- src/toolchain/pascal_codegen.c
git stash pop
make && ./build/lisaemu --headless Lisa_Source 300 2>&1 | grep "Milestones reached:"
# Expect: 22/27  (P89 + my fixes, blocked on PCB offset skew)
```

## Files changed this session (uncommitted)

- `src/main_sdl.c` — 9 lines; fresh bundle paths.
- `src/toolchain/linker.c` — 25 lines; LOADER-yields duplicate rule.
- `src/m68k.c` — 49 lines; P89c smt_adr prime hook.
- `CLAUDE.md` — Current Status section refreshed for P89d findings.
- `NEXT_SESSION.md` — this handoff (new).

Toolchain audit: **100% assemble, 100% parser, link OK, 94.4% JSR resolution**
— no structural regression.

## Pick up here

Paste this prompt to resume:

> Continue from NEXT_SESSION.md. We root-caused the P89 regression to a
> cross-module PCB record-layout mismatch: STARTUP's INIT_PROCESS writes
> slocal_sdbRP at PCB+48 (correct), but MMPRIM's MAP_SYSLOCAL reads from
> PCB+20 (= glob_id's offset). MAP_SYSLOCAL gets $ABC instead of $44FE,
> resolves c_sysl_sdb to an uninitialized area ($CC7AB8 instead of
> $CCB4FA), reads memaddr=memsize=0, writes origin=0 to SMT[seg 103],
> zeros the syslocmmu mapping, and Launch crashes reading env_save_area
> through a now-unmapped syslocal region. MAKE_REGION itself works fine
> (probed: memaddr=$087600 memsize=$4000 arrive correctly; sdb at
> $CCB4FA has correct memaddr=$043B memsize=$0020). Next: instrument
> pascal_codegen.c or toolchain_bridge Phase 2 pre-pass to dump PCB
> layout per module. Compare STARTUP's vs MMPRIM's slocal_sdbRP offset.
> Fix the pre-pass to sync them. See Option 1 in NEXT_SESSION.md for
> the full plan; don't apply Option 3 (HLE guard) — user prefers real fixes.
