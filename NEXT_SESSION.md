# LisaEmu — Next Session Handoff (2026-04-17 evening, P82 trio landed)

## TL;DR

Three codegen fixes landed today that eliminated two crash classes:

- **P82** (`3ad488c`): variant records + chained AST_FIELD_ACCESS
  width/offset fixes. Killed the UNMAPPED-WRITE at `$F23204` from
  `P_DEQUEUE`.
- **P82b/c** (`1571efa`): parser emits `CASE` tag as a field, enums
  default to 1 byte, byte-pair fields stay packed. Killed the
  CLEAR_SPACE infinite loop.

Build + audit green. Boot still stops at 22/27 milestones, now with
a **legitimate kernel assertion** `SYSTEM_ERROR(10598)` ("TAKE_FREE
called on sdb whose sdbtype <> free"). That's the new investigation
target.

## What to do next

The free chain is empty at FS_INIT time:
```
head_sdb=$CCB034 fwd_link=$00CCB5CE bkwd_link=$00CCB062 memsize=$0CD5 sdbtype=$05
head_sdb.freechain.fwd_link=$00CCB070 bkwd_link=$00CCB070
first_free_sdb=$CCB062 (= tail_sdb) memsize=$0000 sdbtype=$05
```

`head_sdb.memsize=$0CD5` is the smoking gun — that's the exact
`msize` MAKE_FREE was called with. Either MAKE_FREE stomped on
head_sdb, or INSERTSDB linked the new sdb in a place that overlaps
with head_sdb.

### Concrete next step

1. **Probe MAKE_FREE entry + exit** (src/m68k.c — see deleted probe
   code in the P82c commit for the pattern). Dump:
   - `maddr`, `msize` (args)
   - `new_sdb = ord4(maddr)*hmempgsize*2 + logrealmem` — compute
     and verify it doesn't land inside the MMRB at $CCB00A–$CCB0F0.
   - Freechain head/tail before and after the INSERTSDB call.

2. **Disassemble MAKE_FREE and INSERTSDB** around the store sites.
   Verify:
   - `new_sdb^.memsize := msize` writes `MOVE.W D0, 10(A0)` where A0
     = new_sdb (not head_sdb).
   - `sdbtype := free` writes `MOVE.B #0, 13(A0)` (P82b layout: 1
     byte at offset 13).
   - `INSERTSDB`'s P_ENQUEUE writes memchain.fwd_link at offset 0
     and bkwd_link at offset 4 of the sdb (4 bytes each, post-P82).

3. **Check `logrealmem`**. It's a global constant — if our codegen
   resolved it wrong (e.g., 0 instead of the real Lisa memory base),
   `new_sdb` lands at `maddr*512` which could collide with MMRB.
   Search for `logrealmem` in Lisa_Source/.

4. **Verify tail_sdb init.** In P82c, the freechain's circular-empty
   state should be head.freechain.fwd_link=@tail.freechain and
   tail.freechain.bkwd_link=@head.freechain. We see head.fwd=$CCB070
   which is MMRB+$66 = tail_sdb.freechain addr. Good. But if
   head.freechain was initialized ONCE at MM_INIT time and then
   MAKE_FREE didn't insert into it, the chain stays empty.

### Build / run commands (unchanged)

```
make                                                        # build
make audit                                                  # link audit
./build/lisaemu --headless Lisa_Source 200 > .claude-tmp/run.log 2>&1
grep -E "Milestones reached|reached|SYSTEM_ERROR|HALT" .claude-tmp/run.log
```

## Blocked / open questions

1. **Deeper `exit()` fix still parked** (from prior handoff) — only
   handles `exit(CurrentProc)`. `exit(EnclosingProc)` still walks
   too far. Not blocking; flagged.

2. **Other CASE-tag records.** My P82b fix applies universally —
   any record with `CASE IDENT : TYPE OF` now has the tag emitted.
   This may shift layouts for PCB / PCB-like records elsewhere,
   possibly breaking asm code that hard-coded offsets. Watch for
   new regressions in process-management paths.

3. **The `16 < hole_memaddr` branch of CLEAR_SPACE.** We only saw
   the toright branch exercised. If the system winds up in toleft,
   there might be a different assertion path.

## Pick up here

> Read NEXT_SESSION.md. P82/b/c codegen trio is in — crashes and
> infinite loop are gone. Boot halts at SYSTEM_ERROR(10598) inside
> TAKE_FREE because the freechain is empty at FS_INIT time and
> head_sdb.memsize has been corrupted with MAKE_FREE's msize arg
> ($0CD5). Start by re-adding the MAKE_FREE/INSERTSDB probes
> (dump new_sdb address, freechain before/after) and disassembling
> MAKE_FREE to verify the `memsize := msize` store lands at the
> right offset and doesn't hit head_sdb.
>
> Build: `make && make audit` (both green). Canonical run:
> `./build/lisaemu --headless Lisa_Source 200 > .claude-tmp/run.log 2>&1`.
> Currently 22/27 milestones with clean HALT at SYSTEM_ERROR(10598).
