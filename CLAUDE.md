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
| 4 | **MDDF / disk-image full FS** | removes 10707 suppression; real FS_INIT / FS_MASTER_INIT | Not started |
| 5 | **IRQ-driven I/O completion** | removes Block_Process-on-POP deadlock | In progress — IRQ scaffold complete (P120), seg-alias guard shipped (P122); PARALLEL → IODONE still pending |
| 6 | **SYS_PROC_INIT + real processes** | removes P89i + CreateProcess HLE pile; fixes Pascal `ptr^[i]` type-resolution class of bugs (P125 surfaced) | Not started — Make_SProcess blocked on codegen |
| 7 | **Cleanup HLEs** (FS/MEM/PR_CLEANUP) | real cleanup bodies | Not started |
| 8 | **Safety-net HLEs** (REG_OPEN_LIST, excep_setup) | remove | Not started |
| 9 | **System libraries** (SYS1LIB, SYS2LIB) | new compile targets | Not started |
| 10 | **Graphics** (LIBQD, LIBTK) | new compile targets | Not started |
| 11 | **Shell (APDM)** | full desktop | Not started |
| 12 | **Apps** | LisaWrite, LisaCalc, etc. | Not started |

## Current Status (2026-04-20 post-P125 — 23/27 SYS_PROC_INIT, stuck on `ptr^[i]` codegen)

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
  SYSTEM_ERROR(10101). **Root cause is a Pascal codegen bug**, not a
  missing subsystem — see below.

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

**Do NOT write a scaffold HLE to side-step SYSTEM_ERROR(10101).** Per
`feedback_do_the_real_fix.md` and `feedback_real_emulator_no_semantic_hles.md`,
suppressing this error would encode nothing about the missing subsystem
— it's a codegen miscompile and the fix belongs in
`src/toolchain/pascal_codegen.c`. Tracked as P126 next session.

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
- MDDF_IO + BitMap_IO HLEs (P121 re-enable).

### Active HLEs (remaining scaffolds)

- CALLDRIVER for fnctn=7 (dcontrol dcode=20 spare-table health).
- CALLDRIVER for fnctn=12 (DATTACH) — no-op.
- MDDF_IO HLE — retires when compiled CD_PROFILE handles
  psio→UltraIO→DiskIO natively with IRQ completion.
- BitMap_IO HLE — same retirement plan.
- SYSTEM_ERROR(10707) suppression → SYS_PROC_INIT unwind.

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
- **MDDF_IO + BitMap_IO** (`src/lisa.c`, `#if 1` since P121) — bypass
  psio chain by reading directly from disk image. P122 added a
  SEG-ALIAS-GUARD in `lisa_mem_write8` so BitMap_IO's writes through
  an unallocated (SOR=0) segment don't trample the vector table.
  Retires once compiled CD_PROFILE handles psio→UltraIO→DiskIO
  natively with IRQ completion.
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
