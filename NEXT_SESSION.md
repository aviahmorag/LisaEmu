# LisaEmu — Next Session Handoff (2026-04-17 PM wrap-up)

## Accomplished this session

- **P83a HLE guard on MERGE_FREE** (`src/m68k.c` ~line 2986): skip
  MERGE_FREE body when `c_sdb_ptr.sdbtype != free`. Apple's source
  lacks a guard against being called on the head_sdb sentinel; the
  merge body scribbles `head.memsize` with the inserted free sdb's
  size (via `memsize := memsize + right_sdb^.memsize` inside
  `with head_sdb^ do`), then TAKE_FREE removes the just-inserted free
  sdb. Result: empty free chain + head.memsize non-zero → downstream
  GetFree finds head as "last free", TAKE_FREE asserts SYSTEM_ERROR(10598).
  Guard averts the halt. Defensively correct per Apple's own
  documented intent ("merge two adjacent free regions").

- **P83b probe on CHECK_DS** (`src/m68k.c` just below P83a guard):
  logs first 3 entries with c_sdb_ptr contents so the next session
  can immediately see which sdb is stuck in the swap-in loop.

- **CLAUDE.md** refreshed with P83a status + new blocker description
  replacing the old "SYSTEM_ERROR(10598)" section.

- **NEXT_SESSION.md** (this file) rewritten for the new blocker.

All build + audit still green (22/27 milestones). The `SYSTEM_ERROR(10598)`
halt from last session is gone.

## In progress

No code is mid-flight. Everything committed in `540fa7e` is in a
consistent state; the guard and probe are debug-ready but functional.

## Blocked / open questions

### New blocker: CHECK_DS/INIT_SWAPIN infinite loop on zero sdb at $CCBA62

```
[P83b] CHECK_DS#1 entry ret=$01093C c_sdb_ptr=$00CCBA62
      c_sdb: memaddr=$0000 memsize=$0000 sdbtype=$00 sdbstate[0]=$00 newlength=$0000
```

CHECK_DS's `while not sdbstate.memoryF or (newlength <> 0)` spins
forever because memoryF=0, newlength=0. In the body,
`init_in_progress=true` → `INIT_SWAPIN(c_sdb_ptr)` →
`GET_SEG(error, c_sdb_ptr)`. GET_SEG returns without setting memoryF
(presumably because memsize=0), loop repeats. Hot pages after 300
frames: `$045E00×188100` (INIT_SWAPIN), `$043C00×75240` (CHECK_DS),
`$043D00×37620`. No new milestones fire, no HALT — just spin.

**Concrete next steps** (prioritized):

1. Map `ret=$01093C` to a symbol and procedure context:
   ```bash
   grep -E '\$010[0-9A-F]' build/lisa_linked.map | sort
   ```
   The PC is well past INIT_PROCESS ($000B40) / INIT_EM ($0010A8) /
   INIT_SCTAB ($002C6A), so likely INIT_EC ($01E374) or something in
   between. Identify which proc owns $01093C and what sdb it's
   passing.

2. Trace upstream — what CREATES the sdb at $CCBA62. `$CCBA62` is
   inside the MMRB region (sysglobal pool area). Read bytes around
   it to see if it's a legitimately-reserved sdb or stale zeros.
   Compare to known MMRB sdb offsets (head_sdb at $CCB034, tail_sdb
   at $CCB062, etc.).

3. Consider: does `GET_SEG` have a bail-out for memsize=0 that skips
   setting memoryF? Check `source-MMPRIM2.TEXT.unix.txt` for GET_SEG.
   If yes, the patch could be to make CHECK_DS detect an uninitialized
   sdb and skip; if no, find who should have initialized the sdb.

### Open question — why Apple's shipped source doesn't need the P83a guard

Apple's MERGE_FREE has no guard. Our disassembly of MERGE_FREE shows
correct offsets (sdbtype byte@13, freechain@14, memsize word@10) and
32-bit compare. The observed runtime state matches exactly what
Apple's source would produce semantically. Either Apple's real hardware
DID hit this and nobody noticed the symptom, or Apple's compiler emits
materially different code for variant-field access under WITH that
avoids triggering the merge. Left unresolved; P83a is semantically
correct regardless.

### Flagged from prior handoffs (still applicable)

- **Deeper `exit()` fix still parked** — only handles `exit(CurrentProc)`.
  `exit(EnclosingProc)` still walks too far.
- **P82b CASE tag shift risk** — any record with `CASE IDENT : TYPE OF`
  now has the tag emitted as a field. Watch for new regressions in
  process-management paths using PCB-like records.

## Pick up here

> Read NEXT_SESSION.md. P83a HLE guard on MERGE_FREE is in `src/m68k.c`
> and fixed the SYSTEM_ERROR(10598) TAKE_FREE halt. Boot now reaches
> FS_INIT then stalls in an infinite CHECK_DS/INIT_SWAPIN loop on a
> zero-initialized sdb at `$00CCBA62` (called from `ret=$01093C`).
> No new milestones fire. Start by running
> `grep -E '\$010[0-9A-F]' build/lisa_linked.map | sort` to find what
> symbol owns `$01093C`, then trace upstream to see who should have
> populated the sdb at `$CCBA62` (memaddr/memsize/sdbtype are all
> zero). Also look at `source-MMPRIM2.TEXT.unix.txt` for GET_SEG to
> see why it returns without setting memoryF for a zero-size sdb.
>
> Build: `make && make audit` (both green).
> Canonical run: `./build/lisaemu --headless Lisa_Source 300 > .claude-tmp/run.log 2>&1`.
> Currently 22/27 milestones. Stall is via spin, not HALT — the run
> completes cleanly after 300 frames without SYSTEM_ERROR.
