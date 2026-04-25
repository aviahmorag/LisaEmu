# Next Session Handoff — 2026-04-25 (P128m diagnosis)

## Accomplished this session

**Root-caused the post-P128l MemMgr REMAP_SEGMENT busy-loop to a Pascal
codegen bug for the boolean literal `false`.**

The symptom: 27/27 milestones reach naturally, no SYSTEM_ERROR, but
post-PR_CLEANUP MemMgr enters CLEAR_SPACE → MOVE_SEG → REMAP_SEGMENT and
spins indefinitely (~144K hits per DIAG frame at PC $00FD00 inside
REMAP_SEGMENT's body).

The cause: `source-MM2.TEXT:595` has

```pascal
with c_mmrb^.clock_ptr^ do CLEAR_SPACE(memaddr+memsize, space_needed, false);
```

The third arg is the literal `false`. Our codegen at the call site
(PC $04A930 in the linked binary) emits `MOVEQ #1,D0` ($7001) for that
arg — i.e., **pushes TRUE for FALSE**. CLEAR_SPACE receives
`force_to_hole_memaddr = 1`, and the outer while-loop becomes

```
while (free_sdb^.memsize < space_needed) or
      (force AND ((free_start > hole_memaddr) OR (free_end < hole_memaddr))) do
```

With force=TRUE and the freechain head always at memaddr > hole_memaddr
($0453), the second OR clause is forever TRUE. MOVE_SEG iterations
shuffle segments without making progress toward a hole at $0453, and
the loop never exits.

Memory entry: `project_clear_space_force_codegen_bug.md`, indexed in
`MEMORY.md`.

## Diagnostic chain (for next session's reference)

1. Bumped P128l probe limits (`src/m68k.c`):
   - `p128l_cis_count < 5` → `< 50` (CLR_INMOTION_SEG entry)
   - `p128d_rs_count < 6` → `< 50` (REMAP_SEGMENT entry)

2. Added P128m probes (`src/m68k.c`):
   - **CLEAR_SPACE entry probe** at `0x0493F8` — dumps hole_ma,
     space_needed, force, mmrb, head_sdb fields, freechain.fwd, etc.
   - **MOVE_SEG entry probe** at `0x049186` — dumps caller's A6 frame
     to extract CLEAR_SPACE locals (free_sdb, next_sdb, moving_sdb,
     free_start, free_end, c_mmrb, space_gained); dereferences free_sdb
     and moving_sdb to show memaddr/memsize/sdbtype.
   - **One-shot disasm-byte dumps** (gated `#if 0`) — toggle to view
     CLEAR_SPACE body and call-site bytes for offline analysis.

3. Extended P128l MMRB watchpoint (`src/lisa_mmu.c`) to also watch
   MMRB+56..+63 (head_sdb.freechain.fwd_link/bkwd_link) — saw P_ENQUEUE/
   P_DEQUEUE writing realmem-range pointers (which is correct behavior:
   free sdbs live AT their memory location, not in sysglobal).

4. **Critical probe-byte fix:** my initial CLEAR_SPACE entry probe read
   `force` from the HIGH byte of the 2-byte arg slot (callee-clean asm
   convention). CLEAR_SPACE is called Pascal-Pascal (caller-clean), so
   the byte is in the LOW byte. After fixing the read, force showed as
   1 (TRUE) — and that pointed straight at the codegen bug.

   The probe is now `force = force_w & 0xFF` at `src/m68k.c` ~line 4070.

5. Verified at the linked-binary level: bytes at $04A930..$04A934 read
   `7001 3F00` = MOVEQ #1,D0; MOVE.W D0,-(SP). For literal `false`,
   should be `7000 3F00`.

## In progress

Probes still active in the build. They fire at known addresses and
have low budgets, so they won't spam logs. They're useful for the next
session's codegen investigation and shouldn't be removed prematurely
(per `feedback_hle_layers_load_bearing.md` — these are diagnostic
scaffolds for an active bug).

## Blocked / next blocker — Pascal `false` literal codegen miscompile

The codegen at `src/toolchain/pascal_codegen.c:2429-2455` looks correct:

```c
if (str_eq_nocase(node->name, "true")) {
    emit16(cg, 0x7001);  /* MOVEQ #1,D0 */
} else if (str_eq_nocase(node->name, "false")) {
    emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
}
```

But the actual binary emits `7001` for `false` at the MM2.TEXT:595
call site. Possible explanations to investigate:

1. The AST node for `false` isn't reaching this branch. Check if it's
   being parsed as a different AST kind (e.g., AST_BOOL_LIT separate
   from AST_IDENT). Add a printf at both branches to confirm which
   path fires for the MM2.TEXT:595 call.

2. The WITH-record context (`with c_mmrb^.clock_ptr^ do`) is
   shadowing `false` with a field lookup. Check if any sdb field
   coincidentally matches `false` (segstates' field names are memoryF,
   motionF, deleteF, pcb_wF, swaperrF, overlayF, releaseF, res1 — no
   bare `false`, but maybe the lookup is fuzzy).

3. There's an implicit boolean inversion happening somewhere in the
   call-arg push path (e.g., for `byte_param` coercion).

4. `false` is being parsed as a TK_IDENT and looked up as a global
   symbol with value 1. Check the symbol table for any `false`
   entry.

### Next-session opener

```
Picking up from P128m. Diagnosis is complete — we know the structural
bug: literal `false` at source-MM2.TEXT:595 compiles to MOVEQ #1
(TRUE) at PC $04A930 in the linked binary, making CLEAR_SPACE's
force_to_hole_memaddr=TRUE, which keeps its outer while-loop's second
OR clause forever TRUE, so MemMgr spins post-PR_CLEANUP.

Concrete next move:

1. `make audit` to confirm baseline. Then build + run 2000 frames to
   confirm the diagnosis still holds (P128m probes will fire and show
   force=1 at CLEAR_SPACE entry).

2. Add a temporary printf in `src/toolchain/pascal_codegen.c` near
   line 2429 (the AST_IDENT "true"/"false" branch) and near any other
   places that emit $7001 (line 2443, 2629). Print the file/line of
   the source location being compiled (cg has source-position tracking).

3. Recompile the full toolchain (`make audit`) and look for the
   printout fired during compilation of MM2.TEXT line 595. That will
   tell us which codegen path produced MOVEQ #1 for `false`.

4. Fix the structural bug. Likely it's one of:
   - AST kind mismatch — `false` parsed as different AST type
   - WITH-record field shadowing
   - Implicit inversion in caller-clean push path

5. Rebuild and confirm: at PC $04A930 the binary should now emit
   `7000 3F00` (push 0). CLEAR_SPACE should then exit naturally
   after the first iteration that fixes the hole, and MemMgr should
   block on memmgr_sem (steady-state idle).

6. Once fixed, watch for any other `false` literal call sites that
   might also be miscompiling. There are likely many across the
   codebase that have been silently broken.

Anchors:
- src/toolchain/pascal_codegen.c:2429-2455 — the literal-bool emit branches
- Lisa_Source/LISA_OS/OS/source-MM2.TEXT.unix.txt:595 — the suspect call
- src/m68k.c:~4030 — P128m CLEAR_SPACE entry probe (force read at LOW byte)
- src/m68k.c:~4135 — P128m MOVE_SEG entry probe (frame dereferencing)
- build/LisaOS.lisa/linked.bin — flat binary, byte $04A930 = $70 $01

Do NOT add an HLE that sets force=FALSE manually. Per
feedback_do_the_real_fix.md, fix the codegen.

Long-term follow-up (still pending from P128l): SLR limit enforcement
in lisa_mmu.c's mmu_translate(). Out of scope for this fix.
```

## Secondary follow-ups (unchanged from P128l handoff)

1. **SLR limit enforcement in `lisa_mmu.c`** — long-term proper fix
   for the MOVE_SEG-aliasing class. The codegen-bug fix above is
   independent and should land first.

2. **P128h char-comparison context flag** — still pending at
   AST_BINARY_OP for `=`/`<>`/`<`/... on char operands. Don't block
   on it unless a char-comparison miscompile surfaces.

3. **Aggregate-copy fallback fidelity (P128i follow-up)** — current
   `agg_sz = lhs_type_sz` fallback is pragmatic; cleaner is fixing
   `expr_size` to return correct sizes for all AST forms.

4. **P128k broader nested-proc mangling** — the current narrow fix
   handles `Recover` only. Watch for any `SYSTEM_ERROR` whose
   `error` value is suspiciously wrapped (e.g. e_dsbase + N).

5. **clang-tidy misleading-indentation warning** at
   `src/toolchain/pascal_codegen.c:407` — pre-existing, cosmetic.

## Files of interest (P128m)

- `src/m68k.c:~4030` — P128m CLEAR_SPACE entry probe (force from LOW byte)
- `src/m68k.c:~4080` — P128m MOVE_SEG entry probe (caller-A6 frame dump)
- `src/m68k.c:~4070` — bumped CLR_INMOTION_SEG limit 5→50
- `src/m68k.c:~4347` — bumped REMAP_SEGMENT limit 6→50
- `src/lisa_mmu.c:~362` — extended MMRB watchpoint to also cover
  MMRB+56..+63 (head_sdb.freechain.fwd/bkw)

## Commit chain this session

(none yet — pending)

## Pick up here

Paste this to the next session:

```
Picking up from P128m. Diagnosis complete — the post-PR_CLEANUP
MemMgr loop is caused by Pascal codegen miscompiling literal `false`
as MOVEQ #1 (TRUE) at source-MM2.TEXT:595 (compiled to PC $04A930).
This makes CLEAR_SPACE's force_to_hole_memaddr=TRUE, which keeps the
loop's second OR clause forever true.

The codegen branches at src/toolchain/pascal_codegen.c:2429-2455
look correct (MOVEQ #0 for false), but the actual binary emits
$7001 not $7000 at the MM2.TEXT:595 call site. Need to find which
codegen path is actually firing for that compilation.

Concrete first move:
1. make audit; ./build/lisaemu --headless Lisa_Source 2000.
   Confirm 27/27 milestones + force=1 in the P128m CLEAR_SPACE entry probe.
2. Add printf in pascal_codegen.c at every $7001 emit site
   (lines 2430, 2443, 2629) printing source position, then make audit.
3. Find which path fired during MM2.TEXT compilation of
   `with c_mmrb^.clock_ptr^ do CLEAR_SPACE(...,false)`.
4. Fix the codegen. Suspect: WITH-context field-name lookup
   shadowing the literal-false branch, or AST kind mismatch.
5. Rebuild; verify byte at $04A930 is $7000 (not $7001).
6. Run 5000 frames; CLEAR_SPACE should exit naturally and MemMgr
   should block on memmgr_sem (idle steady state) instead of looping.
```
