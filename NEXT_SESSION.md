# LisaEmu — Next Session Handoff (2026-04-17 night)

## Accomplished this session

- **P85a — AST_IDENT_EXPR byte loads zero-extend D0** (commit `0844b5d`,
  `src/toolchain/pascal_codegen.c:1704 + :1732`). The prior session's
  P80h2 fix applied MOVEQ #0,D0 to `emit_read_a0_to_d0` but missed the
  AST_IDENT_EXPR direct A6/A0/A5 byte-load paths. `MOVE.B offset(A6),D0`
  only updates the low byte; subsequent `MOVE.W D0,Dn` propagated the
  stale high byte. In MAP_SEGMENT, `l_domain := domain` (domainRange
  byte-param) produced l_domain=$B600, which then made PROG_MMU call
  TRAP #6 with tar_domain=$B600, SMT lookup walked into garbage, the
  MMU segment for MR-data buffer (seg 60, $00780000..$0079FFFF) never
  got programmed, and InitBufPool's writes to the doubly-linked buffer
  list all got dropped by the MMU unmapped-write safety net.

  **Fix**: prepend `MOVEQ #0,D0` before `MOVE.B (A6)/(A0)/(A5),D0`
  when sz==1 in the AST_IDENT_EXPR load path. Applied to the
  outer-scope (A0 via static link), local/param (A6), and global (A5)
  sub-branches at pascal_codegen.c:1706-1738.

- **Effect on boot**: the prior "FlushNodes 188k-iter spin" is gone.
  The buffer pool circular doubly-linked list is actually initialized
  now — probe walk confirms: $78189C → $781068 → $780834 → $780000
  → loops back to $78189C after 4 hops, with link.f and link.b both
  populated. FS_INIT still fires. 22/27 milestones.

- **P85 diagnostic probes** (src/m68k.c, committed). Probes for
  FlushNodes / InitBufPool / InitBuf / MAP_SEGMENT / PROG_MMU entries
  + post-entry buffer-pool walk starting from ptrHot at A5-$2066.
  PROG_MMU probe reads L-to-R (it's an external asm proc, and
  the Pascal compiler pushes args for external procs in declaration
  order — opposite of the right-to-left convention our Pascal bodies
  use). MAP_SEGMENT args are R-to-L (Pascal body). This asymmetry
  matters for future probes.

## In progress

No code is mid-flight. Everything committed and pushed.

## Blocked / open questions

### New blocker: SYSTEM_ERROR(10204) in QUEUE_PR

**10204 = e_flinesyscode** — F-line trap occurred in system code.
Boot now progresses past FlushNodes into scheduler, then crashes.

Chain from the run log:
```
[P85] FlushNodes#1 ... ptrHot=$0078189C ptrCold=$00780000
       *** looped back to ptrHot after 4 hops ***
UNMAPPED-WRITE[1]: PC=$06EB02 log=$2F0002 phys=$2F0002 val=$3F (seg=23) dropped
UNMAPPED-WRITE[2]: PC=$06EB02 log=$2F0003 ... val=$00
UNMAPPED-WRITE[3]: PC=$06EB02 log=$2F0004 ... val=$4E
UNMAPPED-WRITE[4]: PC=$06EB02 log=$2F0005 ... val=$B9
CRASH TO VECTORS: prev_pc=$06EB20 → pc=$00001F opcode=$4ED0 A7=$00CBFE67
[VEC-FIRST] v=11 PC=$000027 SR=$2108 SSP=$00CBFE67 USP=$00F7FD14 handler=$00069AB4
HLE SYSTEM_ERROR(10204) at ret=$069AE6 ...
SYSTEM_ERROR(10204): HALTING CPU
```

QUEUE_PR ($06EAF4, source-starasm1.text asm) manipulates a process
ready-queue:
```
$06EAF4: MOVE.L (A7)+,D0    ; pop return address
$06EAF6: MOVE.B (A7)+,D1    ; pop flag byte
$06EAF8: MOVEA.L (A7)+,A1   ; pop PCB pointer
$06EAFA: MOVEA.L (A1),A0    ; A0 = PCB.link.f (= $2EFFFE, garbage)
$06EAFC: MOVEA.L 4(A1),A2   ; A2 = PCB.link.b
$06EB00: MOVE.L A0,(A2)
$06EB02: MOVE.L A2,4(A0)    ; ← unmapped write ($2F0002)
...
$06EB1E: MOVEA.L D0,A0; JMP (A0)   ; return via D0 = $00001F
```

Two smoking guns:
1. A0 ends up as $00001F. D0 was popped as the return address from
   the stack at entry. Something pushed $00001F as the "return PC"
   before calling QUEUE_PR. QUEUE_PR is called with a hand-coded
   calling convention (args pushed before return-addr — return-addr
   pops FIRST into D0 — this is asm-style). The caller is Pascal
   code that needs to know this convention.

2. `(A1)` read as $2EFFFE — garbage pointer for PCB.link.f. And the
   writes at $06EB02 write "$3F 00 4E B9" to $2F0002 — those bytes
   decode as `MOVE.W D0,-(A7)` + `JSR.L` — **instruction bytes, not
   data**. This suggests the PCB `link.f` field points at something
   that isn't a PCB at all — the whole ready-queue linkage is wrong.

**Concrete next steps** (prioritized):

1. Probe QUEUE_PR entry — log SP, D0 (popped return addr), D1 (flag),
   A1 (PCB) + walk A1's fields. Find the caller (will be some
   JSR/BSR in Pascal scheduler or HLE code).

2. Check what's at $2EFFFE / near it — segment 23 is unmapped, so
   why does any code think it has a PCB there? Probably a wild
   pointer from another codegen bug.

3. Grep for QUEUE_PR callers in our emulator's HLEs. CreateProcess
   HLE or FinishCreate HLE might be pushing PCBs with corrupt
   link fields — check `src/m68k.c` around `CreateProcess` +
   `FinishCreate` HLE (P80g/h2 era). Possibly the link fields
   they populate are at the wrong offset now that our record
   layout has changed (post-P82b CASE tag, P82c byte packing).

4. If 2 & 3 look clean, pursue the D0 = $00001F mystery separately:
   $1F = 31. Looks like a scheduler-priority or an array index
   accidentally treated as a PC. Maybe a SCHEDDIS/SCHEDENA op
   mis-popped the stack.

### Flagged from prior handoffs (still applicable)

- **Apple's MERGE_FREE lacks our P83a guard** — mystery unresolved.
  Unlikely Apple hardware hit this without crashing.

- **exit() codegen is still parked** at "just `exit(CurrentProc)`
  works". `exit(EnclosingProc)` still walks too far.

- **P82b CASE tag risk** — watch PCB-like records for regressions.

- **P84a has narrow scope** — only AST_FIELD_ACCESS NOT. AST_DEREF
  of ptr-to-bool / AST_ARRAY_ACCESS of bool array still emit
  bitwise NOT.W. May bite later.

- **P84b has narrow scope** — only param byte reads with offset > 0.
  VAR-param deref byte reads and WITH-based byte FIELD_ACCESS not
  covered (though P85a might cover most of them by zero-extending).

### New risks from P85a

- **Zero-extend may have exposed latent bugs.** Previously, some
  code paths may have happened to compute the right value *because*
  of stale high bits masking a sign or acting as a sentinel. Watch
  for regressions in paths we thought worked. 22/27 is the same as
  before, so no visible regression yet, but the QUEUE_PR crash may
  itself be an exposure-effect.

- **We did NOT add the same fix** to other AST node types that
  also emit MOVE.B directly: AST_FIELD_ACCESS, AST_DEREF,
  AST_ARRAY_ACCESS. Grep `0x102E\|0x1028\|0x102D\|emit16(cg, 0x10`
  in pascal_codegen.c to find them and decide whether similar
  fixes are warranted.

## Pick up here

> Read NEXT_SESSION.md. The prior session's FlushNodes spin is
> fixed — P85a zero-extends D0 for byte-subrange loads in
> AST_IDENT_EXPR's A6/A0/A5 load paths. MMU seg 60 now
> programs correctly and the buffer pool circular list
> initializes. Boot advances past FS_INIT into scheduler
> and halts with SYSTEM_ERROR(10204) = f-line in system
> code at QUEUE_PR (scheduler ready-queue insertion) in
> source-starasm1.text.unix.txt. PC=$06EB20 executes JMP (A0)
> with A0=$00001F — it treated the popped stack value as a
> return address but got $00001F instead. Also the PCB's
> link.f read as $2EFFFE (wild pointer). Start by probing
> QUEUE_PR entry + walking the stack to find the caller and
> PCB source. Check CreateProcess/FinishCreate HLEs in m68k.c.
>
> Build: `make && make audit` (both green).
> Canonical run: `./build/lisaemu --headless Lisa_Source 300 > .claude-tmp/run.log 2>&1`.
> Currently 22/27 milestones. Boot halts via SYSTEM_ERROR,
> not via spin.
