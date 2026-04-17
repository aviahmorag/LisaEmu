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

### UPDATE (end of session): P89e fix emits all 13 Build_Stack stores, but env_save_area.PC is still $00CBFF at Launch entry

After P89e (AST_FIELD_ACCESS in AST_WITH), `Build_Stack` correctly emits
13 record-field stores including `env_save_area.PC := ord(@Initiate) =
$0503F8`. The emit_reloc for @Initiate works. All offsets resolve.

BUT runtime probe of env_save_area at Launch entry shows:
```
env_save_area@$CE0006: PC=$0000CBFF SR=$0001 A5=$00CC6FFC A6=$00CBFF8E A7=$00F7FD14
```

PC is still $00CBFF — not @Initiate. Investigation:

- Build_Stack writes correctly, but to the sloc_ptr it receives as a
  param (newpcb_ptr's sloc_handle-derived sloc_ptr).
- Launch reads from `b_syslocal_ptr` (A5-24785), which points to the
  CURRENT process's syslocal — = POP's syslocal at $CE0000.
- POP is the pseudo-outer-process. Apple's code does NOT call
  CreateProcess for POP — POP runs INITSYS directly as the boot
  process. Its env_save_area is never populated.
- Our boot path: after FS_INIT + error-unwind (10738, 10707 both
  suppressed), Scheduler fires → Launch → RTE's from POP's
  uninitialized env_save_area → garbage PC = whatever sits in that
  memory (observed: $CBFF or $CC25 depending on link layout).

So the **next blocker is not a codegen bug — it's a control-flow
bug**: Scheduler/Launch shouldn't be firing before SYS_PROC_INIT runs.
The HLE that suppresses SYSTEM_ERROR(10707) "unwind to FS_INIT caller"
must be dropping us into the Scheduler dispatch path instead of
returning to STARTUP's INITSYS to continue with SYS_PROC_INIT.

### Root-caused (earlier in session): `ord(@Initiate)` codegen bug in Build_Stack

The PC ring at the illegal trap shows:
```
... $06FD76 $06FD7A $06FD7C $00CBFF $00CC01 $00CC05 $00CC07
```

$06FD7C is the **RTE in `Launch`** (opcode $4E73). Launch reads the
process's `env_save_area` starting at syslocal+6 (PC at offset 0, SR
at offset 4) and pushes them onto SSP before RTE. The popped PC is
$00CBFF — odd, so the emulator fetches $00CBFF, $00CC01, $00CC05,
hits opcode $00CA at $00CC07 which is illegal → hard_excep.

**Build_Stack (`PMMAKE.TEXT:403`) does** `PC := ord(@Initiate);`.
Initiate is a procedure in PMMAKE. Its actual address in the linker
map:
```
$04FD7C  Initiate
```

But the stored value is $0000CBFF — completely wrong. Same class of
bug as P80h2: `ord(@proc)` codegen must detect procedure identifiers
via find_proc_sig and emit `MOVE.L #imm32, D0` with a linker
relocation, not fall through to gen_lvalue_addr's `LEA offset(A5),A0`.

If Initiate is a **nested** proc (inside CreateProcess), it might not
be registered in proc_sigs the way find_proc_sig expects, so `@Initiate`
falls back to the A5-relative (global-variable-style) lookup and
produces garbage.

### Starting point

1. Check whether Initiate is registered via find_proc_sig. Add a probe
   in AST_ADDR_OF that logs the resolved address/path for "Initiate".
2. If not registered: ensure nested procs are registered in proc_sigs
   (the P81b commit did this for parameterless nested procs; verify it
   covers Initiate's case here).
3. If registered: check the relocation emission path — is the imm32
   being written as a 4-byte relocation or getting truncated somewhere?
4. Once fixed, re-run. Expected: env_save_area.PC = $04FD7C, RTE jumps
   to Initiate, boot progresses past SYS_PROC_INIT.

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

> Continue from NEXT_SESSION.md. Three Pascal-codegen bugs got fixed
> this session (P80g post-creation-repair gate + find_proc_sig
> resolution-tiering + AST_FIELD_ACCESS in AST_WITH). MAP_SYSLOCAL
> now correctly maps seg 103 syslocmmu with origin=$043B. Build_Stack
> now emits all 13 env_save_area stores. But boot still halts at 22/27
> with hard_excep (SYSTEM_ERROR 10201) at odd PC.
>
> The runtime probe at Launch entry shows env_save_area.PC = $0000CBFF
> — despite Build_Stack correctly writing @Initiate = $0503F8 to the
> same field. The discrepancy is because Build_Stack writes to the
> NEW process's sloc_ptr, but Launch is firing on POP (the
> pseudo-outer-process) whose env_save_area was never populated
> (Apple's code doesn't call CreateProcess for POP).
>
> So this is now a **control-flow issue, not a codegen issue**: our
> post-FS_INIT error-recovery path (HLE suppression of 10738 + 10707)
> is dropping into Scheduler/Launch BEFORE SYS_PROC_INIT runs. The
> recovery should return control to STARTUP's INITSYS body so it
> proceeds to the SYS_PROC_INIT call, not dispatch a process yet.
>
> Next: instrument the 10707 suppression path in src/m68k.c — dump
> the stack/A6-chain at that point, verify the unwind target is
> actually INITSYS's FS_INIT call site and not the scheduler. If the
> unwind RTE/JMP target is wrong, fix the suppression logic.

### END-OF-SESSION ADDENDUM

Added **P89f dynamic FS_INIT/FADECONERT lookup** in lisa.c's 10707
suppression (the hardcoded range was stale — $002C8C..$002D4C,
current is $002EEA..$002FAA). After the fix, 10707 unwind lands at
$005A0A inside BOOT_IO_INIT (correct spot, right after the
`JSR FS_INIT` call).

But boot still halts at 22/27. Further probing at Scheduler#1 entry
shows:
```
A5=$CC6FFC  A6=$CC6FFC  A7=$CBFF91 (ODD!)  c_pcb=$00CCB58E
caller ret=$A200011A (invalid)  A6 chain: ret=$1000000 (garbage)
```

Scheduler is entered with an **odd SP ($CBFF91)** and **garbage A6
chain**. That means Scheduler was NOT invoked via a clean JSR — the
stack is already corrupt by the time it fires. Two hypotheses:

1. **Byte-push codegen bug still lurking**. Pascal's standard push
   is word/long aligned. If some codegen emits `MOVE.B Dn, -(A7)`
   instead of `MOVE.W Dn, -(A7)` for a byte-sized param, SP ends up
   odd. We fixed the `c_pcb: ptr_pcb` case via find_proc_sig tiering;
   another param-size mismatch may be lurking elsewhere on the path
   through BOOT_IO_INIT's device init loop.

2. **A timer IRQ fires during BOOT_IO_INIT's INTSON with the CPU
   still in a half-setup state**. The CPU pushes the exception frame
   onto SSP, dispatches to the handler which calls Scheduler. If the
   interrupt fires while SP is odd, or if the handler has a bug, the
   frame may be corrupted.

### Recommended next-session plan

**P89g hunted down the odd-SP source** with a runtime watch and
found `Block_Process` at $0113CA emitting `LINK A6, #-3`. The
displacement -3 is odd → A7 lands on an odd byte.

Root cause: `pascal_codegen.c` computes `frame_size` for each proc
by summing local sizes. When the last local is a 1-byte type (int1,
boolean with 1-byte packing, etc.) and no subsequent even-sized local
forces padding, frame_size stays odd.

**Fix:** `if (frame_size & 1) frame_size++;` right before emitting
the LINK displacement. This rounds up odd frames to even and matches
Apple Pascal's convention (never emit odd-displacement LINK).

Committed. After the fix, Scheduler#1 A7 is now EVEN ($CBFF90 vs
$CBFF91). Launch still fires on POP's uninit env_save_area — error
code shifted from 10201 (illegal-instr) to 10204 (f-line trap),
same class (hardware exception in system code), just a different
opcode lands on the odd boundary.

**Remaining work for next session:** the Scheduler→Launch→POP-env
sequence is still the final blocker. Two approaches:

1. **Populate POP's env_save_area at bootrom time** so if Scheduler
   dispatches POP, the RTE lands somewhere safe (e.g., an ExitSys
   trampoline or just the PC where POP was about to be when the
   scheduler fired).

2. **Suppress premature Scheduler dispatch**. BOOT_IO_INIT enables
   interrupts via INTSON; a timer IRQ fires, handler runs, Scheduler
   selects POP and Launches it. We can HLE-guard: if the selected
   PCB is the current c_pcb, do an RTE-equivalent return without
   touching env_save_area (since POP is already running and its
   context is on SSP from the interrupt).

Option 2 is probably the cleaner fix — it matches what Apple's
Scheduler should actually do for a single-process case.

### P89h experiment (backed out)

Tried adding an HLE guard at Launch entry: detect env_save_area.PC is
zero/odd/out-of-range → skip the env_save_area RTE, do a clean
`return to caller` instead. Guard fires 4 times (budget capped), but
boot still halts at 22/27 — Scheduler keeps re-calling Launch in a
loop. The "return to caller" goes back to Scheduler's next instruction,
but Scheduler may loop back to SelectProcess/Launch instead of RTS'ing.

Backed out pending more analysis of Scheduler's loop structure:
- Is Scheduler inside an interrupt handler? Then the right fix is
  for the handler itself to RTE when candidate == current c_pcb.
- Is Scheduler inside a Pascal body that loops? Then we need to
  RTS back to Scheduler's *caller*, not to Launch's immediate
  caller within Scheduler.

Next-session plan: instrument Launch entry's A7 chain to find
Scheduler's call site, then trace Scheduler's outer caller.

### Launch caller chain (probed)

At Launch#1 entry:
```
[Launch#1 caller chain] ret=$05F792  SP=$CBFF80
    A6=$CBFF8C → prev_A6=$CC6FFC ret=$FFA20001
    A6=$CC6FFC → prev_A6=$038000 ret=$1000000
```

The A6 chain is corrupt (prev_A6=$CC6FFC is b_sysglobal_ptr = A5,
ret=$FFA20001 is garbage). This suggests Launch was entered from a
non-Pascal context — likely an interrupt handler that didn't set up
a Pascal frame.

### env_save_area bytes at Launch entry (probed)

```
syslocal@CE0000 first 64 bytes:
  00CE01EE 40000100 00CB0000 00CC0000 00000000 00CCB58E ...
```

Decoded:
- offset 0: sl_free_pool_addr = $00CE01EE (points into syslocal's
  free pool — valid)
- offset 4: size_slocal = $4000
- offset 6-9: env_save_area.PC = **$010000CB** (garbage / uninit;
  should be @Initiate or current instruction pointer)
- offset 10-11: env_save_area.SR = $0000
- offset 12-...: D0, D1, ... = various garbage

POP's env_save_area has never been explicitly written with valid
values. The bytes are either randomized / leftover init memory or
incidentally-written by unrelated code.

### Strategic options for next session

1. **Populate POP's env_save_area at bootrom_build time** to contain a
   "safe resume" PC (e.g., INITSYS's current PC or an ExitSys handler
   that gracefully halts). Since Apple's code doesn't write POP's
   env_save_area, we have to do it ourselves.

2. **Intercept the INTSON HLE** and defer enabling interrupts until
   SYS_PROC_INIT has run. Without interrupts, no Scheduler dispatch
   fires, and boot proceeds sequentially through INITSYS until
   SYS_PROC_INIT creates real processes with properly-populated
   env_save_area.

3. **HLE-suppress the first N Launch calls** — skip dispatch until
   SYS_PROC_INIT milestone fires, then re-enable. This is the
   cheapest workaround but not structurally correct.

Option 2 is most aligned with Apple's intent: interrupts shouldn't
dispatch until system processes exist. Look at the INTSON HLE
implementation and add a "defer until SYS_PROC_INIT reached" gate.

### Files committed this session (all pushed to origin/main)

- `5b2f848` — main_sdl bundle paths + linker LOADER-yields + P89c prime.
- `6b4a290` — P89d P80g gate + find_proc_sig tiering.
- `48d8972` — P89e AST_FIELD_ACCESS in AST_WITH.
- `d03fd1f` — docs: CLAUDE.md refresh.
- `8edf2e3` — docs: NEXT_SESSION.md update.
- `70bcf20` — P89f dynamic FS_INIT lookup for 10707 unwind.

Still at 22/27 milestones (baseline: 23/27). The fixes DO advance
execution past multiple latent bugs; the remaining 1-milestone gap is
now a control-flow / stack-corruption issue, not a codegen issue.
