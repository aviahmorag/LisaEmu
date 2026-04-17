# Next Session Handoff (2026-04-18 wrap)

## Accomplished this session

Ten commits landed on `origin/main` (5b2f848 → 73eb34c), six of them
substantive structural fixes. Six real Pascal-codegen / linker / infra bugs
rooted out. Boot state: **22/27 milestones** — same count as at session
start, but the failure has moved from a silent stack-corruption crash in
Launch to a deeper architectural blocker (Scheduler loop on POP), and each
milestone now reaches via correct data instead of accident. Toolchain audit
stays green throughout: **100% assemble, 100% parse, Link OK, 94.4% JSR
resolution**.

Fixes by commit:

- **5b2f848** — `auto: claude-turbo checkpoint`. Three fixes:
  - `src/main_sdl.c:164-169, 192-196`: headless runner loads fresh bundle
    artifacts (`build/rom/lisa_boot.rom`, `build/LisaOS.lisa/linked.map`,
    `build/LisaOS.lisa/profile.image`) with fallback to legacy flat paths.
    Previously all "22/27" reports were silently running stale artifacts
    from earlier builds.
  - `src/toolchain/linker.c:195-252`: add LOADER-yields-to-non-LOADER rule
    to duplicate-ENTRY resolution. STARTUP's 5-arg `SETMMU` now wins the
    linker collision over LOADER's 4-arg signature (matches kernel caller
    signature). Mirrors existing DRIVERASM-yields / stub-yields rules.
  - `src/m68k.c:3041-3057`: **P89c** — lazy prime of LOADER's `smt_adr` VAR
    (A5-6112) to `g_hle_smt_base` on first SETMMU entry when A5 is in
    sysglobal range. Mimics what LOADER's BOOTINIT would have done (we
    skip BOOTINIT entirely).

- **6b4a290** — `P89d: two structural codegen fixes unblock MAP_SYSLOCAL`:
  - `src/toolchain/pascal_codegen.c:662-694` (P80g-gate): gate the
    post-creation record repair on `t->fields[1].offset == 0` (the real
    corruption signature). Previously it matched imported records by
    first-field-name only, so MMPRIM's `p_linkage` (2 × ptr_p_linkage, 8
    bytes) got overwritten by DRIVERDEFS's `linkage` (2 × relptr, 4 bytes)
    because both start with `fwd_link/bkwd_link`. That truncated
    `sdb.memaddr` from offset 8 to offset 4 in MMPRIM's compile.
  - `src/toolchain/pascal_codegen.c:4645-4705` (find_proc_sig): add a
    resolution-quality tier. MAP_SYSLOCAL gets registered four times in
    MMPRIM's compile (forward decl + body, on each of two passes). First
    reg had `param_type=NULL` (unresolved → `param_size=2` fallback); third
    had `ptr_PCB` resolved (`param_size=4`). Caller Scheduler was picking
    the first (bad) one. New priority:
    `local-resolved > local-partial > imported-resolved > imported-partial
    > external`.
  - Effect: `MAP_SYSLOCAL` now writes `origin=$043B access=$07 limit=$20`
    to `SMT[seg 103]` instead of all zeros.

- **48d8972** — `P89e: handle AST_FIELD_ACCESS record expression in AST_WITH`:
  - `src/toolchain/pascal_codegen.c:3612-3636`: replace the
    "push NULL and hope for the best" stub with a real type resolver using
    `lvalue_record_type`. `with sloc_ptr^.env_save_area, stk_handle do
    begin SR := 0; PC := ord(@Initiate) end` in Build_Stack now has a valid
    `record_type` on the WITH stack, so its field stores actually emit
    (previously silently no-op'd). Build_Stack now emits all 13 expected
    record-field stores (was 8).

- **70bcf20** — `P89f: dynamic FS_INIT/FADECONERT lookup for 10707 unwind`:
  - `src/lisa.c:2882-2895`: the hardcoded FS_INIT range
    ($002C8C..$002D4C) was stale; current is $002EEA..$002FAA. Replace with
    `boot_progress_lookup("FS_INIT")` + `boot_progress_lookup("FADECONERT")`
    so the unwind correctly identifies saved-return-PCs that are inside
    FS_INIT vs outside.

- **4236dab** — `P89g: align frame_size to even before LINK emission`:
  - `src/toolchain/pascal_codegen.c:4376-4389`: `Block_Process` had a lone
    1-byte `int1` local → `frame_size=3`. `LINK A6,#-3` leaves A7 on an odd
    byte; 68000 word/long memory ops stall and interrupts write their
    exception frame to misaligned memory. Added
    `if (frame_size & 1) frame_size++;` right before the LINK emit.
    Empirically: Scheduler#1's A7 is now even ($CBFF90 vs $CBFF91); the
    hard-exception code shifted from `10201` (illegal-instr-in-system-code)
    to `10204` (f-line-in-system-code), confirming we rode past the odd-SP
    cascade.

- **73eb34c** — `boot_progress: add boot_progress_reached()`:
  - `src/boot_progress.c:240-253` + `src/boot_progress.h:44-47`: new
    `bool boot_progress_reached(const char *name)` API. Unused for now;
    added while investigating phase-aware HLE gates. Stable API for
    "don't fire until milestone X" guards in future sessions.

- Docs commits (`d03fd1f`, `8edf2e3`, `3df483f`, `32e745f`) — rolling
  refreshes of `CLAUDE.md` and `NEXT_SESSION.md`.

## In progress

Nothing in progress — all changes committed and pushed. Working tree is
about to be clean except for NEXT_SESSION.md itself (gitignored).

A **P89h experiment was attempted and backed out** (see session log /
git history): HLE guard at Launch entry to defer dispatch when
`SYS_PROC_INIT` has not been reached. Implementation added, verified
to fire correctly, verified the Launch "return to caller" lands at
`$05F792` inside Scheduler. But Scheduler at `$05F7A0-$05F7AA` has a
`BRA.W -302` that loops back to `SelectProcess` + `Launch`, so skipping
Launch just spins Scheduler forever (boot hangs at 22/27 instead of
crashing at 22/27).

Lesson: Scheduler is an infinite idle loop by design; the only exit
path is via Launch's successful RTE into the dispatched process's
env_save_area. You can't "just skip Launch" and return to the caller.
Any HLE workaround has to either fake a correct context switch or
prevent Scheduler from being invoked in the first place.

## Blocked / open questions

**Remaining blocker:** `Scheduler` fires (from the timer IRQ handler
enabled by BOOT_IO_INIT's INTSON) on POP, whose `env_save_area` was
never populated (Apple's code doesn't call CreateProcess for POP — POP
runs INITSYS on the direct CPU context). Launch RTEs from uninit
env_save_area, lands on a garbage/odd PC, `hard_excep` fires, halt at
SYSTEM_ERROR(10204) before `SYS_PROC_INIT` runs.

Concrete runtime probe at Launch#1 entry showed POP's syslocal at
`$CE0000`:

```
00CE01EE 40000100 00CB0000 00CC0000 00000000 00CCB58E ...
```

Decoding: `sl_free_pool_addr=$CE01EE` (valid), `size_slocal=$4000`,
`env_save_area.PC = $010000CB` (garbage — should be @Initiate or the
interrupted PC), `SR=$0000`. Launch's RTE pops `$010000CB`, CPU walks
off into `hard_excep`.

Three strategic options on the table — pick one for next session:

1. **Populate POP's env_save_area at bootrom_build time** with a safe
   resume PC. Apple doesn't do this, so we have to. Possible safe PC:
   an `ExitSys` trampoline, or the current INITSYS PC snapshot at the
   moment of each interrupt entry. Structurally clean but requires
   hooking the interrupt entry path to snapshot PC before dispatching.

2. **Defer INTSON until SYS_PROC_INIT has run**. Without interrupts, no
   timer IRQ, no Scheduler dispatch. Boot proceeds sequentially through
   INITSYS → SYS_PROC_INIT → create real processes → INTSON can safely
   fire. Find the INTSON call site in BOOT_IO_INIT (or the `INTSON` HLE
   in src/m68k.c around line 5844) and gate it on `boot_progress_reached
   ("SYS_PROC_INIT")`. This matches Apple's implicit assumption (timer
   shouldn't dispatch before there are real processes).

3. **HLE-intercept the interrupt entry** itself — detect "this is a
   timer IRQ on POP" and just RTE immediately without calling Scheduler.
   Fakes a "no-op timer tick" until SYS_PROC_INIT completes. More
   surgical than option 2 but needs care to not drop legitimate IRQs.

Option 2 is the cleanest structural answer; option 3 is the narrowest
change.

**Open question:** is POP's env_save_area supposed to be populated by
some boot path we've skipped or disabled? Worth grepping for any write
pattern `sloc_ptr^.env_save_area := ...` or direct address writes to
`$CE0006` at boot-ROM-init time. If Apple did expect it populated
elsewhere, fix that upstream; otherwise option 1 or 2 stands.

## Pick up here

Paste this to the next session:

> Continue the LisaEmu kernel-boot investigation from NEXT_SESSION.md. Six
> structural Pascal-codegen / linker / infra fixes landed and pushed last
> session (5b2f848..73eb34c, peaking with P89g's even-align-LINK). Boot
> reaches 22/27 milestones, halts at `hard_excep` → `SYSTEM_ERROR(10204)`
> because the timer IRQ's Scheduler → Launch fires on POP (the
> pseudo-outer-process running INITSYS) whose `env_save_area` at
> `b_syslocal_ptr+6` = `$CE0006` was never populated (reads `$010000CB`
> instead of a valid PC). Apple doesn't call `CreateProcess` for POP so
> its env_save_area has no legit writer.
>
> Pick option 2 from NEXT_SESSION.md: defer the first `INTSON` until
> `boot_progress_reached("SYS_PROC_INIT")` returns true. Find the INTSON
> HLE site in `src/m68k.c` (around line 5844 or the kernel's INTSON at
> `$06DA6A`) and wrap it with the guard. Once SYS_PROC_INIT has run and
> created real system processes with populated env_save_areas, let
> interrupts fire normally. If that works, SYS_PROC_INIT reaches →
> INIT_DRIVER_SPACE → FS_CLEANUP → MEM_CLEANUP → PR_CLEANUP should all
> follow naturally and we'll land at 27/27.
>
> If INTSON deferral causes a new cascade, back off and try option 1
> (populate POP's env_save_area at bootrom time) or option 3 (intercept
> the timer IRQ at the vector, not the Scheduler call).
