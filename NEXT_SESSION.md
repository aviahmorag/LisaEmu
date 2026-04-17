# LisaEmu — Next Session Handoff (2026-04-17 late)

## Accomplished this session

- **P85a codegen fix** (committed earlier, `0844b5d`) — AST_IDENT_EXPR
  byte loads zero-extend D0 via `MOVEQ #0,D0` before `MOVE.B`. Fixed
  a stale-high-byte bug in MAP_SEGMENT that left MMU seg 60 unmapped
  and caused the FlushNodes buffer-pool spin.

- **Diagnostic infrastructure** (committed `833f727`) — QUEUE_PR
  entry probe + all other P85 probes. Confirmed:
  - FlushNodes now works (buffer pool circular list verified).
  - QUEUE_PR called from Signal_sem at $01118E.
  - PCB byte-layout read as WORD at offset 12 gives $FF00 (signed
    -256), breaking BLE priority compare in RQSCAN.

- **Diagnosis for the next blocker nailed down.** Boot reaches
  scheduler. Signal_sem calls `Queue_Process(c_pcb, Ready)` after
  `priority := norm_pri`. QUEUE_PR's RQSCAN does
  `CMP PRIORITY(A0),D1; BLE.S RQSCAN`. PRIORITY .EQU 12 = word read.
  With our P82c tight byte packing, priority byte at @12 + norm_pri
  byte at @13 reads as $FF00 word — negative signed. BLE never exits,
  RQSCAN spins at PC=$06EB46 forever (hot page $06EB00 x 277k).

## In progress / blocked

Everything committed, nothing mid-flight. Build + audit green.
22/27 milestones. 300-frame headless run "clean" (no halt) but
CPU spins forever in QUEUE_PR.

## The real fix (what was tried and why it didn't stick)

**Hypothesis**: Apple's Pascal widens byte-subranges in records to
2 bytes, storing the value in the LOW byte (offset+1). So priority
= 255 stored as $00FF at offset 12-13 → MOVE.W 12(A),D1 reads $00FF,
positive signed. Pair-packing (P82c) should only apply when the
second byte is a VARIANT-TAG enum (codesdb's lockcount+sdbtype).

**Attempted fix (P85b)**: in both the AST record resolver
(`pascal_codegen.c:~421`) and the pre-pass fixup
(`toolchain_bridge.c:~845`):
1. Restrict pair-packing to `nft->kind == TK_ENUM` (variant tag).
2. For widened byte-subrange fields, set field offset to
   `offset + 1` (low byte of word), padding high byte.

**Result**: QUEUE_PR's word@12 now reads $00FF (correct!), boot
advances past scheduler, but breaks further down — SYSTEM_ERROR
10707 (FS couldn't init), 10701 (no sys space), and 14 UNMAPPED
writes to seg 32 from RELSPACE/GETSPACE. Reverted.

The cascading failures are because many records besides PCB have
byte-byte pairs that were relying on tight packing. Changing the
layout convention grows records and changes offsets — which
obviously doesn't agree with all the ASM code that hardcodes
offsets via `.EQU`.

**Two possible paths forward**:

A. **Narrow the widening to PCB specifically**, or to records that
   are accessed by ASM code via MOVE.W word reads. Hard to detect
   at codegen time. One pragmatic proxy: any record with fields
   whose types match PASCALDEFS symbols (priority, domain, etc.).

B. **Fix the reader side instead of the writer side**. Patch our
   generated code for `CMP PRIORITY(A),D1` specifically (in
   PROCASM.TEXT assembly) to sign-agnostic unsigned compare. The
   asm source uses `BLE.S` (signed ≤). If priority values are
   always 0..255, swap to `BLS.S` (unsigned ≤). Then $FF00
   compared as unsigned to $0000 = 65280 > 0, BLS fails → exit
   scan. But this changes Apple's ASM source — invasive.

C. **Widen only the leaf fields QUEUE_PR and Sched code actually
   read as words**: priority and norm_pri in PCB. Don't touch
   any other records. Do this by detecting the *record name* and
   *field name* at layout time — hard-coded.

Options A/C are hacks, B is invasive. Option A restricted by
"only fields whose ASM offsets appear in PASCALDEFS" might be
principled enough to try.

## Pick up here

> Read NEXT_SESSION.md. Last session fixed FlushNodes (P85a) and
> diagnosed the next blocker: QUEUE_PR spins in RQSCAN because
> our P82c tight byte-pair packing puts PCB.priority byte at @12
> where Apple's ASM reads it as a word — value ends up in HIGH
> byte, $FF00 = -256 signed, BLE never exits. The fix is to widen
> priority/norm_pri (and similar byte-subrange pairs in non-variant
> records) to 2-byte words with value in LOW byte (offset+1). The
> attempted P85b widening change has the right idea but cascades
> into FS init failures because OTHER records are affected. Next
> session needs a narrower approach — likely restrict widening
> to records with fields whose names appear in PASCALDEFS (priority,
> domain, etc.), or only PCB specifically. Build: `make && make audit`
> (both green). Run: `./build/lisaemu --headless Lisa_Source 300 >
> .claude-tmp/run.log 2>&1`. Current state: 22/27 milestones, 300
> frames clean but spinning in QUEUE_PR at PC=$06EB46.

## Flagged from prior handoffs (still applicable)

- **Apple's MERGE_FREE lacks our P83a guard** — unresolved mystery.
- **exit() codegen**: only `exit(CurrentProc)` handled.
- **P84a narrow scope**: only AST_FIELD_ACCESS NOT.
- **P84b narrow scope**: only param byte reads with offset > 0.
- **P85a may have exposed latent bugs**: the QUEUE_PR crash itself
  is an exposure-effect — we just didn't see it before because
  priority/norm_pri reads were getting stale garbage that happened
  to make things work (sometimes).
