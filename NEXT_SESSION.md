# Next Session Handoff (2026-04-18 — P89 investigation of realmemmmu SMT programming)

## TL;DR

Root-caused why seg 85-100 end up with SOR=0: the compiled SETMMU
body was a **no-op** because our Pascal codegen didn't handle the
`with ptr^[N] do ... end` pattern. Fixed the codegen (two edits in
`pascal_codegen.c`), and now SETMMU actually emits field writes to
the SMT. But:

1. The SMT writes still go to the wrong address because LOADER's
   SETMMU uses `smt_adr` (A5-6112), which we never initialize — we
   skip BOOTINIT where Apple's code would set it.
2. The fix exposes a 1-milestone downstream regression: previously
   silent WITH bodies in kernel modules (MMPRIM2, MEASURE, PMMAKE,
   LOAD, DS0, STARTUP itself) now actually run their field writes,
   and one of them breaks boot at FS_INIT (was 23/27 with
   SYS_PROC_INIT reached; is now 22/27).

**Uncommitted** on purpose so main stays at 23/27 until the
downstream regression is diagnosed. `git diff` shows the two files
with the proposed fix.

## Accomplished this session

### SMT-region physical watchpoint added (`src/lisa_mmu.c`)

Fires on any byte-write into `[g_hle_smt_base, g_hle_smt_base +
0x400)` — that's domain 0 of the SMT, covering segs 0..255. Each
hit prints PC, logical/physical address, value, decoded (seg, field
name, byte offset). Survived the session and is worth keeping —
it's how we proved SETMMU emits zero SMT writes.

### Pascal codegen fix for `with ptr^[N] do` (WIP, uncommitted)

**P89a — AST_WITH resolves record type for pointer-to-array-indexed
WITH** (`src/toolchain/pascal_codegen.c:3531-3558`). The AST_WITH
type resolver only handled `AST_ARRAY_ACCESS` where `children[0]`
was `AST_IDENT_EXPR`. For `ptr_smt^[128*domain+index]` the tree is
`AST_ARRAY_ACCESS(AST_DEREF(AST_IDENT_EXPR("ptr_smt")), N)`, which
fell through → `rt == NULL` → `with_stack[].record_type = NULL` →
field lookups via `with_lookup_field` return -1 → assignments to
`origin`, `limit`, `access` silently lvalue-resolve to placeholder
addr 0 and the emits become no-ops. Added a branch that follows
the pointer's `base_type->element_type` to get the record.

**P89b — gen_lvalue_addr AST_ARRAY_ACCESS resolves elem_size
through AST_DEREF base** (`pascal_codegen.c:1610-1629`). Same
structural issue in the address-computation path: for `ptr^[i]`,
`elem_size` defaulted to 2, which for `smtent` (4 bytes) halves
the stride. Added the AST_DEREF + IDENT_EXPR case before the
existing IDENT_EXPR case.

**Verified** via hex-dump of SETMMU post-fix: now correctly emits
`MOVE.W D0,(A0)` at offset 0 (origin), `MOVE.B D0,(A0)` at offset
3 (limit), `MOVE.B D0,(A0)` at offset 2 (access) — all at the SMT
entry address `smt_adr + index*4` for domain 0.

## Root cause of SMT still reading zeros

Two separate problems stack:

### (1) Pascal has TWO SETMMU procedures

- `SOURCE-STARTUP.TEXT:111-154` — kernel's 5-param
  `SETMMU(index, domain, base, length, permits)` using
  `ptr_smt^[128*domain+index]`.
- `source-LOADER.TEXT:253-277` — bootloader's 4-param
  `SETMMU(index, base, length, permits)` using `smt_adr^[index]`.

LOADER is in the same directory as STARTUP. Our LOOSE-mode fileset
compiles both. They both export a symbol named `SETMMU`. Our
linker's "first ENTRY wins" policy (`linker.c:199-252`) keeps
LOADER's because Pascal modules are compiled before STARTUP (which
is deliberately last-compiled to see all imports).

So when INITSYS's realmemmmu loop calls `SETMMU(i, oscontext, ...)`
with 5 args, the linker dispatches to LOADER's 4-param SETMMU. For
domain=0 (oscontext) the math happens to still work — LOADER writes
`smt_adr^[index]`, same offset as `ptr_smt^[0*128+index]`.

### (2) `smt_adr` is never initialized

Per `source-LOADER.TEXT:398`:

```pascal
smt_base := INITMMUTIL(BGETSPACE(lo, 5*pagesize, onpage));
smt_adr  := pointer(smt_base);
```

This runs in `BOOTINIT`, which is part of the bootloader. We skip
the bootloader entirely — our C-side `bootrom_build` primes memory
and jumps straight into STARTUP. So `smt_adr` (the Pascal VAR at
A5-6112) stays $00000000 forever, and LOADER's SETMMU ends up
writing origin/access/limit to `$0154..$0190` (low memory /
vector-table range), which the emulator's VEC-GUARD silently
drops after SYS_PROC_INIT (and before that, corrupts the vector
table).

The **STARTUP-version** of the same initialization (line 340)
would have handled it:

```pascal
smt_addr := smt_base;        {establish base address of smt to OS/Pascal}
sys_smt := pointer(smt_addr);
```

where `smt_addr` is pinned by our PASCALDEFS to A5-24887 (and
IS being read correctly by our boot-time plumbing). Note the
`_addr` vs `_adr` — different names, different slots, different
initialization stories.

## Tried and backed out: exclude LOADER.TEXT from the kernel fileset

Added an exclusion list to `toolchain_bridge.c` LOOSE branch so
`source-LOADER.TEXT` gets skipped. Result: LOADER's SETMMU gone,
STARTUP's SETMMU promoted to `$000404`, linker map clean.

But boot broke much earlier — crashed at
`SYSTEM_ERROR(code=46572)` with `ret=$005D70` inside PR_CLEANUP.
Stack dumps showed A6-chain corrupted (`A6=$F7FFF4 → $F80000`),
which means something in the now-missing LOADER code was silently
providing a dependency the kernel needed. Reverted.

Didn't have time to diagnose which LOADER symbol the kernel
depends on. Candidates: BGETSPACE, FINDSPARE, INITMMUTIL, or one
of LOADER's TERMINATE/RANGEERR (PASLIB hooks).

## Verify-before-continuing

```bash
# Baseline (stash my fix):
git stash
make && ./build/lisaemu --headless Lisa_Source 300 2>&1 | grep "Milestones reached:"
# Expect: 23/27 (SYS_PROC_INIT reached)
git stash pop

# With fix (current):
make && ./build/lisaemu --headless Lisa_Source 300 2>&1 | grep "Milestones reached:"
# Expect: 22/27 (FS_INIT last, SYS_PROC_INIT no longer reached)
```

Dig for the regression: diff two trace runs, find which milestone
between FS_INIT and SYS_PROC_INIT regresses — it's one of the
previously-silent WITH bodies now actually writing. Likely
candidates (based on the grep of `with.*\^\[`): MMPRIM2's four
`with c_mrbt^[...] do` sites, STARTUP's own `with sys_mrbt^[...] do`
block, or a PMMAKE/MEASURE variant.

## Recommended plan for next session

**Start by deciding which of the two problems to solve first:**

### Option A — fix the regression, then solve SMT

1. Keep my WIP codegen fixes.
2. Compare `smt_trace4.stderr` (with-fix, 22/27) vs `baseline.stderr`
   (without-fix, 23/27) head-to-tail to find where the runs diverge
   — first instruction that runs differently between the two is
   probably the WITH body whose previously-silent writes now break
   something.
3. Diagnose that specific regression. It might be another
   codegen-adjacent bug (wrong record-type resolution for some
   WITH-base, wrong element size), or a legitimate behavioral
   change that reveals a latent kernel init bug.
4. Once 23/27 is restored WITH the codegen fix, address smt_adr
   initialization by one of:
   - (a) prime `smt_adr`'s A5-slot in `lisa.c`'s bootrom init (the
     slot lives at A5-6112 = `A5+$FFFFE820`).
   - (b) rename `smt_adr` to `smt_addr` in LOADER's parse — hacky.
   - (c) find the LOADER-provided dependency the kernel needs,
     stub it, and re-enable the LOADER exclusion so STARTUP's
     SETMMU wins cleanly.

### Option B — solve the SETMMU collision first

1. Figure out what LOADER actually provides that the kernel
   depends on. Probably one of the loader-helper functions is
   being called by a kernel path (unexpected — LOADER is
   supposed to run BEFORE the kernel). Grep for each exported
   symbol from LOADER in the SYSTEM.OS kernel sources.
2. Stub or replicate the needed pieces.
3. Re-apply the LOADER exclusion so STARTUP's `SETMMU` wins,
   and STARTUP's `smt_addr := smt_base` path initializes the
   pointer.
4. Verify SMT entries get correct values, seg 85-100 programmed
   with real SORs, boot progresses past 23/27 including the
   previously-blocking `$019400` crash documented in the
   previous handoff.

**Both options are multi-hour investigations. Option A is closer
to where we stopped and probably faster. Option B is the cleaner
structural answer.**

## Files changed (uncommitted)

- `src/lisa_mmu.c` — added WATCH-SMT watchpoint (lines ~417-443).
- `src/toolchain/pascal_codegen.c`:
  - `gen_lvalue_addr` AST_ARRAY_ACCESS — added AST_DEREF(IDENT)
    base resolver (lines ~1610-1629).
  - AST_WITH type resolver — added AST_ARRAY_ACCESS(AST_DEREF)
    branch (lines ~3531-3558).

Previous pre-existing clang-tidy warning still not fixed (line
355 misleading-indentation on AST_TYPE_ARRAY case).

## Pick up here

Paste this prompt to resume:

> Continue from NEXT_SESSION.md. P89a (AST_WITH for ptr^[N]) and
> P89b (gen_lvalue_addr AST_ARRAY_ACCESS AST_DEREF base) are in
> the working tree uncommitted. They enable SETMMU to emit SMT
> writes that were previously silent, but cause a 1-milestone
> regression (22/27 vs 23/27 baseline — SYS_PROC_INIT no longer
> reached). Also, SMT writes still go to address 0 because
> LOADER's `smt_adr` Pascal VAR is never initialized (we skip
> BOOTINIT). Two problems to solve — see "Recommended plan".
> Start with Option A: diff baseline vs with-fix traces to find
> which WITH body's newly-active writes regress boot past FS_INIT.
