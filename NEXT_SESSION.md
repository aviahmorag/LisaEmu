# LisaEmu — Next Session Handoff (2026-04-17 PM — P83a landed)

## TL;DR

The `SYSTEM_ERROR(10598)` TAKE_FREE halt from last session is
**averted** via a new HLE guard on `MERGE_FREE`. Build + audit green,
still 22/27 milestones. Boot reaches FS_INIT then enters a new
**infinite CHECK_DS/INIT_SWAPIN loop** on a zero-initialized sdb at
`$CCBA62` — that's the next investigation target.

Uncommitted work: HLE guard + CHECK_DS probe in `src/m68k.c`. Decide
whether to commit as-is or revise.

## What P83a did

`MERGE_FREE(left_sdb)` inside INSERTSDB's free-chain insert path is
called with `left_sdb = head_sdb` the very first time MAKE_FREE runs
(free chain empty → the predecessor walk lands on head_sdb). Apple's
code has no guard; the merge body evaluates its condition
successfully and then scribbles `head.memsize` with the inserted
free region's size, removing that region from both chains. Result:
free chain empty, head.memsize non-zero → GetFree later walks
`tail.freechain.bkwd - 14 = head_sdb`, proceeds past the
"memsize < size" check, calls `TAKE_FREE(head_sdb, true)` which
asserts `sdbtype <> free` → SYSTEM_ERROR(10598).

**Guard**: HLE on MERGE_FREE entry — if `c_sdb_ptr.sdbtype != free`,
return immediately. Defensively correct per Apple's own comment
("merge two adjacent free regions") — it only makes sense when
c_sdb_ptr is actually a free region. Code in `src/m68k.c` near the
`P41` boot_progress_lookup cache block; look for `/* P83a HLE guard`.

After P83a: no more SYSTEM_ERROR halt; free chain stays populated
([head → $B35600 → tail]), head.memsize stays 0. GetFree / TAKE_FREE
now behave consistently.

## New blocker — CHECK_DS/INIT_SWAPIN spin on zero sdb at $CCBA62

```
[P83b] CHECK_DS#1 entry ret=$01093C c_sdb_ptr=$00CCBA62
      c_sdb: memaddr=$0000 memsize=$0000 sdbtype=$00 sdbstate[0]=$00 newlength=$0000
```

CHECK_DS's `while not sdbstate.memoryF or (newlength <> 0)` spins
because memoryF=0, newlength=0 → `not false or false = true`. Inside
the body, `init_in_progress=true` → `INIT_SWAPIN(c_sdb_ptr)` →
`GET_SEG(error, c_sdb_ptr)`. GET_SEG returns without setting
memoryF (presumably because memsize=0 = nothing to load), the loop
repeats forever.

Hot pages after 300 frames: `$045E00×188100` (INIT_SWAPIN region),
`$043C00×75240` (CHECK_DS), `$043D00×37620`. No new milestones fire.

`ret=$01093C` → the caller is in STARTUP or INIT-like code around
that address. Likely passes an sdb pointer that *should* have been
populated by an earlier MAKE_REGION / MAKE_DATA / GETSPACE call but
wasn't. Our guard may have altered an upstream side-effect, or
there's an independent init gap.

### Concrete next steps

1. **Identify the caller and what sdb `$CCBA62` is supposed to be.**
   Look at PC `$01093C` — what proc is that, what does it do before
   calling CHECK_DS. The ret_pc is well past INIT_EC ($01E374) and
   INIT_EM ($0010A8), closer to INIT_PROCESS ($000B40). Map the
   symbol range.
   ```
   grep -E '\$010[0-9A-F]' build/lisa_linked.map | sort
   ```

2. **Trace upstream**: what CREATES the sdb at $CCBA62, and does it
   fill in memaddr/memsize/sdbtype? If it's a MAKE_REGION with
   link_sdb=false, the sdb might be a local struct never persisted
   properly. If it's from BLD_SEG with a 0-size code/data segment,
   the init sequence has a gap.

3. **Consider reverting the P83a guard temporarily** to confirm the
   CHECK_DS loop is a genuinely new blocker (exposed by the guard)
   and not a regression. Git diff should show only m68k.c changes.

4. **Maybe GET_SEG has a bail-out for memsize=0** that skips setting
   memoryF; that'd be the natural place to patch if we determine
   this is an edge case.

### Diagnostic state (probes still live)

- P83a HLE guard on MERGE_FREE (prints up to 4 skip messages per run).
- P83b CHECK_DS entry probe (prints up to 3 entry dumps per run).

Both are in `src/m68k.c` near the P41 boot_progress_lookup cache
block. Low overhead, safe to keep or delete.

## Open questions / unresolved

1. **Why Apple's boot doesn't hit the MERGE_FREE-scribbles-head bug.**
   Apple's source has no guard; our disassembly of MERGE_FREE shows
   correct offsets and 32-bit compare; the observed state matches
   exactly what Apple's code would produce. Either Apple's real
   hardware runs into the same issue and we haven't noticed the
   visible symptom (there wasn't one on hardware? unlikely), or
   Apple's compiler emits materially different code for variant-
   field access under WITH. Left unresolved; our guard is semantically
   correct regardless.

2. **Whether the CHECK_DS loop is pre-existing or newly exposed.**
   Baseline (pre-guard) parks at $005E60 after SYSTEM_ERROR HALT with
   no CHECK_DS activity in hot pages. Post-guard, CHECK_DS dominates.
   So yes, newly exposed by the guard.

## Build / run commands (unchanged)

```
make                                                        # build
make audit                                                  # link audit
./build/lisaemu --headless Lisa_Source 300 > .claude-tmp/run.log 2>&1
grep -E "Milestones reached|SYSTEM_ERROR|HALT|P83" .claude-tmp/run.log
```

## Pick up here

> Read NEXT_SESSION.md. P83a HLE guard on MERGE_FREE fixed the
> SYSTEM_ERROR(10598) TAKE_FREE halt by preventing scribble into
> head_sdb.memsize. Boot now reaches FS_INIT then stalls in a
> CHECK_DS infinite loop on a zero-initialized sdb at $CCBA62
> (called from ret=$01093C). No new milestones fire. Start by
> mapping what symbol owns $01093C, tracing backward to see who
> constructed the sdb at $CCBA62, and whether it should have had
> memaddr/memsize populated. Also consider whether to commit the
> P83a guard as-is or find the "real" reason Apple's boot doesn't
> need it.
>
> Build: `make && make audit` (both green). Canonical run:
> `./build/lisaemu --headless Lisa_Source 300 > .claude-tmp/run.log 2>&1`.
> Currently 22/27 milestones, boot stalls in CHECK_DS loop (no halt,
> no SYSTEM_ERROR, no new milestones past FS_INIT).
