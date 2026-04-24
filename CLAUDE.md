# LisaEmu — Apple Lisa Emulator

## Project Vision

**This is a real emulator, not a toy.** Like Parallels, but for the 1983
Apple Lisa. It takes Apple's officially-released Lisa OS 3.1 source code
(`Lisa_Source/`, released by Apple via the Computer History Museum in 2018
under Academic License — not redistributable, user-supplied) and:

1. Compiles it with our built-from-scratch 68000 cross-assembler + Lisa
   Pascal cross-compiler + linker.
2. Lays out the resulting binaries on real ProFile-format disk images
   (MDDF + slist + catalog + per-file filemap + 24-byte pagelabels).
3. Runs those images on our 68000 + Lisa-hardware emulator **exactly the
   way a real Lisa would** — full bootloader chain, real driver loading
   at runtime, real filesystem access, real process scheduling, real
   desktop UI.

The endgame is booting Apple's compiled code into a working Lisa desktop
with no shortcuts. Every component Apple released source for must end
up running as compiled Pascal/asm, not as a C reimplementation.

## Architectural Invariants (NON-NEGOTIABLE)

These rules constrain every design decision. They exist in durable memory
(`.claude/memory/`) for a reason — don't ask to relax them.

1. **Real bootloader chain.** The canonical boot path is
   `boot ROM → boot-track code (LDPROF / LDTWIG / LDSONY) → LOADER → SYSTEM.OS
   → SYSTEM.CD_PROFILE → ... → APDM (desktop)`. Every link must ultimately
   run as compiled source. Skipping a link is scaffolding, not a solution.

2. **No semantic HLEs.** If Apple has source for it, we compile it. Do
   *not* "just reimplement OPENINPUT in C" or "replace MMU programming
   with a C function" when the Pascal/asm source is sitting in
   `Lisa_Source/`. Ephemeral HLEs to scaffold past a not-yet-built
   subsystem are fine; keeping an HLE when the source is compilable is
   architectural regression.

3. **HLEs are load-bearing layers, not cruft.** Each active HLE encodes
   real insight about a missing subsystem. Never remove one "as cleanup."
   Remove only in the same commit that compiles + wires up the replacement.
   See `feedback_hle_layers_load_bearing.md`.

4. **Do the real fix, not the workaround.** MMU write-guards, address-
   based bypasses, suppression ranges, placeholder return values — all
   symptom-masking. When a structural bug surfaces (codegen, linker,
   record-layout mismatch), fix the structure. See
   `feedback_do_the_real_fix.md`.

5. **Acceptable HLEs, total list:** hardware surfaces with no source
   (specific VIA/COPS/ProFile chip behaviors we cannot literally emulate),
   host-OS interactions (timer, IRQ scheduling on macOS), scaffolding to
   reach a new subsystem *while we build its replacement*.

## Architecture

### Two layers

1. **Toolchain** — 68000 cross-assembler + Lisa Pascal cross-compiler +
   linker + disk-image builder + boot-ROM generator. Everything needed
   to turn `Lisa_Source/` into bootable disk+ROM artifacts.
2. **Emulator** — Motorola 68000 CPU + Lisa MMU + VIAs + COPS + ProFile
   + video + keyboard + mouse. Runs the toolchain's output.

### Directory Structure

```
/
├── Lisa_Source/           # Apple's source (NOT in git, user-supplied)
├── src/                   # C emulator core + toolchain (canonical source)
│   ├── m68k.h/c           # Motorola 68000 CPU emulator
│   ├── lisa_mmu.h/c       # Memory controller + MMU
│   ├── via6522.h/c        # VIA 6522 chip emulation (x2)
│   ├── lisa.h/c           # Main machine integration + HLE intercepts
│   ├── lisa_bridge.h/c    # C-to-Swift bridge API
│   ├── main_sdl.c         # Standalone SDL2 frontend (headless tests)
│   └── toolchain/         # Cross-compilation pipeline
│       ├── pascal_lexer.h/c      # Lisa Pascal tokenizer
│       ├── pascal_parser.h/c     # Recursive descent parser → AST
│       ├── pascal_codegen.h/c    # AST → 68000 machine code
│       ├── asm68k.h/c            # Two-pass 68000 cross-assembler
│       ├── linker.h/c            # Multi-module linker
│       ├── bootrom.c             # Boot ROM generator
│       ├── diskimage.h/c         # Disk image builder (MDDF + catalog)
│       ├── compile_targets.c     # Per-target module lists (SYSTEM.OS, ...)
│       ├── toolchain_bridge.h/c  # Orchestrates the full compile pipeline
│       ├── audit_toolchain.c     # make audit diagnostic tool
│       └── test_*.c              # Per-component test tools
├── lisaOS/                # Xcode macOS app (SwiftUI, Swift 6)
│   └── lisaOS/
│       ├── Emulator/      # SYMLINKS to src/ (never copy!)
│       ├── ContentView.swift
│       ├── EmulatorViewModel.swift
│       ├── LisaDisplayView.swift
│       └── lisaOSApp.swift
├── docs/                  # Reference (Lisa_Source map, hardware specs, HLE inventory)
├── _inspiration/          # Reference projects (LisaSourceCompilation, lisaem-master)
├── .claude-handoffs/      # Archived NEXT_SESSION.md files — full project history
├── build/                 # Build output (gitignored)
├── Makefile               # Standalone SDL2 build + audit targets
├── CLAUDE.md              # This file
└── NEXT_SESSION.md        # Handoff for the next session (created at /wrap-up)
```

## Key Commands

```bash
# Build standalone emulator (SDL2)
make

# Run toolchain audit — primary diagnostic tool
make audit              # Full report (all 4 stages)
make audit-parser       # Stage 1
make audit-codegen      # Stage 2
make audit-asm          # Stage 3
make audit-linker       # Stage 4: full pipeline + linker

# Headless boot test (1000 frames)
./build/lisaemu --headless Lisa_Source 1000

# Xcode build (or just open in Xcode)
cd lisaOS && xcodebuild -scheme lisaOS -destination 'generic/platform=macOS' build 2>&1 | grep -E "(error:|BUILD)"
```

## Bootloader Chain (real target)

```
[Power On]
  │
  ▼
Boot ROM ($FE0000, 16 KB)
  • Self-test, set up VIAs, initialize RAM
  • Read boot-track block 0 from ProFile (or Sony/Twiggy)
  • Jump into boot-track code
  │
  ▼
Boot-track code (source-LDPROF.TEXT / LDTWIG / LDSONY — asm)
  • Read MDDF at block 24 (ProFile) or block 0 (floppy)
  • Walk slist/catalog to find LOADER file
  • Load LOADER into low RAM
  • Set [$204] = @LOADER.ldrtrap so OS can trap into it
  • Jump into LOADER main entry
  │
  ▼
LOADER (source-LOADER.TEXT + source-ldlfs.text + source-LDUTIL + source-LDEQU + source-LDASM)
  • Read MDDF → bufsize, slist_addr, catentries, fsversion, root_page
  • Walk catalog to find SYSTEM.OS
  • Load SYSTEM.OS code segments via codeblock/endblock format (see LOADCD)
  • Jump into INITSYS
  │
  ▼
SYSTEM.OS kernel (27 milestones: PASCALINIT → ... → PR_CLEANUP)
  • MMU, process scheduler, memory manager, filesystem, IPC
  • When a driver is needed: LD_OPENINPUT('SYSTEM.CD_PROFILE')
    → ENTER_LOADER (mode switch) → LOADER.OPENINPUT → read file from disk
  • Install driver, call its init, register in configinfo
  │
  ▼
APDM (Desktop Manager)
  • Via LIBQD (QuickDraw), LIBTK (Toolkit)
  • Shell, file manager, app launcher
  │
  ▼
Apps (LisaWrite, LisaCalc, LisaDraw, LisaProject, ...)
```

Every arrow above is a real compile target. The toolchain today produces
SYSTEM.OS only; the rest is Phase 2+ work.

## Phases (the multilayer-subsystem-build arc)

Each phase compiles the next real component and removes whatever HLE
was scaffolding the gap. HLE removals happen **only** in the same
commit as the real replacement.

| # | Phase | Target Real Component | Current Status |
|---|---|---|---|
| 1 | **PMEM + boot-CD selection** | Native `FIND_PM_IDS` via PRAM stub | ✅ Complete (2026-04-18 P90) |
| 2 | **Real LOADER + filesystem** | source-LOADER.TEXT + source-ldlfs.text + real MDDF/slist/catalog/filemap on profile.image | In progress — started 2026-04-18 |
| 3 | **First driver file (SYSTEM.CD_PROFILE)** | source-PROFILE.TEXT compiled as .OBJ, placed on disk with catalog entry | In progress — driver loads, dispatch armed |
| 4 | **MDDF / disk-image full FS** | removes 10707 suppression; real FS_INIT / FS_MASTER_INIT | In progress — P128g retires MDDF_IO+BitMap_IO, psio HLE handles both via single nbytes-respecting intercept; FS_INIT completes naturally now (10707 no longer fires during boot) |
| 5 | **IRQ-driven I/O completion** | removes Block_Process-on-POP deadlock | In progress — IRQ scaffold complete (P120), seg-alias guard shipped (P122); PARALLEL → IODONE still pending |
| 6 | **SYS_PROC_INIT + real processes** | removes P89i + CreateProcess HLE pile; fixes Pascal `ptr^[i]` type-resolution class of bugs (P125 surfaced) | Not started — Make_SProcess blocked on codegen |
| 7 | **Cleanup HLEs** (FS/MEM/PR_CLEANUP) | real cleanup bodies | Not started |
| 8 | **Safety-net HLEs** (REG_OPEN_LIST, excep_setup) | remove | Not started |
| 9 | **System libraries** (SYS1LIB, SYS2LIB) | new compile targets | Not started |
| 10 | **Graphics** (LIBQD, LIBTK) | new compile targets | Not started |
| 11 | **Shell (APDM)** | full desktop | Not started |
| 12 | **Apps** | LisaWrite, LisaCalc, etc. | Not started |

## Current Status (2026-04-24 post-P128h — **case-of-char codegen bug retired; DecompPath works; post-SYS_PROC_INIT bogus-sem cascade no longer the blocker.** Boot now advances past MAKE_DATASEG → DS_OPEN → FMAP_IO → vm before hitting SYSTEM_ERROR(10201) at `hard_excep+646` — the SAME Bug C from P128b (kernel-view and user-view stack segments don't alias for newly-created process's Initiate RTS), now re-exposed because we actually reach the Initiate path. 10201 was previously masked by the open_temp(-1) fake-sem infinite loop that preceded it.)

### P128h (2026-04-24 pm, post-dff5940) — Single-char string-literal codegen fix

**Root cause (class-of-bug, structural).** Lisa Pascal's `'-'`, `' '`,
`'+'`, etc. are CHAR values, not STRING pointers. Our Pascal codegen
emitted AST_STRING_LITERAL uniformly as a `LEA data(PC),A0 / MOVE.L
A0,D0` sequence — so in expression contexts D0 holds the ADDRESS of
the length-prefixed string data, not the char VALUE. In CHAR-expecting
contexts (case labels, char assignments, char comparisons) the
downstream code then treats that address as the value:

- `case delim of '-': ...`: `CMP.W D3,D0` compares delim byte against
  low word of a code address. Never matches. The `' '`/`'+'` arm
  never fires either. `device` stays -1 (its initial value), which
  cascaded into `open_temp(device=-1)` and the P128e/g bogus-sem wall.
- `delimiter := ' '` (Gobble): `MOVE.B D0,(A0)` writes the LOW byte
  of the address (e.g. $A8, depending on where the ' ' literal landed
  in code) into the delim slot. Every caller of DecompPath/Gobble
  that relied on the delim return was reading a garbage byte.
- `if c <> '-'` comparisons (Gobble's scan loop): same class.

**Fix — `src/toolchain/pascal_codegen.[ch]` + context flag**. Added
`codegen_t.char_literal_context` flag. `AST_STRING_LITERAL` codegen
now checks: if `char_literal_context && strlen==1`, emit
`MOVEQ #ch,D0` (load the char value). Otherwise emit the original
LEA/data/MOVE.L pattern (correct for actual string pointers). Flag
set at three call sites (P128h structural fix):

1. **Case labels** (both `AST_CASE_LABELS` multi-label loop and
   single-label else branch in the `AST_CASE` handler). `case of
   char` now dispatches correctly.
2. **Assignments where LHS is byte-sized**
   (`char_literal_context = true` when `expr_size(LHS) == 1`).
   `charVar := 'x'` now writes the char value, not an address byte.
3. Comparisons still TODO (Gobble's scan loop uses the string-path
   in AST_BINARY_OP string-compare logic, which happens to byte-compare
   char-vs-1-char-string correctly because the length byte mismatch
   path falls through to "not equal" — which is the right answer for
   non-delimiter chars. Not reliable for correctness; add context flag
   to AST_BINARY_OP `=`/`<>` next session.).

**Verification.**
- `make audit`: 0 unresolved, all modules link clean.
- Headless run: `DecompPath EXIT @dev=$CBF9DC → dev=37` (was `dev=-1`).
- Case dispatch trace: `delim = $20` (was $A8). PC flows
  `+118 BNE → +166 ('-' mismatch) → +174 BEQ for ' ' (match) →
  +194 body` and writes working_dev=37 into @device.
- Boot advances: `Sys_Proc_Init → Make_SProcess → Get_Resources →
  Make_SysDataseg → MAKE_DATASEG → DS_OPEN → FMAP_IO → vm` before
  halting at SYSTEM_ERROR(10201). Pre-P128h the chain stalled at
  `open_temp(device=-1)` waiting on a bogus sem address.
- Boot milestone: still 23/27 (SYS_PROC_INIT), but reaching the
  Root-process Initiate path is new forward progress inside that
  milestone.

**Files changed (P128h).**
- `src/toolchain/pascal_codegen.h` — new `char_literal_context` flag
  on `codegen_t`.
- `src/toolchain/pascal_codegen.c` — AST_STRING_LITERAL honors the
  flag; AST_CASE label emission and AST_ASSIGN (when LHS size==1)
  set/clear the flag.
- `src/m68k.c` — P128h probe block: `DecompPath` / `getdevnum` /
  `Gobble` / `FS_Setup` entry+exit + code dumps + case-dispatch PC
  trace. Load-bearing diagnostic infrastructure for future
  codepath debugging. Keep.

**Retires.** The P128f/g `open_temp(device=-1) → bogus mt[-1] →
fake sem wait → scheduler dispatches nobody → Pause` cascade no
longer happens. The P128e probes (which diagnosed that cascade) are
kept as infrastructure but that specific path is dead.

### Next blocker — SYSTEM_ERROR(10201) at hard_excep+646 (recurrence of P128b Bug C)

Once DecompPath correctly returns dev=37 and MAKE_DATASEG runs, the
flow reaches `DS_OPEN → FMAP_IO → vm` and eventually `Initiate` for
the newly-created Root process. `Initiate`'s RTS reads the user
stack via stackmmu (seg 123) but the kernel-view setup wrote to
seg 104. The two physical pages don't alias → RTS pops zeros →
PC=0 → vector walk → `SYSTEM_ERROR(10201)` at `hard_excep+646`.

Same root cause as the P128b Bug C diagnosis. P128d shipped a
partial fix (`mmustack hw_adjust` in `lisa_hle_prog_mmu`) but
apparently doesn't cover the Root process's ldsn setup path.
Concrete next move: probe `SET_LDSN(stk_sdb, stackmmu, mmustack)`
against the Root process's stack sdb and confirm seg 123's SOR
aliases the same physical frames as seg 104 used during Build_Stack.
If not, extend the P128d fix to cover `SET_LDSN` as well.

Do NOT add an HLE to suppress 10201 — per
`feedback_do_the_real_fix.md`, the MMU aliasing is a real
emulator bug (our MMU translates SOR=0 literally instead of
demand-faulting). Fix the structural invariant: a given LDSN's
user-view and kernel-view both map to the same physical frames.

### P128g (2026-04-24 pm) — psio HLE, MDDF_IO/BitMap_IO retired (real fix for freelist corruption)

**Root cause.** MDDF_IO HLE wrote a full 512-byte disk block into the
caller's buffer, but Apple's MDDF_IO calls
`psio(..., nbytes=sizeof(MDDFdb)=316, ...)` — the buffer is only 316
bytes. The 196-byte overflow clobbered the sysglobal pool's next
freelist entry at `currfree + 318`, setting its `size` field to 0.
On the next GETSPACE call the walk saw `free[0]: size=0 next=0`,
treated the pool as empty, and returned FALSE. Observed at
`GetFCB → getspace(sizeof(sfcb)=254, b_sysglobal_ptr, addrFCB)`
during `fs_mount → OPEN_SFILE` — matches the P128f handoff's
"GETSPACE returns FALSE for sysglobal" blocker.

**Fix.** `src/lisa.c:lisa_hle_intercept` — retire `hle_handle_mddf_io`
and `hle_handle_bitmap_io` (length-ignorant 512-byte writes).
Enable `hle_handle_psio` instead: it already existed (gated off as
P108 scaffold) and respects `nbytes` correctly via RMW on the
trailing partial page. Apple's compiled MDDF_IO / BitMap_IO /
FMAP_IO / pglblio all call through psio (or `vm`) so a single
psio intercept subsumes three bug-prone HLEs.

**Verification.** Pool freelist stays healthy across all 60+
GETSPACE calls (pre-fix: `free[0].size=0 next=0` by call #59;
post-fix: clean `size=14230 next=0` at the previously-failing
254-byte call). GETSPACE(254) from GetFCB now returns D0=1
(success). `SYSTEM_ERROR(10707)` no longer fires during boot —
FS_INIT completes naturally, `FS_Setup` runs end-to-end. Pool
never hits the out-of-pool retry path anymore.

**Files changed (P128g).**
- `src/lisa.c:~4510` — `#if 0` MDDF_IO + BitMap_IO intercepts;
  `#if 1` the psio intercept (was `#if 0`).
- `src/m68k.c` — P128g-ENTRY diagnostic probe at every sysglobal
  GETSPACE entry: dumps pool header + free-list walk so future
  freelist corruption is immediately visible. Gated by `(b_area
  >> 16) == 0xCC` so it fires across builds. Limited to 30 dumps.
  Added `"FS_Setup"` to `pcs[]` so [POST-BOOT] logs confirm
  FS_Setup entry (prior sessions wrongly assumed it didn't run).
- `src/lisa_mmu.c` — removed P128f-era one-shot pool watchpoints
  that targeted $CCBE8A (build-specific pool offset) now that
  the overflow is fixed at the source.

**Retirement chain.** `hle_handle_mddf_io` and `hle_handle_bitmap_io`
remain defined in `src/lisa.c` but unreferenced. Safe to delete in
a future cleanup pass. Do NOT delete unless you also delete the
Pascal-side callers' expectations — they don't exist, since Apple's
compiled MDDF_IO / BitMap_IO work fine through psio.

### Next blocker — post-SYS_PROC_INIT bogus-sem cascade (unchanged from P128e)

Even with P128g's real fix, the 23/27 → 24/27 wall holds because
`SYS_PROC_INIT → Make_SProcess → Get_Resources → MAKE_SYSDATASEG('') →
MAKE_DATASEG('x') → open_temp(device=-1)` still fires — `working_dev`
reads as -1 at that call. FS_Setup DOES run now (verified via
`[POST-BOOT] entered FS_Setup @$02FA9E` probe fires twice: once on
the kernel syslocal, then again on a process-under-creation's
syslocal during SYS_PROC_INIT). But either:

1. The `working_dev` field inside the NEW process's syslocal isn't
   getting populated by our FS_Setup run (our codegen may produce
   the write but to the wrong offset), OR
2. The `getdevnum` call that reads `working_dev` is reading from
   a different syslocal than the one FS_Setup wrote to.

Next-session starting move: add an A5-relative `working_dev` dump
at FS_Setup exit and at `getdevnum` / MAKE_DATASEG entry so we
can see which syslocal each side touches. Per our pin table,
`b_syslocal_ptr` is at `A5-24785` (= $CC5F6F in our boot). That
pointer's dereferenced value is the syslocal base; `working_dev`
is a field inside that syslocal. Dump `(*b_syslocal_ptr).working_dev`
before and after FS_Setup, and at every getdevnum call, to close
the gap.

### Earlier (P128f, P128e, etc.) findings — still load-bearing



### P128f (2026-04-24 pm) — slist_io HLE shipped, GETSPACE zero-fill gated

Three fixes shipped this round, in order of discovery:

**Fix 1 — `hle_handle_slist_io`** (`src/lisa.c`). Reads the requested
s_entry directly from the slist on our profile image. Sits parallel
to MDDF_IO and BitMap_IO HLEs — bypasses `vm → UltraIO → DISKIO`
chain that would otherwise need a real IRQ-driven I/O completion to
deliver the bytes. Verified empirically: pre-fix `vm()` returned
`ecode=511` and DID NOT WRITE the sentry buffer; post-fix the
sentry decodes correctly (hintaddr=1589, fileaddr=1590, filesize=3456
for `rootsnum=3`). Wired into the dispatch alongside the other FS
HLEs at `src/lisa.c:4416`-ish.

**Fix 2 — re-enabled `hle_handle_vm`** (`src/lisa.c:4538`). The
P108 `hle_handle_vm` (general-purpose page-aware sub-block reader)
was gated off after a previous regression. With slist_io HLE in
place, the regression no longer applies — vm now handles
`hentry_io / FMAP_IO / pglblio` cleanly. Toggle: `#if 1` in the
fsio_addr dispatch block.

**Fix 3 — GETSPACE zero-fill sanity gate** (`src/m68k.c`). The
existing GETSPACE post-RTS hook zero-fills the allocated chunk to
give Pascal `GETSPACE` calloc semantics. But when compiled GETSPACE
returns a bogus address (observed: `$CBFF1E` = OPEN_SFILE's saved
A6, on the supervisor stack, returned by GetFCB's GETSPACE call
inside fs_mount), the zero-fill amplifies the bug into stack
corruption — wiping OPEN_SFILE's `devnum` parameter at A6+14, which
caused the cleanup `signal_sem(mounttable[devnum]^.semio)` to read
`mounttable[0]^.semio` (= badptr1 + $6C = bogus addr — the P128e
bogus-sem). Gate: only zero when `(allocated >> 16) == (b_area >>
16)` — i.e., allocated lives in the same 64KB region as the requested
pool. Skip the zero-fill silently otherwise. Pascal allocator bugs
will surface naturally as caller-visible failures rather than as
stack corruption masquerading as deeper symptoms.

### Probe sweep added in P128f

`src/m68k.c` P128f probe block (additive to P128e):
- `slist_io / OPEN_SFILE / FMAP_IO / hentry_io / psio / vm /
  real_mount` added to `pcs[]` table (`pcs_cache[64]`)
- `OPEN_SFILE ENTRY` probe at `$009400` decodes the call args
- `OPEN_SFILE post-slist_io` probe at `$0095B0` dumps the sentry
  bytes the OS sees, confirming whether vm wrote them
- `OPEN_SFILE cleanup signal_sem` probe at `$009DDE` dumps A0,
  devnum, and `mounttable[devnum]` so we can spot bogus sems
- `WATCH armed` at `$009404` (post-LINK) latches OPEN_SFILE's A6
  so we can monitor `devnum` clobbering throughout the call
- `GETSPACE entry` probe at `$0072CA` decodes size/b_area/@allocaddr
- `GETSPACE-STORE` probe at `$0076C8` (gated on `open_sfile_a6 != 0`)
  was added but never fires — execution skips that path in our
  trace (TODO: verify if Pascal pool walk takes a different exit)
- `GETSPACE-ZF` probe at the post-RTS hook decodes whether the
  allocation is in-pool or out-of-pool (see Fix 3)

`src/lisa_mmu.c` adds a P128f write watchpoint:
- `lisa_mem_write8` and `lisa_mem_write16` log writes to
  `$CBFF2C..$CBFF2D` (the OPEN_SFILE devnum slot in the trace's
  specific frame) when `g_p128f_watch_armed` is true
- Globals `g_p128f_watch_armed`, `g_p128f_last_pc`,
  `g_p128f_last_a6`, `g_p128f_last_a0`, `g_p128f_last_sp` defined
  in `m68k.c`, set per-instruction inside the P128e/P128f probe
  block, consumed by the write hook

These probes are **load-bearing diagnostic infrastructure**.
Do not remove them.

### Next blocker (P128g territory)

**Pascal `GETSPACE` returns invalid stack-area address `$CBFF1E`
for a sysglobal pool allocation** (`size=254 b_area=$CC6FE0`).
GetFCB requested 254 bytes from sysglobal; GETSPACE walked the
free list and ended up at `currfree = $CBFF1C` (a stack address),
returning `ordaddr = currfree + 2 = $CBFF1E`. Possible causes:

1. **Corrupted pool header**. The sysglobal pool header at
   `b_sysglobal - 24575` (= `$CC1001` for our boot) has
   `firstfree` field that ends up pointing into stack memory.
   If something earlier wrote bad data into the pool header,
   the free-list walk follows broken next-pointers into garbage.
2. **Codegen miscompile** in GETSPACE's free-list walk. P125
   identified a class of `ptr^[i]` codegen failures (~40 sites
   per build) that default to `array_low=0, elem_size=2` when
   pointer-to-array type chains don't resolve. GETSPACE's free
   list traversal might be hitting a sibling site of that bug.

Per CLAUDE.md note on P111 ("Our compiled MM4.TEXT:GetFree is
allocating LOW pages that overlap kernel A5-globals") this is a
known class of bug in our compiled allocator. **Fix it once and
many downstream FS / process-creation paths get unblocked.**

### P128e (2026-04-24 mid) — Diagnostic that root-caused the cascade

**The post-P128d handoff hypothesized "MemMgr fails to signal a sem
during init handshake." That hypothesis was wrong.** Empirical probes
of `Wait_sem` / `Signal_sem` / `Block_Process` / `open_temp` /
`SplitPathname` / `MAKE_SYSDATASEG` / `MAKE_DATASEG` / `DS_OPEN` show
the actual cascade:

1. `SYS_PROC_INIT → Make_SProcess(Root, …, resident=false)` (non-resident path)
2. `Get_Resources → MAKE_SYSDATASEG(progname='', discsize=$0C00)`
3. `SPLITPATHNAME('')` returns **`progunit=-1`** (DecompPath default for
   empty path; ecode=E_NO_DEVICE — caller MAKE_SYSDATASEG **ignores**
   `errnum` after this call)
4. `MAKE_SYSDATASEG`: `firstunit := progunit = -1` →
   `MAKE_IT(-1)` → `dsname := 'x'` (Apple's placeholder)
5. `MAKE_DATASEG('x', ds_private)`:
   - `SplitPathname('x')` → Gobble returns `delim=' '` (no '-' in 'x') →
     `device := working_dev`
   - **`working_dev = -1`** in the kernel pseudo-PCB's syslocal
   - `f_segname := concat('-', configinfo[-1]^.devname, '-PDS')` (garbage)
6. `DS_OPEN.OPENIT`:
   - `SplitPathname(garbage_segname)` → `device = -1`
   - `OPEN_TEMP(?, device=-1, ?)`
7. `open_temp`: `mounttable[-1]` reads 4 bytes BEFORE the table at
   logical `$CC502C` → returns garbage `$4C0E5C8F`
8. `mounttable[device]^.semio = $4C0E5C8F + $6C = $4C0E5CFB` —
   the bogus "sem" address
9. `wait_sem(@$4C0E5CFB, [])` → cnt at that addr happens to be 0 →
   `Block_Process(kernel_pcb=$CCB58E, [sem])`
10. **Nobody will ever signal a fake sem.** Scheduler dispatches MemMgr
    (the only other Ready process), MemMgr runs its body, blocks on
    `WAIT_SEM(memmgr_sem)` (cnt=0, waiting for a swap request that
    will never come), idle → Pause forever.

**Why `working_dev = -1` in the kernel syslocal:** `FS_SETUP(sloc_ptr)`
(at SOURCE-FSINIT2.TEXT:891) sets `working_dev := bootdev`. It runs
at the END of `FS_INIT` (STARTUP.TEXT:954), AFTER `FS_MASTER_INIT`
succeeds. Our **10707 HLE suppression** unwinds out of FS_INIT
before FS_SETUP runs, leaving `working_dev` in its default-init
state of `-1`. Pre-P128c+P128d this didn't surface because the
kernel pseudo-PCB never actually got dispatched into Block_Process
(Bug B's queue-unlink miscompile masked it via the 10204 cascade).

**Why `FS_MASTER_INIT` fails (= why 10707 fires):** After
`fs_mount → real_mount → def_mount → MDDF_IO HLE → BitMap_IO HLE →
MDDF_IO WRITE HLE → UltraIO → DISKIO → MakeSGSpace HLE → DISKIO retry`,
the chain reaches an `OPEN_SFILE(rootsnum)` call. The compiled `slist_io`
returns `ecode=871 = E1_SENTRY_BAD` (visible in regs at 10707-fire:
`D2 = $367 = 871`). So either:
- our disk image's slist entry for `rootsnum=3` (root catalog) has
  `hintaddr = 0` despite `diskimage.c:476` writing it correctly, OR
- the MDDF read returns wrong `rootsnum` / `slist_addr` /
  `slist_packing` so we look up the wrong slist page, OR
- `slist_io`'s `vm()` call sub-block-reads from the wrong physical
  block due to our P104 MDDF_IO HLE intercepting only some I/O paths
  but not the real `vm` chain that slist_io actually uses.

### Three options for the next session (user picked C / "real fix")

**A) One-line HLE extension** — make the 10707 suppression also do
`working_dev := bootdev` (`writeword to syslocal+$A8`, `bootdev` is
at `A5+$FF2C` per FS_Setup compiled asm). Smallest patch; probably
unblocks until the next FS-dependent failure (`MAKE_SYSDATASEG`
returning `e_filesyserr+e_dsbase` → `SYSTEM_ERROR(10101)`). Path of
least resistance, but band-aid.

**B) Fake-mount HLE** — populate `mounttable[bootdev]` with a synthetic
mountrec whose `semio=1` (mutex) and whose other fields cause
downstream FS calls to return clean errors. Allows resident-process
creation if we patch open_temp's "if mounted" check. Bigger surface.

**C) ✅ Real fix — Phase 4** — make `FS_MASTER_INIT` actually succeed.
This requires: (a) verify our disk image's MDDF + slist entries are
read correctly by the OS; (b) find why `slist_io(rootsnum)` returns
`E1_SENTRY_BAD = 871`; (c) fix the underlying mismatch (likely in
`diskimage.c` MDDF/slist field placement, OR add slist_io HLE so it
reads from disk image directly like MDDF_IO does). Multi-session
work but architecturally clean per CLAUDE.md Phase 4 plan.

### Probes added this session (P128e — diagnostic only, no fix)

`src/m68k.c` — pci_trace budget bumped 200 → 800; new probe block
that fires only after `Make_SProcess#2` and dumps:
- `Wait_sem` / `Signal_sem` entries with @sem, count, owner,
  wait_queue (decoded from sem record at @sem)
- `Block_Process` / `Unblock_Process` entries with @pcb and reasons
- `Get_Resources` / `FinishCreate` / `MAKE_SYSDATASEG` /
  `MAKE_DATASEG` / `DS_OPEN` / `open_temp` / `SplitPathname`
  entries with their typed args decoded from the stack
- `open_temp` additionally dumps `mounttable[device]` and the first
  16 mounttable entries

Three new probe symbols added to `pcs[]`: `open_temp`, `DS_OPEN`,
`MAKE_DATASEG`. Probe budget capped at 80 fires of the diagnostic
block to avoid log explosion.

These probes are **load-bearing diagnostic infrastructure** for the
next session. Do not remove them when shipping a fix.

### P128d (2026-04-24 am) — Bug C shipped (10201 retired)

Root cause: `lisa_hle_prog_mmu` in `src/lisa.c` stored the SMT `origin`
directly as the MMU's SOR register value — but the real Lisa
`DO_AN_MMU` asm handler (source-LDASM.TEXT:403-411) applies a stack-
segment transform before writing the SOR:

```
if access = mmustack ($6) then
  if length = 0 then length := $100       (meaning full 128KB)
  origin := origin + length - hw_adjust   (hw_adjust = $100 pages)
  length := length - 1
```

This places the stack segment's valid pages at the TOP of the 128KB
logical window, so Build_Stack's formula
`A6 := MMU_Base(stackmmu+1) - (seg_size - offset)` — which positions
the user-mode USP near the top of the window — maps to physical
bytes actually inside the allocated stack pages.

Without the transform, seg 123 SOR was written as `memaddr` ($F77 for
mmstk_sdb post-Move_MemMgr). User logical $F7FFE0 then translated to
phys $20EDE0 — **past** the 9 allocated stack pages at $1EEE00..$1F0000
— and Initiate's RTS popped zeros → PC=0 → vector walk →
`SYSTEM_ERROR(10201)` at hard_excep+646.

With the transform (shipped in this session): seg 123 SOR = $E80,
USP=$F7FFE0 → phys $1EFFE0 ∈ [$1EEE00, $1F0000]. RTS now reads the
`@ExitSys` byte pattern Build_Stack wrote (via seg 104 kernel view,
copied by Move_MemMgr's MOVER to the new phys location).

Files changed in P128d:

- **`src/lisa.c:889`** — add the `mmustack` branch to the SOR
  computation inside `lisa_hle_prog_mmu`.
- **`src/lisa.c:4042`** — retire the 10201/10204 SYS_PROC_INIT unwind
  HLE (per `feedback_hle_layers_load_bearing.md`): the two root
  causes it was scaffolding past (Bug B shipped in P128c, Bug C
  shipped in P128d) are structurally fixed. SYSTEM_ERROR now HALTs
  as it should, so any future 10201/10204 surfaces a real bug
  instead of being silently suppressed.
- **`src/lisa_mmu.c` / `src/lisa_mmu.h`** — add three diagnostic
  helpers used by the new probe and useful for future MMU-aliasing
  investigations: `lisa_mmu_xlate_info()` (translate logical → phys
  and report seg/SOR/SLR), `lisa_mmu_seg_info()` (read any context's
  segment descriptor), `lisa_mem_read_phys8/16/32()` (phys-direct
  reads, bypassing MMU).
- **`src/m68k.c`** — P128d probe block that logs Build_Stack entry,
  Move_MemMgr entry, MOVE_IT entry, Mover entry, REMAP_SEGMENT,
  Launch entry (with MRBT/sdb/seg-context dump), and Initiate entry
  (with USP→phys translation). Leave-in for future regression
  hunts; capped to a few fires per probe.

### Post-P128d boot state

- `./build/lisaemu --headless Lisa_Source 3000`:
  **23/27 milestones** (last: SYS_PROC_INIT).
- **No** `SYSTEM_ERROR(10201)` or `SYSTEM_ERROR(10204)`. Only 10707
  (FS_MASTER_INIT suppression) fires, unrelated.
- Boot ends at Pause ($07765A, 2 bytes into the Pause procedure's
  `STOP #$2000` idle pattern, SR=$2000). MemMgr dispatch succeeded;
  scheduler drained the Ready queue and entered idle.
- **Why 23/27 instead of 26/27**: the 10201 unwind HLE used to skip
  past SYS_PROC_INIT and let INITSYS fire INIT_DRIVER_SPACE,
  FS_CLEANUP, MEM_CLEANUP. With the real scheduler now dispatching
  MemMgr, the kernel pseudo-PCB self-blocks inside SYS_PROC_INIT
  waiting for MemMgr to signal a semaphore; if MemMgr hits its own
  Pause before signalling, SYS_PROC_INIT never returns to INITSYS.
  Diagnosing this is the next step (Phase 6 area).
- `make audit`: 0 unresolved, 8876/8876 resolved.

### P128c recap (2026-04-24 am) — Bug B shipped

**Milestones**: **23/27** SYS_PROC_INIT (back to the pre-P121 baseline
but now with real UltraIO + MDDF/BitMap HLEs + SEG-ALIAS-GUARD +
MakeSGSpace pool-expansion HLE all live).

Today's commits:

- **P122** (`e8d410a`) — **SEG-ALIAS-GUARD** in `lisa_mem_write8`.
  Drop writes where logical ≥ `$20000` MMU-translates to phys `< $400`.
  Fixes vector-table corruption when BitMap_IO writes 1208 B through
  seg-61 (SOR=0 = Apple's "unallocated demand-page" state — our MMU
  doesn't emulate segment faults, so SOR=0 literally aliases to phys 0).
  22/27 crash path eliminated; still stuck at 22/27 until P124.
- **P123** (`a8411a7`) — diagnostics only. Expanded POST-BOOT watchlist
  + scheduler-entry PC-ring dump. Traced 22/27 FS_INIT wall to compiled
  `DISKIO` → `GETSPACE(106)` failing (sysglobal pool exhausted after
  ~60 allocations) → `UltraIO` error recovery → `MakeSGSpace` →
  `Enter_Scheduler` waiting for a memmgr process that doesn't exist.
- **P124** (`8651c8f`) — **HLE MakeSGSpace** at `$012970`. Intercepts
  the compiled Pascal body and runs the pool-expansion work the memmgr
  would have done: reads `sg_free_pool_addr` (A5-24575), allocates a
  4 KB chunk from segment headroom, links it at head of firstfree,
  bumps freecount, clears `grow_sysglobal` (A5-26541), sets
  `no_space=false`, RTS. Restores 22/27 → **23/27**. Retires with
  Phase 6 (real memmgr process).
- **P125** (`5bf47f3`) — diagnostics only. Traced the 23/27 →
  24/27 wall to `SYS_PROC_INIT` → `Make_SProcess(MemMgr)` →
  `Get_Resources` → `Make_SysDataseg` → `CHK_LDSN_FREE(ldsn=-1)` →
  `lbt[-1]` returns non-zero → errnum=304 (e_ldsnused) →
  SYSTEM_ERROR(10101).
- **P127b+** (this session) — **Two more structural fixes retire the
  MM_Setup infinite-loop wall; boot still 23/27 but the next blocker is
  a subtle state bug (freechain walk cycle in INSERTSDB) — NOT a
  codegen bug anymore.**

  - **P127c** (`src/toolchain/pascal_codegen.c` SIZEOF handler) —
    `SIZEOF(initSegMap)` inside `with c_syslocal_ptr^ do` returned 2
    (default) instead of 16 (packed array[0..127] of boolean). The
    handler only looked up argument as `find_type` or
    `find_symbol_any`, never checked WITH-scoped fields. Fix: also
    consult `with_lookup_field` and use the field's type size. This
    retired MM_Setup's initSegMap-clear loop infinite spin (`long_ptr
    := @initSegMap + Sizeof(initSegMap)`, decrement by 4, until ==
    @initSegMap; started at +2 so never hit +0).

  - **P127d** (`src/lisa.c` loader-params) — clamp `l_physicalmem`
    reported to the OS at 2MB. Apple's MM4 hardcodes
    `tail_sdb.memaddr = 4096` (= 2MB / 512-byte block) as the
    freechain sentinel. Our emulator has 2.25MB RAM (LISA_RAM_SIZE)
    to dodge the mmucodemmu SOR=$FE4 → phys $1FC800 wrap. Reporting
    the full 2.25MB to the OS let MAKE_REGION / GetFree produce sdbs
    with memaddr > 4096. INSERTSDB's freechain walk loops on any
    memaddr exceeding the tail — fatal. Clamp: pass 2*1024*1024 for
    l_physicalmem. Keeps the emulator RAM at 2.25MB but the OS sees
    exactly 2MB. The clamp itself is correct, but the INSERTSDB spin
    with `c_sdb.memaddr=4480` still happens via a different path
    (GetFree's `f_sdb.memaddr + f_sdb.memsize - size = 4480`, so
    some free sdb has memaddr+memsize ≥ 4486 — cause not yet
    identified).

- **P127e** (this session) — **screen clamp**. `b_screen` was
  computed as `LISA_RAM_SIZE - l_screen = $238000` (top of 2.25MB).
  But the REAL Lisa screen sits at `$1F8000` (top of 2MB). AVAIL_INIT
  computes `freelen := himem - lomem` using `himem = b_dbscreen`,
  and with b_dbscreen=$230000 the free pool extended to memaddr 4480
  — past Apple's tail_sdb.memaddr=4096 sentinel. MAKE_FREE probe
  showed `maddr=1372 msize=3108 (end=4480)` — the 512-bytes-too-many.
  Fix: hardcode `LISA_HW_RAM_SIZE = 2MB` for screen-base computation,
  independent of LISA_RAM_SIZE. Post-fix: `MAKE_FREE[1] maddr=1372
  msize=2596 (end=3968)`, free pool stays strictly < 4096. INSERTSDB
  freechain walk terminates normally now.

  After P127e: Make_SProcess(MemMgr) completes end-to-end:
  Get_Resources → Make_SysDataseg (×2) → CreateProcess →
  Build_Syslocal → MM_Setup → Build_PCB → FinishCreate.

- **P127f** (this session): **SMT pre-population**. Root-caused the
  A6=0 illegal-instruction crash to REMAP_SEGMENT iterating the SMT
  from entry 17..511 and matching every slot whose origin==0 against
  an old_memaddr==0. Our reset-time `SET_MMU_SEG` wrote directly to
  `mem->segments[ctx][seg]` but NEVER populated the OS-visible SMT
  (a block at `smt_base` in RAM that the Pascal code scans). So the
  OS's SMT had origin=0 for seg-101 (superstack), seg-102, seg-103,
  etc. During Move_MemMgr's REMAP_SEGMENT call, the OS found SMT[101]
  matching and called MAP_SEGMENT with c_mmu=101 — which wrote
  c_sdb.memaddr=$F80 as seg-101's SOR in the current context. Seg-101
  IS the supervisor stack. Reprogramming it invalidated the stack
  mid-call: UNLK A6 read zero for saved_A6 and ret_pc, RTS popped 0
  into PC, CPU walked the vector table until it hit the illegal
  opcode at $000028.

  Fix: added `SET_SMT_ENTRY(seg, origin_blk, access, limit_blk)` that
  writes the 4-byte SMT entry at `smt_base + seg*4` with the correct
  origin/access/limit, alongside every `SET_MMU_SEG` call. Wrote
  entries for segs 101, 102, 103, 104, 105, 123. The REMAP_SEGMENT
  scan now correctly skips these (their origins are non-zero and
  specific), and the three SEG101-REPRG events now fire only for
  contexts 2/3/4 (other domains), NOT context 1 (live supervisor
  context) — so the stack stays intact.

- **P127g** (this session): **ptr^[i].field codegen fix — unblocked
  all remaining milestones.** Tracing `MOVE_IT(mmstk_sdb)` showed
  mmstk_sdb.memsize=0 (completely-zero sdb record). Root cause: the
  compiled `c_mrbt^[stackmmu].sdbRP` in Move_MemMgr was reading from
  mrbtEnt's offset +0 (the `access` field, a byte) instead of +2
  (the `sdbRP` relptr). Our codegen's `lvalue_record_type` in
  `pascal_codegen.c` handled AST_ARRAY_ACCESS only when `children[0]`
  was AST_IDENT_EXPR — it had NO case for AST_DEREF as the base
  (i.e. `ptr^[i]` form). So for `c_mrbt^[stackmmu].sdbRP`,
  lvalue_field_info silently failed, `field_off` defaulted to 0,
  and the generated code did `LEA (c_mrbt + 122*4)(A0); MOVE.W
  (A0),D0` — reading access+state bytes instead of sdbRP. The
  "sdbRP" value was then added to b_sysglobal_ptr producing a
  pointer into sysglobal that happens to contain all zeros →
  mmstk_sdb all zero → GetFree(size=0) → rest of flow broken.
  
  Fix: extended `lvalue_record_type`'s AST_ARRAY_ACCESS branch to
  handle AST_DEREF base. If `arr->type == AST_DEREF` and its child
  is an AST_IDENT_EXPR, resolve the pointer's base_type (which
  should be an array), unwrap to its element_type, and return the
  record type. Also handles WITH-scoped pointer-to-array case.
  
  Result: `c_mrbt^[stackmmu].sdbRP` now correctly resolves to
  offset 2 within the 4-byte mrbtEnt. Move_MemMgr gets the real
  stack sdb (memsize=9, sdbtype=stack, valid memchain). MOVE_IT
  succeeds, Move_MemMgr returns, SYS_PROC_INIT returns, and
  INITSYS continues through INIT_DRIVER_SPACE → FS_CLEANUP →
  MEM_CLEANUP → PR_CLEANUP. **All 27 milestones resolve.**

  Class-of-bug observation: this is the same structural class as
  P126's ptr^[i] codegen fix — missing AST_DEREF handling in
  related codegen paths. The 2 remaining `ptrStr` warnings from
  P125 surfaced the existence of the class; P126 fixed one path
  (gen_lvalue_addr for AST_ARRAY_ACCESS →AST_DEREF), P127g fixes
  a sibling path (lvalue_record_type for AST_ARRAY_ACCESS where
  base is AST_DEREF). Likely more sibling sites remain to
  harden — watch for future miscompiles around `ptr^[i].field`
  constructs.

### P128c (earlier this session) — Bug B shipped: two structural fixes retire the 10204 cascade.

- **P128c.1 — Pascal global-layout fix for queue heads**. Apple's asm
  treats `fwd_BlockQ` / `fwd_ReadyQ` as 2-field head structs and reads
  the prev/tail pointer at `PREV_SCH(head) = 4(head)`. That requires
  `bkwd_BlockQ` at `fwd_BlockQ + 4` (higher address = less-negative
  A5-offset). Our PASCALDEFS-pin table in `src/toolchain/pascal_codegen.c`
  had `bkwd_*` at `fwd_* - 4`. Fix: change pin offsets so
  `bkwd_ReadyQ = -1112` (PFWD_REA + 4) and `bkwd_BlockQ = -1104`
  (PFWD_BLO + 4). Now Blocked-queue init correctly sets
  `bkwd_BlockQ := fwd_BlockQ` at the address asm reads from.
- **P128c.2 — Pascal codegen fix: byte args to callee-clean procs**.
  Apple's Pascal-to-asm calling convention places 1-byte arguments
  (byte/char/bool/enum/subrange) in the HIGH byte of a 2-byte stack
  slot, so the asm's `MOVE.B (SP)+,Dn` (which reads the byte at
  logical SP on big-endian 68000) gets the correct value. Our
  codegen pushed byte-args via `MOVE.W D0,-(SP)` which puts the
  value in the LOW byte, so `MOVE.B (SP)+,D1` in QUEUE_PR's
  `MOVE.B (SP)+,D1 ; get queue` read 0 for every `Blocked`
  (enum=1) — QUEUE_PR silently took the Ready path instead of
  the Blocked path, so `Block_Process → Queue_Process(pcb, Blocked)`
  was a no-op. Fix: `pascal_codegen.c` push site for callee-clean
  callers now emits `MOVE.B D0,-(SP)` ($1F00) when the param's
  actual type size is 1 and the kind is byte/char/bool/enum/
  subrange. On m68k, predec byte-size on A7 decrements by 2 for
  alignment and writes the byte at the low-addr byte (= HIGH byte
  of the word slot). Retires at both PROC_CALL and FUNC_CALL
  push sites.

**Observed behaviour post-P128c**: 10204 completely eliminated.
Boot now correctly blocks kern, dispatches MemMgr via the real
scheduler, crashes in Initiate's `LINK A6,#-6; UNLK A6; RTS`
because RTS reads USP=$F7FFE0 (user-view of MemMgr stack) which
is uninitialized — Build_Stack wrote `@ExitSys` via seg 104
(kernel-view) to a different physical page than seg 123
(user-view) points at. SYSTEM_ERROR(10201) at hard_excep+646.
This is Bug C from the P128b handoff.

Milestones regressed 27→26 (last checkpoint: MEM_CLEANUP) because
PR_CLEANUP was previously reached only via the 10204 unwind path.
With the real scheduler dispatching MemMgr, boot fails at Bug C
before PR_CLEANUP gets called. This is architectural progress —
we're failing in the correct flow, not in the wrong flow.

**Next**: Bug C — make seg 104 (minsysldsnmmu) and seg 123
(stackmmu) of the same process map to aliased physical pages
when SET_LDSN programs the stack sdb. See the P128b handoff's
Layer-3 analysis for the concrete diagnosis.

**P128b investigation (2026-04-23 pm) — full root cause
found, three independent bugs, all structural (no suppression needed).
P128c shipped fixes for Bug B; Bug A no longer fires; Bug C is next.**

Earlier P128 hypothesis ("both crashes are the same MMU-aliasing bug")
was wrong. P128b probes confirm the MMU programming for MemMgr is
actually correct: `Map_Syslocal(MemMgr) → PROG_MMU(103, 0, 1, 0)`
reprograms ctx 1 seg 103 sor=$4DC → phys $9B800, and `Move_MemMgr →
MOVE_IT → MOVER` correctly copies MemMgr's syslocal to the new
physical location + updates sdb.memaddr. For Launch#2 (MemMgr), seg
103 sor=$F71 → phys $1EE200 and env_save.PC reads back as $055994
(Initiate) — correct. The actual bugs are:

**Bug B — Block_Process's Ready-queue unlink fails.** During
`SYS_PROC_INIT → Make_SProcess#2(Root) → ... → Wait_sem → Block_Process
(self-block)`, Apple's Pascal unlinks the kernel pseudo PCB
($CCB58E, priority 255, glob_id $ABC from INIT_PROCESS) from Ready
via `Queue_Process(pcb_ptr, Blocked)` at `source-procprims.text.unix
.txt:410`. But at Launch#1 the PCB's `next=$CCBC40` (MemMgr) and
`prev=$CC6B84` (=@fwd_ReadyQ) — still Ready-queue linkage. So
SelectProcess re-dispatches the just-blocked kernel pseudo. Cause
TBD: either QUEUE_PR dequeue step miscompiled, or a later path
re-queues it into Ready, or our PCB next/prev offsets disagree
with the compiled asm's.

**Bug A — ENTER_SC supervisor-path corrupts env_save.** Block_Process
self-block calls `Enter_Scheduler`. Apple's ENTER_SC asm
(`source-PROCASM.TEXT:57`) has a supervisor-mode fast path: `ADDQ
#4,SP ; BRA.S SCHDTRAP` that assumes an interrupt's PC+SR trap frame
already sits on SSP underneath the JSR return address. STARTUP's
init code is supervisor-mode but NOT an interrupt — there's no
pre-existing trap frame. SCHDTRAP then pops garbage bytes as PC/SR
and stores them in the current process's env_save. Direct evidence:

```
[P128] ENTER_SC#1 SR=$2704 SSP=$CBF622 ret-PC=$0121C6
  SSP top 8 longs: $000121C6 $08200700 $00CBF63E $000127AC $00CCB58E ...
[P128] SCHDTRAP#1 SR=$2700 SSP=$CBF626
  SSP top 8 longs: $08200700 $00CBF63E $000127AC $00CCB58E ...
```

After `ADDQ #4` and SCHDTRAP's A6 dance, the (SP)+ pops read
mid-data. Launch#1 then RTEs to `$070000CB` (garbage) → line-1111 →
`spurintr_trap` → `SYSTEM_ERROR(10204)`.

**Bug C — kernel-view and user-view stack segs don't alias.**
Build_Stack writes `stk_base^.start_addr := @ExitSys` via
`stk_handle.seg_ptr = $D00000` (seg 104 = minsysldsnmmu, ldsn=-2)
which maps to phys $0AF7E0. But at Launch#2, Initiate's RTS reads
the user stack at `USP=$F7FFE0` via seg 123 (stackmmu) which maps
to phys $20EDE0 — **different physical page.** Apple's design
expects the same physical RAM to be visible through the kernel-side
MMU (seg 104, used during creation) and the user-side MMU (seg 123,
used by the running process). Our emulator doesn't enforce that
aliasing when `SET_LDSN(stk_sdb, stackmmu, mmustack)` programs
seg 123. Result: RTS reads zeros → PC=0 → vector walk → 10201 at
hard_excep+646.

**Fix order (next session):**
1. **Bug B** — instrument `Block_Process → Queue_Process(Blocked)`
   post-dequeue; confirm which sub-step fails. Fix the Pascal codegen
   or the off-by-offset PCB layout assumption.
2. **Bug A** — either (a) find/fix the init-time Wait_sem call that
   hits a count=0 sem (shouldn't happen if sems are init'd to 1) so
   Block_Process never fires, or (b) synthesize a PC+SR trap frame in
   our ENTER_SC supervisor-path handler.
3. **Bug C** — make `SET_LDSN` / `MAP_SEGMENT` programming of seg 123
   mirror the physical base of the corresponding kernel-side seg so
   both LDSN views share pages.

Retire the 10204 suppression HLE (`src/lisa.c`) in the same commit as
the real fix for A+B (per `feedback_hle_layers_load_bearing.md`).

Full probe recipes, SSP dumps, PCB hex, and file/line anchors:
see `.claude-handoffs/2026-04-23-post-P128b-NEXT_SESSION.md` on the
next session (this session's handoff).

Boot progress ledger: 336 / 2153 procedures entered (15.6%). Every
kernel init milestone is green.

At `Move_MemMgr → MOVE_IT → INSERTSDB` for mmstk_sdb (or mmsl_sdb),
`c_sdb.memaddr = 4480` (> Apple's tail sentinel 4096). The freechain
walks head ($CCB034, memaddr=0) → $B4F800 (memaddr=1404) → $CCB062
(memaddr=4096, = tail) → back to head. Never finds memaddr > 4480.
Infinite loop in $046DCC..$046DFE (PC ring confirmed).

`4480 = $1180`. Write trace (`WRITE-4480` probe) confirmed the store
happens at PC $04C386 inside MOVE_IT, via GetFree's
`allocaddr := memaddr + memsize - size` line (MM4.TEXT:78). Root
cause: some f_sdb has memaddr+memsize ≥ 4486, which implies the
free pool contains blocks past 4096 despite the l_physicalmem clamp.
Still to investigate:
  1. Is AVAIL_INIT's `MAKE_FREE(freebase/mempgsize, freelen/mempgsize)`
     writing a correct size? lomem=$AB800, himem=$1F0000 should
     produce memaddr=1372, memsize=2594. Max in pool = 3966, <4096.
     But GetFree observes memaddr+memsize > 4486.
  2. Does another path extend the free chain past himem?
  3. Is GetFree itself miscompiled (reading wrong fields)?

- **P127a+b** (prior commit `6420a1a`) — **Retired the
  SYSTEM_ERROR(10101) wall.**

  - **P127a** (`src/lisa.c:1999 lisa_reset`) — phys-layout fix.
    Reset-time pre-programming had `b_syslocal = b_sgheap + l_sgheap`,
    placing seg-103's phys base at `b_sysglobal + $12E00` — INSIDE
    seg-102's 128KB phys window (`b_sysglobal..b_sysglobal+$20000`).
    Our MMU passes through all 128KB of logical offset without
    enforcing SLR page-count limit, so seg-102 logical writes past
    sgheap-end (e.g. logical `$CD2E00`) aliased to phys
    `b_sysglobal+$12E00 = b_syslocal`. P124 MakeSGSpace granted a
    4KB chunk at logical `$CD2E00` (sgheap grow-headroom), compiled
    `DISKIO` zero-filled it, and the writes obliterated
    `c_syslocal_ptr^.lbt_addr` at logical `$CE005A` → `CHK_LDSN_FREE`
    read bogus `lbt[-1]` → `errnum=304` → `SYSTEM_ERROR(10101)`.
    Fix: place `b_syslocal` at `b_sysglobal + 0x20000` so seg-102's
    full 128KB phys window doesn't overlap seg-103. This leaves
    `$D200` bytes of legitimate growth headroom for sgheap inside
    seg-102 (unused padding phys RAM, not aliased).

  - **P127b** (`src/toolchain/pascal_codegen.c:1805 gen_lvalue_addr`) —
    codegen fix for by-value record/array/string params in nested
    procs. Our calling convention passes records > 4 bytes as
    4-byte pointers (see `register_proc_sig`), but `gen_lvalue_addr`
    for a non-var param emitted `LEA offset(A6),A0` (= slot address)
    instead of `MOVEA.L offset(A6),A0` (= pointer value). In
    Build_Stack's `with stk_info do ... stk_delta`, the codegen
    computed `[static_link+20 + 24] = [+44]` directly, reading
    garbage from the frame past the stk_info pointer slot. Fix: when
    a param is non-var but type size > 4, treat it like a VAR param
    (deref the slot). This fixed Build_Stack producing `stk_base =
    $A8CEF0` (an UNMAPPED seg-84 logical addr) — correct value is
    now `$D011E0` (within MemMgr's user-stack seg-104).

  Post-P127 state: boot reaches SYS_PROC_INIT (23/27), creates
  MemMgr (Make_SProcess #1), Move_MemMgr, creates Root
  (Make_SProcess #2), FinishCreate... then something inside the
  second FinishCreate (Close_Dataseg → disk write path) calls
  `MDDF_IO` (at `$066420`), the HLE fires, and CPU runs afterward
  but stays inside hot page `$042B00` (MM_Setup). No crash, no
  halt — just doesn't return from SYS_PROC_INIT to
  `INIT_DRIVER_SPACE` (milestone 24). Investigation pending: why
  is MDDF_IO being called from Close_Dataseg when Root is
  non-resident and its seg is still being set up.

- **P126** (`e150040`) — **ptr^[i] codegen — WITH-field fallback +
  string-deref case**. In `gen_lvalue_addr` (`AST_ARRAY_ACCESS`
  → `AST_DEREF` branch), fall back to `with_lookup_field` when
  `find_symbol_any` misses — same pattern the sibling non-deref
  branch already used. Also handle pointer-to-string (1-byte
  stride for `pathnm_ptr^[i]` in UPSHIFT et al). Cut the
  compile-time ARRAY_ACCESS miss warnings **from 40 to 2**
  (the remaining 2 are `ptrStr` with a distinct upstream
  type-resolution bug where the symbol resolves as TK_INTEGER
  on one pass). Doesn't unblock 24/27 (that wall is the MMU
  timing issue — see below), but is a real structural fix that
  removes a silent miscompile class across ~36 call sites.

### The real 24/27 wall is a Pascal codegen class-of-bugs

`lbt = array[min_ldsn..max_ldsn] of relptr` where `min_ldsn=-2`.
Accessed through `c_lbt_ptr: lbt_ptr = ^lbt`. `CHK_LDSN_FREE(ldsn=-1)`
reads `c_lbt_ptr^[-1]`, which should compile to
`(c_lbt_ptr + (-1 - -2) * 4) = c_lbt_ptr + 4`. Our codegen falls
into `gen_lvalue_addr` → `AST_ARRAY_ACCESS` → `AST_DEREF` branch.
Silently defaults to `array_low=0, elem_size=2` when pointer-to-array
type chain doesn't resolve — reading `c_lbt_ptr + (-1)*2 = c_lbt_ptr - 2`
instead. That byte-pair happens to be non-zero in our build, so the
check reports "ldsn used" and Make_SProcess fails.

P125 added a one-shot warning in `pascal_codegen.c` at that branch.
Running compile surfaces **~40 sites per compile** where pointer-to-
array type resolution fails: `ptrStr`, `File_Dir`, `sSeg_Dir`,
`iUnit_Dir`, `pSeg_list`, …

**However, CHK_LDSN_FREE is not one of those 40 sites.** Deeper
probing (2026-04-20 follow-up, unshipped) shows the compiled
CHK_LDSN_FREE body correctly emits `SUBI.W #-2, D0` and `MULU #2, D1`
for `c_lbt_ptr^[ldsn]` — `array_low=-2, elem_size=2` ARE resolved
right here. The nonzero value read from `lbt[-1]` comes instead
from `c_lbt_ptr` itself being **zero** at call time. Why:
`c_syslocal_ptr^.lbt_addr` reads as zero at MAKE_SYSDATASEG entry,
even though `STARTUP.TEXT:589 lbt_addr := ord_lbt` runs and (per
a byte watchpoint on `$CE0000..+256`) writes `$00CE03F8` to
syslocal offset +90.

The reason the write doesn't persist: **POOL_INIT (PC $007E70)
writes through logical `$CE0000` before the MMU is programmed for
seg-103**. First WATCH-SMT (SMT/MMU programming) fires ~57 lines
later, at line 834. Our emulator's MMU treats unconfigured segments
as passthrough-with-`% LISA_RAM_SIZE` wrap, so writes to logical
`$CE0000+N` land at phys `$1A0000+N`. Later, the OS programs
seg-103 with `SOR=$473` → phys `$08E600`. Reads of logical
`$CE0000+N` now go to phys `$08E600+N`, which was never written.

So there are TWO independent real-fix directions for the 24/27
wall, either/both of which will land before P126 can exercise:

1. **Pascal codegen ptr^[i] type resolution** — ~40 sites fail
   silently. Even if CHK_LDSN_FREE itself is fine, anything that
   reads from a zero'd syslocal field through a broken ptr^[i]
   path compounds the bug.
2. **MMU segment-fault emulation** — on real Lisa, accessing an
   unconfigured segment faults. Apple's boot sequence presumably
   programs seg-103 before POOL_INIT runs, OR POOL_INIT's writes
   take a different path. Our MMU's silent passthrough-wrap hides
   the ordering issue and lets the OS "succeed" into corrupt
   state. Fixing this also retires P122 SEG-ALIAS-GUARD (which
   is the same class of bug: unconfigured seg masked).

**Do NOT write a scaffold HLE to side-step SYSTEM_ERROR(10101).** Per
`feedback_do_the_real_fix.md` and `feedback_real_emulator_no_semantic_hles.md`,
suppressing this error would encode nothing about the missing subsystem
— both root causes are real fixes in our own toolchain/emulator.
Tracked as P126 (codegen) and P127 (MMU segment faults) next session.

**Load-bearing scaffolds to reconsider retiring once the structural
fixes land**: P122 SEG-ALIAS-GUARD, P124 MakeSGSpace HLE. Both are
shaped like "encode what the missing subsystem does" but the real
subsystems they scaffold past are in *our* code (MMU faults + POOL
expansion via memmgr-process, which itself comes online with Phase 6).

### P122 follow-up — the handoff's "scheduler ABI" theory was wrong

The pre-P122 handoff diagnosed the `MOVEA.L 8(A6),A5` crash at
`$077524` as a Pascal→asm ABI mismatch (sibling of P115 CALLDRIVER
SP fix-up). It wasn't. That instruction is SCHDTRAP's `SETUP` block
(`source-PROCASM.TEXT:100`):

```
SETUP     MOVEA.L  SGLOBAL,A6               ; read absolute $200
          MOVEA.L  SYSA5(A6),A5             ; = MOVEA.L 8(A6),A5
          MOVEA.L  SCHDADDR(A6),A0
          JMP      (A0)
```

A6=0 at `$077524` means `SGLOBAL` at absolute `$200` is zero. Tracing
with a raw-phys-RAM poll showed phys[$200] got zeroed *just before*
PC=`$069FC6`, with last executed PC=`$066420` (= `BitMap_IO` entry).
The write went through `cpu->write8` with logical `$7A0000` — seg-61.
The MMU dump showed seg-61 configured with `SOR=$000 SLR=$700 chg=$3`
— i.e., logical `$7A0000` → **physical `$000000`** → the 1208-byte
bitmap read trampled vectors `$0`..`$4B7`, including SGLOBAL at `$200`.

### Root cause the handoff's "scheduler ABI" theory got wrong

The P121 handoff diagnosed the `MOVEA.L 8(A6),A5` crash at
`$077524` as a Pascal→asm ABI mismatch (sibling of P115 CALLDRIVER
SP fix-up). It wasn't. That instruction is SCHDTRAP's `SETUP` block
(`source-PROCASM.TEXT:100`):

```
SETUP     MOVEA.L  SGLOBAL,A6               ; read absolute $200
          MOVEA.L  SYSA5(A6),A5             ; = MOVEA.L 8(A6),A5
          MOVEA.L  SCHDADDR(A6),A0
          JMP      (A0)
```

A6=0 at `$077524` means `SGLOBAL` at absolute address `$200` is
zero. Tracing with a raw-phys-RAM poll showed phys[$200] got
zeroed *just before* PC=`$069FC6`, with last executed address
`$066420` (= `BitMap_IO` entry per `linked.map`). The write went
through `cpu->write8` with logical addr `$7A0000` — seg-61. The
MMU dump showed seg-61 configured with `SOR=$000 SLR=$700 chg=$3`
— i.e., logical `$7A0000` → **physical `$000000`** → the 1208-byte
bitmap read trampled vectors `$0`..`$4B7`, including SGLOBAL at
`$200`.

Seg-61's SOR=0 is Apple's "unallocated demand-page" state: on real
Lisa hardware the first access to such a segment faults, and the
SMT fault handler allocates a real physical frame before retrying.
Our MMU doesn't emulate segment faults — it just translates SOR=0
literally to phys 0. The guard encodes the architectural invariant
that only seg-0 legitimately addresses phys `< $400`; any seg-1+
write aliasing there is a broken translation.

### Observed behavior post-P125

- Full chain: fs_mount → real_mount → def_mount → MDDF_IO (HLE) →
  BitMap_IO (HLE, 16 × SEG-ALIAS-GUARD drops) → MDDF_IO WRITE (HLE) →
  UltraIO → DISKIO → GETSPACE fails → MakeSGSpace (P124 HLE grants
  4KB chunk) → DISKIO retries → SYS_PROC_INIT reached via 10707
  suppression unwind → Make_SProcess(MemMgr) → Get_Resources →
  Make_SysDataseg → errnum=304 → System_Error(10101) → HALT.
- Audit clean: 8876/8876 symbols resolved, 0 unresolved.

**Next blocker — `ptr^[i]` codegen type-resolution (P126)**. The 10101
failure traces to a miscompile of `c_lbt_ptr^[ldsn]` in CHK_LDSN_FREE
when lbt has a negative low bound (`-2`). Our Pascal codegen at
`src/toolchain/pascal_codegen.c` (gen_lvalue_addr, AST_DEREF branch)
silently defaults `array_low=0, elem_size=2` when the pointer's
target-type chain doesn't resolve to TK_POINTER→TK_ARRAY. P125 added
a warning that surfaces ~40 sites/compile. Concrete starting points:

1. Run `./build/lisaemu --headless Lisa_Source 1 2>&1 | grep
   "ARRAY_ACCESS(ptr^\[i\])"` to enumerate every miss by symbol
   and source file.
2. For each, inspect the Pascal source to find the declared type
   chain (e.g., `lbt_ptr = ^lbt; lbt = array[-2..16] of relptr`)
   and figure out why `find_symbol_any` / `ptr_sym->type->base_type`
   doesn't end up as TK_ARRAY. Likely candidates: typedef chain
   not fully followed; forward declarations; type definitions
   defined in one unit but referenced from another without
   cross-unit type resolution.
3. Do NOT add a suppression for SYSTEM_ERROR(10101) — see
   `feedback_do_the_real_fix.md`. The right fix is structural,
   in the codegen.

### IRQ scaffold summary (P120 three-step chain, still in place)

- **Step-1** (`389f489`) — `profile_t.completion_pending` fires VIA1
  CA1 via `profile_check_irq()` + main-loop `via_trigger_ca1()`.
- **Step-2** (`15163fc`) — INT1V ($64) → $3F4 sentinel with RTE stub;
  `lisa_hle_intercept` catches PC=$3F4, reads+clears VIA1 IFR.
- **Step-3** (`0c97b75`) — `DiskDriver` HLE captures the single-arg
  routine into `lisa->hle.disk_routine` (= **PARALLEL** at $074A32).
  Level1 HLE synthesizes JSR into PARALLEL with $3F8 (linker RTE
  stub) as return address.
- **P121 track 1** (`87fecec`) — Level1 HLE only JSRs `disk_routine`
  when `(IFR & VIA_IRQ_CA1) != 0` — i.e. on real CA1 completions.

End-to-end chain: CA1 edge → IFR set → update_irq → irq_via1 →
m68k_set_irq(1) → vector fetch at $64 → PC=$3F4 → Level1 HLE
clears IFR → synthesizes JSR → PARALLEL runs → RTS to $3F8 →
RTE → continue. Retires when compiled `SYSTEM.CD_PROFILE` actually
handles the IRQ end-to-end via `dinterrupt → IODONE`.

### Active infrastructure (all `#if 1`)

- HLE GET_BOOTSPACE ($200000+ MMU seg 16 pass-through).
- ldr_movemultiple MMU-translated writes.
- P110 reloc scan (75 sites × 3 drivers per boot).
- **P124 HLE MakeSGSpace** (`$012970`) — grants 4KB chunks from
  sysglobal segment headroom so compiled DISKIO's retry loop can
  succeed. Retires with Phase 6 memmgr process.
- CALLDRIVER(dinit) + CALLDRIVER(hdinit) pass-through + P115 SP fix-up.
- Phase-5 IRQ scaffold (profile completion_pending, CA1, INT1V
  sentinel, Level1 HLE with PARALLEL JSR synthesis, CA1 gate).
- **P122 SEG-ALIAS-GUARD** (`src/lisa_mmu.c` write8) — drops
  seg-1+ writes that alias into phys `< $400`.
- **P128g psio HLE** (`src/lisa.c:hle_handle_psio`) — single intercept
  covers Apple's compiled MDDF_IO / BitMap_IO / FMAP_IO / pglblio
  paths (all call psio). Respects `nbytes` arg so partial-page reads
  (e.g. MDDFdb=316 bytes, bitmap=1208 bytes) copy only the requested
  bytes via RMW on the trailing page. Retires together with the vm
  HLE once SYSTEM.CD_PROFILE handles psio→UltraIO natively + IRQ
  completion is fully wired.

### Active HLEs (remaining scaffolds)

- CALLDRIVER for fnctn=7 (dcontrol dcode=20 spare-table health).
- CALLDRIVER for fnctn=12 (DATTACH) — no-op.
- **psio HLE** — covers all FS block I/O. Retires with CD_PROFILE.
- **vm HLE** — covers FS sub-block reads. Retires with CD_PROFILE.
- **slist_io HLE** — reads sentry directly from disk image. Retires
  when vm HLE covers the specific slist_io indexing path.
- **MakeSGSpace HLE** — pool-expansion work normally done by memmgr
  process. Retires with Phase 6.
- SYSTEM_ERROR(10707) suppression — NO LONGER FIRES during boot
  (P128g made FS_INIT complete naturally). Kept as safety net for
  unusual boot paths; can be retired once we've verified no boot
  scenario still needs it.

## Reference: previous session history

Full per-session history (P4–P120) lives in `.claude-handoffs/` (one
file per handoff, archived at session start). If you need the
reasoning behind a specific HLE, codegen fix, linker rule, or
structural Pascal bug — search by date or topic there, not here.
CLAUDE.md is a living architectural doc, not an engineering log.

Key milestones for orientation:
- **P4–P78**: cumulative Pascal-codegen structural fixes (record
  layout, variant records, packed bit-packing, WITH blocks, goto,
  CASE multi-labels, strings, calling conventions, cross-unit type
  resolution, two-pass types pre-pass).
- **P80–P89**: static-link ABI for sibling-nested proc calls;
  8-char significant identifiers; iterative record pre-pass; packed
  boolean bit-packing; MERGE_FREE guard; linker LOADER-yields-to-
  non-LOADER rule; AST_WITH handles AST_FIELD_ACCESS.
- **P90 (Phase 1 complete)**: PMEM + ProFile-on-paraport boot
  device; five structural codegen fixes (signed byte sign-extension,
  packed variant boolean width, packed whole-byte field offset
  advance, aggregate record/array assignment block-copy, lvalue_addr
  A0 save across array index); PRAM stub `pm_slot=10`.
- **Phase 2 step 3d (`5b082e3`..`9b3afa4`)**: real Lisa filesystem
  in `diskimage.c` — fsversion 15, 14-byte s_entry records, LDHASH-
  placed centry catalog, per-file filemaps, 20-byte on-disk tags.
- **Phase 2 step 4a (`d5e50e1`)**: smart ENTER_LOADER HLE
  dispatching loader calls against ldr_fs. 21→24 milestones; 10738
  retired.
- **P91–P104**: integer-param sign-extension on push (P91); array-
  of-record field type resolution (P92); WITH-record shadowing of
  same-named globals (P93); PROFILE-only filename redirect (P94);
  signed `byte` subrange (P95); OBJ endblock offset (P96); WITH
  array-deref record_type (P97); string-compare branch
  displacements (P98); exact-match type lookup + snapshot kind
  (P101–P102); LCP-based proc resolution + 8-char rule (P102b/c);
  INTERFACE sig preserved across asterisk-comment body decl (P103);
  MDDF_IO HLE bypassing psio chain (P104).
- **P105–P108**: nested WITH with pointer-deref of outer field;
  WITH fields shadow globals on read path; comma-separated record
  fields split per-name; LENGTH() address-of for non-pointer
  strings (P105). CALLDRIVER dcontrol dcode=20 healthy spare-table
  reply + WITH-field array-of-longint read size (P106). MDDF_IO
  HLE retired (P107). Integer→longint arg widening EXT.L on push
  (P108).
- **P109–P115**: OBJ reloffset / driver relocation investigation.
  P110 reloc scanner for inter-proc absolute-long
  JSR/JMP/LEA/PEA/MOVE.L#imm (115 sites/load × 3 loads). P111
  GetFree returns kernel-globals pages — replaced by P112 HLE
  GET_BOOTSPACE returning seg-16 pass-through pages. P115
  CALLDRIVER(dinit) pass-through with `MOVE.W (SP)+,D0` SP fix-up
  at dynamic CALLDRIVER post-JSR (compensates for our caller-clean
  Pascal RTS vs Apple's callee-clean asm-ABI).
- **P116–P117**: MDDF_IO HLE re-disabled (post-P115 unreachable on
  current path); HDINIT passthrough crash root-caused to P110
  scanner false positives — `is_abs_long` gate enforced (115→75
  sites/load), HDINIT passthrough enabled.
- **P118**: P110 scanner narrowed (drop `$203C`/`$2F3C` from abs-
  long whitelist — they carry data literals); CALLDRIVER post-JSR
  fix-up address made dynamic via `boot_progress_lookup`. Boolean
  NOT codegen investigated, not shipped (regresses elsewhere).
- **P119**: BitMap_IO + MDDF_IO layered-HLE path hits open_sfile
  wall in fs_mount; can't advance without IRQ completion.
- **P120**: Phase-5 IRQ-driven I/O completion scaffold — profile
  completion → VIA1 CA1 → INT1V sentinel → Level1 HLE JSRs
  PARALLEL.
- **P121**: narrow boolean-NOT codegen fix (WITH-scope boolean
  field) + re-enabled MDDF_IO/BitMap_IO HLEs; UltraIO reached for
  the first time in project history. P121-track-1 added CA1 gate
  so spurious Level1 IRQs short-circuit instead of dispatching
  PARALLEL on garbage. 23/27 → 22/27 (predicted regression inside
  FS_INIT).
- **P122**: SEG-ALIAS-GUARD in `lisa_mem_write8`. Root-caused an
  A5-drop / SCHDTRAP null-deref crash to BitMap_IO's 1208-byte write
  through logical `$7A0000` (seg-61 configured with SOR=0 — Apple's
  demand-page unallocated state) aliasing to phys `$0`, wiping the
  vector table + SGLOBAL. Guard drops writes where logical ≥ `$20000`
  lands at phys `< $400`.
- **P123**: diagnostics only. Traced 22/27 FS_INIT wall to
  DISKIO → GETSPACE(106) failing (sysglobal pool exhausted).
- **P124**: HLE MakeSGSpace at `$012970`. Directly runs pool-
  expansion work the memmgr process would have done — allocates 4KB
  from segment headroom, links into freelist, clears `grow_sysglobal`,
  sets `no_space=false`. Restores 22/27 → 23/27 SYS_PROC_INIT.
- **P125 (current)**: diagnostics only. Traced 23/27 → 24/27 wall
  to `CHK_LDSN_FREE` reading `c_lbt_ptr^[-1]` on a Pascal
  `array[-2..16]`. Codegen silently defaults `array_low=0,
  elem_size=2` in the `ptr^[i]` branch when the pointer-to-array
  type chain doesn't resolve — reads wrong offset, wrong stride.
  ~40 sites/compile exposed. Next session (P126): fix the
  structural codegen class-of-bug in `pascal_codegen.c`.

## On-disk format reference (load-bearing for Phase 2/3 work)

**Filesystem layout** (from `source-ldlfs.text.unix.txt`):

- **MDDF at `firstblock`** with `mddfaddr=0`, `datasize`, `slist_addr`,
  `slist_packing`, `rootmaxentries`, `map_offset`, `smallmap_offset`,
  `fsversion`, `tree_depth`, `root_page`, `rootsnum`.
- **S-file descriptors** (`s_entry`) packed in the slist area — holds
  `filesize`, `hintaddr`, `fileaddr` per file.
- **Catalog**:
  - `fsversion < 16`: flat, LDHASH-probed (Fibonacci-ish), `centry`
    records with `name` / `cetype` / `sfile`.
  - `fsversion >= 16`: B-tree with `Make_Key` + `Search_Node`.
- **Per-file filemap** at `hintaddr + map_offset`: array of
  `(address, cpages)` tuples describing which blocks hold each page.
- **Each block read** returns 512 data bytes + a 24-byte pagelabel (hdr)
  carrying `fwdlink` for filemap chaining.

**Driver file format** (from `source-STARTUP.TEXT:1207` LOADCD):

```
  byte:   block type (codeblock=$FF85, endblock=$FF81, others skipped)
  byte:   counter (must be 0)
  word:   blocksize (BE)
  [if codeblock]:
    long:  reloffset
    bytes: blocksize - 8 bytes of code
```
Loop until `header == endblock`.

**Per-block tag file IDs** (from `_inspiration/lisaem-master/src/tools/src/lisafsh-tool.c:66`):

| Tag ID | Meaning |
|---|---|
| `0xAAAA` | Boot sector — block 0 holds the LDPROF stub |
| `0xBBBB` | OS loader — blocks holding the loader body |
| `0x0000` | Free block |
| `0x0001` | MDDF (Master Directory Data File, aka superblock) |
| `0x0002` | Free-space bitmap |
| `0x0003` | S-records (slist / file descriptors) |
| `0x0004` | Directory (catalog) |

ProFile blocks: 512 data + 20-byte tag (lisaem `libdc42.h`) +
4 bytes driver-added padding = 24-byte pagelabel surfaced to FS.

## Phase 2 plan (status)

1. ✅ Add `SYSTEM.BT_PROFILE` second compile target (`3ef159d`).
2. ✅ Iterate `ALL_TARGETS[]`; per-target artifacts; primary mirrors to
   bundle root for backward-compat.
3. ✅ **Step 3a–3c**: strict module filter, boot_entry placement, boot
   track carries the BT_PROFILE blob (`f9388e5`, `84550ef`, `4764d4c`).
4. ✅ **Step 3d**: real filesystem (5b082e3..9b3afa4) — fsversion 15,
   slist, LDHASH catalog, per-file filemaps, 20-byte on-disk tags.
5. ✅ **Step 4a (`d5e50e1`)**: smart ENTER_LOADER HLE dispatching against
   ldr_fs. 10738 dead.
6. ⏳ Step 4b: relink BT_PROFILE at high RAM and run real compiled LOADER
   (blocked on `compile_target_t.base_addr` linker support).
7. ✅ Step 9: SYSTEM.CD_PROFILE compiled, OBJ-wrapped, placed on disk.
8. ✅ Step 10: driver loaded into user RAM via LOADCD; relocation scanner
   rebases inter-proc absolutes; compiled driver dispatched via
   CALLDRIVER passthrough.
9. ⏳ Step 11: retire 10738/10741/10707 suppressions as the underlying
   subsystems come online (Phase 4/5 work).

## Active HLE bypasses (to remove as phases land)

- **P27** Unmapped segment writes dropped (generic MMU safety net)
- **P33** REG_OPEN_LIST: mounttable chain walk
- **P34** excep_setup: wild `b_sloc_ptr`
- **P35** SYS_PROC_INIT: full system-process creation
- **P36** MEM_CLEANUP: fires milestone, bypasses body
- **P37** FS_CLEANUP: fires milestone, bypasses body
- **P38** PR_CLEANUP: fires milestone, bypasses body
- **P42** Dynamic HLE lookup via `boot_progress_lookup` (cached per `g_emu_generation`)
- **P78** Signal_sem HLE guard + RELSPACE guard
- **ENTER_LOADER smart HLE** (`src/lisa.c:lisa_hle_enter_loader`) —
  dispatches loader calls against `ldr_fs`. Replaces the older
  `m68k.c:4846` pop+RTS bypass.
- **CALLDRIVER HLEs** (`src/lisa.c:hle_handle_calldriver`):
  - dinit/hdinit pass-through to compiled driver (with P115 SP fix-up)
  - dcontrol dcode=20: healthy spare-table reply
  - DATTACH (fnctn=12): no-op
- **GET_BOOTSPACE** (`src/lisa.c:lisa_hle_intercept`) — returns pages
  in MMU seg 16 (pass-through). Retires when MM4.GetFree returns
  high-memory pages.
- **P128g psio HLE** (`src/lisa.c:hle_handle_psio`, `#if 1` since P128g)
  — single intercept replacing both MDDF_IO and BitMap_IO HLEs.
  Respects `nbytes` arg (pre-P128g MDDF_IO/BitMap_IO HLEs wrote a
  fixed 512-byte block which overflowed the caller's 316-byte
  MDDFdb buffer into the sysglobal pool's next-chunk freelist
  metadata, setting its size field to 0 and breaking subsequent
  GETSPACE calls). Retires once compiled CD_PROFILE handles
  psio→UltraIO→DiskIO natively with IRQ completion.
- **vm HLE** (`src/lisa.c:hle_handle_vm`, `#if 1` since P128f) —
  covers sub-block reads (HENTRY_IO/FMAP_IO/pglblio paths).
- **slist_io HLE** (`src/lisa.c:hle_handle_slist_io`, `#if 1`) —
  reads sentry directly from disk image for slist indexing path
  that vm doesn't cover.
- **SEG-ALIAS-GUARD** (`src/lisa_mmu.c` `lisa_mem_write8`) — drops
  writes where logical ≥ `$20000` aliases to phys `< $400`.
  Architectural invariant: only seg-0 addresses the vector table.
  Retires when the MMU properly emulates Apple's segment-fault
  demand-paging semantics (currently we translate SOR=0 literally
  to phys 0 instead of trapping).
- **10738..10741 suppression** (`src/lisa.c:3090`) — safety net for
  alternate boot paths (PRAM failures → 10738, LOADCD failures →
  10739). Dead on the ProFile-on-paraport default path.
- **10707 suppression** (`src/lisa.c`) — masks FS_MASTER_INIT failure.
- **Phase-5 IRQ scaffold** (`src/profile.c`, `src/lisa.c`,
  `src/via6522.c`): profile `completion_pending`, CA1 trigger, INT1V
  sentinel at $3F4, Level1 HLE clears IFR + JSRs `disk_routine`
  (PARALLEL). Retires when LIBHW DriverInit body actually loads.
- `$234` fetch bypass: IPL=7→RTE (DB_INIT skip), IPL=0→execute (Lisabug)
- `$4FBC` NOP-skip: illegal opcode in Workshop init loop
- **INTSON/INTSOFF**: manages IPL for compiled OS code
- **Loader TRAP HLE**: MMU-translated reads/writes for `fake_parms`
- **Setup_IUInfo HLE**: skips INTRINSIC.LIB read loop
- **GETSPACE**: zero-fills allocated blocks (calloc semantics)

See `docs/HLE_INVENTORY.md` for the complete ledger.

## Debug infrastructure

- `DBGSTATIC` macro + `g_emu_generation`: all debug statics reset on power cycle.
- Bounded print budgets on all diagnostic output.
- `VEC-FIRST` per-vector first-fire trace, periodic DIAG frame dumps.
- 256-entry PC ring (`g_pc_ring`) for crash analysis; dumped on
  `illegal_instruction` along with regs + 32-byte stack window +
  ±32-byte PC window.
- DRV-ENTER/EXIT tracing at driver-range PC transitions (gated).
- `boot_progress_lookup(name)` public accessor for linker symbol table —
  cached per generation; used by HLE intercepts that need to find
  compiled Pascal entry points by name (since addresses shift with
  every codegen change).

## Inspiration projects

- `_inspiration/LisaSourceCompilation-main/` — 2025 working compilation
  of LOS 3.0 on real Lisa hardware. `scripts/patch_files.py` catalogs
  the source patches needed to make Apple's source cross-compile. Our
  toolchain aims to avoid needing these patches (by fixing the
  structural Pascal-codegen bugs the patches work around).
- `_inspiration/lisaem-master/` — Reference for SCC/VIA/COPS/ProFile/
  floppy emulation.

Cross-project conclusions: neither reference has a modern source-to-
bootable-image writer. LisaSourceCompilation requires manual
IUMANAGER steps on real hardware; lisaem reads DC42 images but
doesn't build them. Our Phase 2 disk-image builder is genuinely new
work (using their format specs as ground truth, not copying code).

## Lisa_Source Reference

See also:
- `docs/LISA_SOURCE_MAP.md` — complete catalog (~1,280 files)
- `docs/HARDWARE_SPECS.md` — hardware spec derived from source
- `docs/TOOLCHAIN.md` — compilation pipeline doc
- `docs/HLE_INVENTORY.md` — complete HLE ledger

Key facts:
- **Version**: Lisa OS 3.1 (Office System), circa 1983–1984
- **Languages**: Motorola 68000 assembly + Lisa Pascal (Apple's custom dialect)
- **~1,280 files** across OS kernel, 21 libraries, 13 applications, fonts, toolkit
- Contains 8 pre-compiled `.OBJ` files (68000 binaries) and 57 binary font files
- Build scripts in `LISA_OS/BUILD/` and `LISA_OS/OS exec files/` describe the full build process
- Linkmaps in `LISA_OS/Linkmaps 3.0/` show exact segment layout of every linked binary
- No pre-built ROM images or bootable disk images — everything compiles from source

## Hardware Specs (from source analysis)

| Component | Specification |
|-----------|---------------|
| CPU | Motorola 68000, 5 MHz |
| RAM | 1 MB (24-bit address bus) |
| Display | 720 × 364, monochrome bitmap |
| ROM | 16 KB at $FE0000 |
| I/O Base | $FC0000 |
| VIA1 | $FCD801 — Parallel port / ProFile hard disk |
| VIA2 | $FCDD81 — Keyboard / COPS (mouse, clock, power) |
| Video | Dual page, base at $7A000, contrast latch at $FCD01C |
| Interrupts | 7 levels (M68000 standard), VIA-based |
| Storage | Twiggy floppy, Sony 3.5" floppy, ProFile hard disk (5/10 MB) |
| Keyboard | 128 keys via COPS microcontroller, event queue |
| Mouse | Delta tracking via COPS, hardware cursor |

## Code Conventions

- **Swift**: Swift 6, `@Observable` (not ObservableObject), `@State` (not @StateObject), modern SwiftUI APIs (`.foregroundStyle`, `fileImporter`, etc.)
- **C**: C17, `-Wall -Wextra`, no external dependencies beyond SDL2 (standalone) or AppKit (Xcode)
- **Target**: Apple Silicon (arm64-apple-darwin), macOS 15+
- **`lisaOS/lisaOS/Emulator/` files are SYMLINKS** to `src/`. No copying — edit `src/` and Xcode picks it up automatically.

## Git Conventions

- No Claude attribution in commit messages.
- `Lisa_Source/` is gitignored (Apple license prohibits redistribution).
- `.claude/` is gitignored.
- Push after every commit unless told otherwise.
- No destructive git ops without explicit user permission.
