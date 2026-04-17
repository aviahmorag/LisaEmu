# Next Session Handoff (2026-04-18 end — P89d structural fixes landed; new hard_excep blocker at PC=$CC07)

## TL;DR

Two structural Pascal-codegen bugs fixed this session that were together
causing MAP_SYSLOCAL to write origin=0 to SMT[seg 103] and zeroing the
syslocmmu mapping at boot:

1. **P80g post-creation record repair was over-eager.** It matched imported
   records to local by first-field-name only. MMPRIM's `p_linkage` (2 ×
   ptr_p_linkage, 8 bytes) was being overwritten by DRIVERDEFS's `linkage`
   (2 × relptr, 4 bytes) because both start with `fwd_link/bkwd_link`. Gated
   the repair on `fields[1].offset == 0` (the real corruption signature).

2. **find_proc_sig returned first non-external match, even if unresolved.**
   MAP_SYSLOCAL registered 4 times in MMPRIM — first with unresolved ptype
   (param_size=2 fallback), later with ptr_PCB resolved (param_size=4).
   Scheduler picked the unresolved one, pushed c_pcb as a WORD instead of
   LONG, which byte-shifted the whole frame-read chain. Added a resolution
   tier to prefer fully-resolved sigs.

Post-fix the boot reaches the same 22/27 count but with a **different and
later failure mode**: MAP_SYSLOCAL now writes the correct origin=$043B
access=$07 limit=$20 to SMT[seg 103], Launch succeeds, and boot hits a
NEW blocker — hardware exception (vector 4, odd PC=$00CC07) handled by
`hard_excep` → SYSTEM_ERROR(10201).

Baseline (pre-P89) still reaches 23/27 by accident (silent SETMMU didn't
touch seg 103, so bootrom's pre-programmed mapping stood).

## Accomplished this session

### Uncommitted changes

- `src/main_sdl.c` — load from bundle paths (ROM at `build/rom/`, map+disk
  at `build/LisaOS.lisa/`), fall back to legacy flat paths only for
  `--image` mode. All prior "22/27" reports were running STALE artifacts.
- `src/toolchain/linker.c` — LOADER entries yield to non-LOADER in
  duplicate-ENTRY resolution. STARTUP's 5-arg SETMMU now wins over
  LOADER's 4-arg version (matches kernel callers).
- `src/m68k.c` — P89c smt_adr prime on first SETMMU entry (A5 in sysglobal
  range). LOADER's BOOTINIT would have initialized LOADER's `smt_adr` VAR
  at A5-6112; we skip BOOTINIT, so this lazy prime fills the gap.
- `src/toolchain/pascal_codegen.c`:
  - P80g record repair gated on `fields[1].offset == 0` (per P89d
    diagnosis above). Prevents MMPRIM's p_linkage (8 bytes) from being
    clobbered by DRIVERDEFS's linkage (4 bytes).
  - `find_proc_sig` resolution tiering (local-resolved > local-partial >
    imported-resolved > imported-partial > external). Ensures callers pick
    the sig registered AFTER types were fully resolved.
- `CLAUDE.md` + `NEXT_SESSION.md` — status refresh.

Toolchain audit: 100% assemble, 100% parser, link OK, 94.4% JSR resolution
— no structural regression.

### Diagnostic trail

- Observed: MAP_SYSLOCAL writes origin=0 to SMT[seg 103]; Launch crashes
  reading A5=$3F8 from env_save_area (actually stomped TRAP6 vector).
- Instrumented `lvalue_field_info_full` to dump `slocal_sdbRP` and
  `memaddr` resolutions. Found MMPRIM's `sdb` had sz=52 memaddr@4
  (WRONG) for most accesses, should be sz=56 memaddr@8.
- Instrumented `AST_TYPE_RECORD` in MMPRIM to dump `p_linkage` state.
  Found local `p_linkage`: sz=4, bkwd_link@2 (should be sz=8 bkwd_link@4).
  Each field correctly `kind=TK_POINTER sz=4`, but the record layout
  collapsed to 4 bytes total.
- Read lines 662-677 of `pascal_codegen.c` (P80g post-creation record
  repair). Realized it matches `imp` by `imp->fields[0].name ==
  t->fields[0].name` only. DRIVERDEFS's `linkage` (fwd_link: relptr,
  bkwd_link: relptr, size=4) matched MMPRIM's `p_linkage` (fwd_link:
  ptr_p_linkage, bkwd_link: ptr_p_linkage, size=8). Copied linkage's
  offsets over p_linkage's. → P80g fix.
- Re-ran: p_linkage now sz=8 bkwd_link@4, sdb now sz=56 memaddr@8. But
  MAP_SYSLOCAL still wrote origin=0.
- Instrumented runtime: MAP_SYSLOCAL entry showed c_pcb=$CCB58E,
  rp@+50=$44FE, c_sysl_sdb=$CCB4FA, memaddr=$043B (all correct!). Yet
  the compiled code wrote 0.
- Disassembled MAP_SYSLOCAL at $E6DA and probed D0/D2/A0 at each step.
  Found MOVE.L 8(A6), D0 was returning $B58E0000 instead of $00CCB58E
  — byte-shifted by 1 byte.
- Probed A6-stack: A6+8 contained $B58E0400; reading at A6+11 gave
  $00CCB58E (correct c_pcb). So caller pushed c_pcb starting at wrong
  byte.
- Disassembled caller at $5EC64 (Scheduler → MAP_SYSLOCAL). Found
  `$3F00` = `MOVE.W D0, -(A7)` — caller pushing c_pcb as a WORD (2 bytes)
  instead of LONG (4 bytes).
- Instrumented `register_proc_sig`. Saw MAP_SYSLOCAL registered 4 times;
  first with param_type=NULL (unresolved) → param_size=2; third with
  ptr_PCB (4-byte pointer) → param_size=4. find_proc_sig returned the
  first, not the third. → find_proc_sig fix.
- Re-ran: SMT[seg 103] now has origin=$04 $3B access=$07 limit=$20.
  Launch succeeds. But boot hits vector-4 (illegal instr) at odd PC=$CC07.

## Next blocker: hard_excep at PC=$00CC07

After MAP_SYSLOCAL + PROG_MMU correctly map syslocmmu, the boot
progresses past Launch into actual process code. But the CPU takes an
illegal-instruction trap (vector 4) at PC=$00CC07 — an ODD address in
user mode.

Observed in headless trace:
```
[VEC-FIRST] v=4 PC=$00CC07 SR=$0004 SSP=$00F7FD14 USP=$00F7FD14 handler=$0006AD56
HLE SYSTEM_ERROR(10201) at ret=$01FD4E SP=$00CBFED6 A6=$00CBFF3F
```

PC=$00CC07 is in seg 0 (logical address under $20000). Seg 0 is
unmapped or mapped to vector table. Trying to execute from $CC07 (odd
and probably inside vector-table data) throws the illegal-instr trap.

`hard_excep` at `$01FACC` (Pascal impl in source-EXCEPRIM/EXCEPRES)
catches this and calls SYSTEM_ERROR(10201) ("hardware exception while
in system code").

### Likely causes — investigation plan

1. **CreateProcess HLE wrote wrong start_PC into env_save_area.** Launch
   RTEs with a stomped PC. Check `src/m68k.c`'s CreateProcess bypass
   (look for "CreateProcess HLE populates env_save_area" per CLAUDE.md).
   Print start_PC at HLE time and verify against the proc's real entry.

2. **Byte-shift-class bug still lurking.** The MAP_SYSLOCAL fix corrected
   one WORD-vs-LONG push site. Maybe CreateProcess or another proc has
   a similar param-size-2 issue that byte-shifts a proc's entry-PC
   argument.

3. **Stack unwind hits wrong return frame.** Launch's RTE pops A5/A6/PC
   from env_save_area. If env_save_area has any byte-shift or
   zero-padding, PC will be wrong.

### Starting point

Instrument the CPU just before the illegal trap fires:
- At v=4 handler entry, dump the exception frame (SR, PC, SSP, A6 chain).
- At the instruction BEFORE the trap (walk back from PC=$CC07 via the
  last PC ring), log what instruction set PC=$CC07.
- Probe CreateProcess HLE entry/exit; if it HLE-bypasses, note where
  start_PC comes from and whether the env_save_area it wrote is
  byte-aligned.

## Verify-before-continuing

```bash
# Baseline (pre-P89): 23/27 by accident.
git stash
git checkout 3585952 -- src/toolchain/pascal_codegen.c
make && ./build/lisaemu --headless Lisa_Source 300 2>&1 | grep "Milestones"
# Expect: 23/27

# Restore P89 + my fixes.
git checkout HEAD -- src/toolchain/pascal_codegen.c
git stash pop
make && ./build/lisaemu --headless Lisa_Source 300 2>&1 | grep "Milestones"
# Expect: 22/27 — same count, but DIFFERENT (later) failure mode.
```

## Pick up here

Paste this prompt to resume:

> Continue from NEXT_SESSION.md. Two structural Pascal-codegen bugs got
> fixed this session (P80g post-creation-repair gate + find_proc_sig
> resolution-tiering) — MAP_SYSLOCAL now correctly maps seg 103 syslocmmu
> with origin=$043B access=$07 limit=$20. Launch no longer crashes from
> stack corruption. Boot now hits a NEW blocker: vector 4 (illegal instr)
> at odd PC=$00CC07 in user mode → hard_excep → SYSTEM_ERROR(10201). Odd
> PC suggests a byte-shift bug still lurking, likely in CreateProcess HLE
> or another Pascal-Pascal call site that treats pointer params as
> WORDs. Start by instrumenting just before vector 4 fires (walk back
> the last PC ring to find the instruction that JMP/RTE'd to $CC07), and
> check CreateProcess HLE's start_PC computation. Same class as the
> MAP_SYSLOCAL fix — once you find the next WORD-vs-LONG parameter push,
> check whether it's also fixable via find_proc_sig's new resolution
> tiering or requires its own fix.
