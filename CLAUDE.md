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
| 5 | **IRQ-driven I/O completion** | removes Block_Process-on-POP deadlock | In progress — IRQ scaffold complete (P120) |
| 6 | **SYS_PROC_INIT + real processes** | removes P89i + CreateProcess HLE pile | Not started |
| 7 | **Cleanup HLEs** (FS/MEM/PR_CLEANUP) | real cleanup bodies | Not started |
| 8 | **Safety-net HLEs** (REG_OPEN_LIST, excep_setup) | remove | Not started |
| 9 | **System libraries** (SYS1LIB, SYS2LIB) | new compile targets | Not started |
| 10 | **Graphics** (LIBQD, LIBTK) | new compile targets | Not started |
| 11 | **Shell (APDM)** | full desktop | Not started |
| 12 | **Apps** | LisaWrite, LisaCalc, etc. | Not started |

## Current Status (2026-04-20 post-P121 — IRQ scaffold exercised, FS_INIT wall)

**Milestones**: **22/27** FS_INIT. *Intentional* regression from
baseline 23/27 SYS_PROC_INIT — predicted by P119/P120 handoff. The
boolean-NOT codegen fix + re-enabled MDDF_IO/BitMap_IO HLEs let
real_mount carry through `MDDF_IO READ → BitMap_IO READ (1208 B) →
MDDF_IO WRITE → UltraIO entered`. Pre-P121, UltraIO bailed at
`if not blockstructured` with E_IO_MODE_BAD, so real_mount never
issued I/O. Now it does, and the IRQ scaffold is being exercised.

**P121 shipped**:

1. **Narrow boolean-NOT codegen fix** (`src/toolchain/pascal_codegen.c`
   AST_UNARY_OP / TOK_NOT, after the existing `is_boolean` checks).
   When the operand is `AST_IDENT_EXPR` whose name misses both
   `find_symbol_any` and `find_proc_sig`, AND we're inside a WITH
   block, AND `with_lookup_field` resolves it to a TK_BOOLEAN field —
   take the boolean path (`TST.W ; SEQ ; ANDI.W #1`) instead of
   bitwise `NOT.W`. Narrowest possible scope: doesn't affect any
   site where the name resolves to a global, a proc, or a non-WITH
   field.

2. **MDDF_IO + BitMap_IO HLEs re-enabled** (`src/lisa.c`, was `#if 0`).
   Bypass the psio→LisaIO→UltraIO chain by reading directly from
   the ProFile image. With the NOT fix in place, UltraIO no longer
   fires E_IO_MODE_BAD and the chain runs through all three reads.

**Observed behavior**:
- `[POST-BOOT] entered fs_mount → real_mount → def_mount → MDDF_IO
  READ → BitMap_IO READ → MDDF_IO WRITE → UltraIO @$06C406`.
- 20+ `P120 Level1 IRQ` traces firing — but with `IFR=$00 IER=$00
  CA1=0`. **Not real CA1 sources** (likely vretrace-routed Level1).
- IRQ #1 has reasonable `SP=$00CBFB7E`; IRQ #2 has `SP=$00000008`
  — catastrophic SP corruption between the first and second IRQ.
  Cascade into 18 more bogus IRQs against zero stack.
- Boot doesn't crash — 3000 frames complete cleanly. Just stuck
  in FS_INIT (wait_sem on disk request that never completes).
- Audit still clean: 8876/8876 symbols resolved, 0 unresolved.

**Next blockers — pick a track**:

1. **Spurious-IRQ source** (`src/lisa.c:lisa_hle_intercept` PC=$3F4
   handler). All Level1 IRQs after the first show IFR=0 — meaning
   VIA1 isn't asserting. Either the m68k IRQ-acceptance state is
   sticky from a previous IRQ, or vretrace is routing to Level1
   without setting IFR, or update_irq has a bug that asserts
   irq_via1 when IFR&IER==0. Fix: gate the disk_routine JSR on
   `(ifr_val & VIA_IRQ_CA1) != 0` so we only dispatch on real
   completions; let the IFR=0 case fall through to the RTE.
   Likely 5-line change but needs verification that we don't
   miss real completions queued during HLE entry.

2. **PARALLEL → process unblock** (binary-archaeology PARALLEL
   $074AA4). Even when CA1 fires legitimately, our PARALLEL
   call needs to find the matching `request` in the kernel's
   queue and signal IODONE so the wait_sem wall comes down.
   Trace what PARALLEL does on the first IRQ: does it identify
   the request? Does it call IODONE? Does any compiled scheduler
   code wake up? May need to inspect IRQ #1's stack frame
   ($CBFB7E) to see if the kernel had pushed a request handle
   that PARALLEL should consume.

3. **SP corruption between IRQ #1 and #2**. Likely caused by
   PARALLEL's first dispatch leaving the stack in a bad state,
   or a fault during PARALLEL execution that scrambles things.
   Worth dumping PC ring + reg state at IRQ #2 entry.

Track 1 is the obvious smallest-cut next step. Track 2 is the
real prize. Track 3 may resolve as a side effect of fixing 1
or 2.

- **Step-1** (`389f489`) — `profile_t.completion_pending` fires VIA1
  CA1 via `profile_check_irq()` + main-loop `via_trigger_ca1()`. Also
  fixed latent bug where BSY-transition `ifr |= 0x02` bypassed
  `update_irq` so `irq_via1` stayed stuck at 0.
- **Step-2** (`15163fc`) — INT1V ($64) → $3F4 sentinel with RTE stub;
  `lisa_hle_intercept` catches PC=$3F4, reads+clears VIA1 IFR,
  falls through to RTE. Root-caused that LIBHW.DriverInit ($070A4A)
  is a TRAP #5 trampoline whose body never loads, so installing a
  sentinel at INT1V was the minimum viable fix.
- **Step-3** (`0c97b75`) — `DiskDriver` HLE at `boot_progress_lookup
  ("DiskDriver")` (dynamic addr, currently $070A56) captures the
  single-arg routine into `lisa->hle.disk_routine`. Level1 HLE
  extended: after IFR clear, synthesize a JSR into `disk_routine`
  by pushing $3F8 (linker RTE stub) as the return address. The only
  registered routine is **PARALLEL** ($074A32) — a kernel-local
  Level-1 IRQ dispatcher that walks `configinfo[bootdev]^` and
  JSRs the compiled driver's PRODRIVER entry.

End-to-end chain verified via the step-2 synth test (gated `#if 0`
in the shipped build): CA1 edge → IFR set → update_irq → irq_via1
→ m68k_set_irq(1) → CPU accepts IRQ → vector fetch at $64 → PC=$3F4
→ Level1 HLE clears IFR → synthesizes JSR → PARALLEL runs
(confirmed by its `MOVE.B #$3B,IER` clearing IER CA1 bit $02→$00) →
RTS to $3F8 → RTE pops IRQ frame → continue.

**Next blocker → trigger a real ProFile transaction**: the IRQ
scaffold is now armed end-to-end, but baseline never initiates a
VIA1 transaction — compiled hdinit only reads config registers, and
UltraIO's `if not blockstructured` buggy NOT.W guards real_mount
away from issuing a disk I/O. Two tracks converge here:

1. **P121 — boolean NOT codegen fix**. Root cause from prior P118
   investigation: WITH-scope bare-ident booleans hit bitwise
   `NOT.W` which turns `0x0001` into `0xFFFE` (still TRUE).
   `NEW_DEVICE` compiles `blockstructured := (devt = diskdev)` to
   `SEQ D0 ; NEG.B ; ANDI.W #1` → stores 0x0001 for TRUE.
   `UltraIO`'s `if not blockstructured` then does `NOT.W ; EXT.L
   ; TST.L` → 0x0001 → 0xFFFE → still nonzero → check fires for
   every disk read. Two fix paths:
   - (a) Boolean-aware NOT: extend `is_boolean` gate in
     `pascal_codegen.c` AST_UNARY_OP/TOK_NOT so it triggers when
     operand is `AST_IDENT_EXPR` resolving via `with_lookup_field`
     to TK_BOOLEAN. P118 attempt regressed elsewhere; needs
     narrower scoping (per-site WITH-field check).
   - (b) Apple-convention boolean storage: change `SEQ ; NEG.B ;
     ANDI.W` to `SEQ ; EXT.W` so TRUE = 0xFFFF. Wider-reaching;
     needs audit of `ord(boolean)`, `=#1` comparisons, etc.

2. **Re-enable layered HLEs**: `MDDF_IO` (`src/lisa.c:3976`) and
   `BitMap_IO` (`src/lisa.c:3985`), plus the narrow NOT fix for
   `blockstructured` once P121 scopes it. Per
   `feedback_hle_layers_load_bearing.md`, these three changes land
   together with the IRQ-completion scaffold retirement plan.

Once a transaction fires, `profile.completion_pending → VIA1 CA1 →
Level1 HLE → PARALLEL → compiled driver's dinterrupt → IODONE →
scheduler unblocks`. Phase-5 done; MDDF_IO/BitMap_IO HLEs retire in
the same commit that flips them on and proves the path.

**Active infrastructure (all `#if 1`):**
- HLE GET_BOOTSPACE ($200000+ MMU seg 16 pass-through).
- ldr_movemultiple MMU-translated writes.
- P110 reloc scan (75 sites × 3 drivers per boot, post-P118 narrowing).
- CALLDRIVER(dinit) + CALLDRIVER(hdinit) pass-through + P115 SP fix-up.
- Phase-5 IRQ scaffold: profile completion_pending, CA1 trigger,
  INT1V sentinel, Level1 HLE with PARALLEL JSR synthesis.

**Active HLEs (remaining scaffolds):**
- CALLDRIVER for fnctn=7 (dcontrol dcode=20 spare-table health).
- CALLDRIVER for fnctn=12 (DATTACH) — no-op.
- MDDF_IO HLE (gated `#if 0` — pending IRQ transaction proof).
- BitMap_IO HLE (gated `#if 0` — pending IRQ transaction proof).
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
- **P120 (current)**: Phase-5 IRQ-driven I/O completion scaffold —
  see "Current Status" above.

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
- **MDDF_IO + BitMap_IO** (`src/lisa.c`, gated `#if 0`) — bypass psio
  chain by reading directly from disk image. Re-enable + retire once
  IRQ-completion path proves through.
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
