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

## Phases

Each phase compiles the next real component and removes whatever HLE was
scaffolding the gap. HLE removals happen **only** in the same commit as
the real replacement.

| # | Phase | Target Real Component | Status |
|---|---|---|---|
| 1 | PMEM + boot-CD selection | Native `FIND_PM_IDS` via PRAM stub | ✅ Done (P90) |
| 2 | Real LOADER + filesystem | source-LOADER.TEXT + ldlfs + real MDDF/slist/catalog/filemap on profile.image | In progress |
| 3 | First driver file (SYSTEM.CD_PROFILE) | source-PROFILE.TEXT compiled, on disk, dispatched | In progress — driver loads, dispatch armed |
| 4 | MDDF / disk-image full FS | removes 10707 suppression; real FS_INIT / FS_MASTER_INIT | In progress — psio HLE covers MDDF_IO+BitMap_IO; FS_INIT completes naturally |
| 5 | IRQ-driven I/O completion | removes Block_Process-on-POP deadlock | In progress — IRQ scaffold complete (P120), seg-alias guard (P122); PARALLEL→IODONE pending |
| 6 | SYS_PROC_INIT + real processes | removes P89i + CreateProcess HLE pile | In progress — MemMgr dispatches and runs post-PR_CLEANUP (P128l); next: REMAP_SEGMENT loop |
| 7 | Cleanup HLEs (FS/MEM/PR_CLEANUP) | real cleanup bodies | In progress — all four cleanup milestones reach naturally |
| 8 | Safety-net HLEs (REG_OPEN_LIST, excep_setup) | remove | Not started |
| 9 | System libraries (SYS1LIB, SYS2LIB) | new compile targets | Not started |
| 10 | Graphics (LIBQD, LIBTK) | new compile targets | Not started |
| 11 | Shell (APDM) | full desktop | Not started |
| 12 | Apps | LisaWrite, LisaCalc, etc. | Not started |

## Current Status (2026-04-25 post-P128n diagnosis)

**27/27 milestones reached naturally; MemMgr LIVE post-PR_CLEANUP.** No
SYSTEM_ERROR, no hard_excep, no illegal-inst. After scheduler idle, MemMgr
dispatches, processes its first service request (CLEAR_SPACE → MOVE_SEG →
REMAP_SEGMENT), and the loop runs but does not progress.

**P128m claim debunked (P128n).** P128m said Pascal codegen miscompiles
`false` as MOVEQ #1 at MM2.TEXT:595. P128n verified that bytes
`70 01 3F 00` at $04A930 in `linked.bin` actually correspond to
`Clear_Space(holeAddr, delta_size, true)` at MM3.TEXT:609 (inside
`ALT_DS_SIZE`, $04A7E4..$04AC99 in the linker map) — not MM2.TEXT:595.
Instrumenting all four AST_IDENT_EXPR emit sites for `false`/`true`
across the whole codebase produced 36760 traces, every one correct.
**There is no codegen bug for boolean literals.** The P128n memory
file `project_clear_space_force_codegen_bug.md` is the canonical
debunk — read it before re-litigating.

**Active blocker (real one):** ALT_DS_SIZE legitimately calls
`CLEAR_SPACE(force=TRUE, hole_memaddr=$0453, space_needed=$0006)`. After
two productive MOVE_SEG iterations, iteration 3+ keeps picking the
SAME `moving_sdb=$00CC8352` (memaddr $0F5B size $0015) and REMAP_SEGMENT
becomes a no-op ($0F5B → $0F5B). The smoking gun: at iteration 3 entry
the local `free_sdb` is still $00B51600 (memaddr $058B size $09E5) —
the SAME free region as iteration 2 — even though iter 1 created a new
free at memaddr $03DC and iter 2 created another. `head_sdb.freechain.fwd`
is not being updated to point at the new lower-memaddr free regions.
P_ENQUEUE/P_DEQUEUE bytes were dumped from `linked.bin` at $046A6E /
$046AC2 and look correct; the suspicion now is INSERTSDB's
freechain-walk-and-insert, MAKE_FREE, or how head_sdb is reached
through the `c_mmrb^.head_sdb` `with`-context. Anchors: `source-MM0.TEXT`
P_ENQUEUE @203, REMOVESDB @221, TAKE_FREE @250, INSERTSDB @332,
MAKE_FREE @377; linker-map addrs $046A6E P_ENQUEUE, $046AFE REMOVESDB,
$046B58 TAKE_FREE, $046CEE INSERTSDB, $046E54 MAKE_FREE.

**Long-term MMU cleanup (deferred):** `lisa_mmu.c mmu_translate()` ignores
the SLR limit, which silently lets writes land outside sdb ranges. P128l's
loader-params fix sized the sysglobal sdb to cover sgheap precisely so we
no longer rely on this gap, but the underlying MMU should fault on
limit-exceeded accesses to match Apple's demand-paging design. See
`project_move_seg_needs_full_sdb.md`.

**For the full per-session history (P4–P128l):** see `.claude-handoffs/`.
This file tracks architecture and current state, not engineering log.

## Active HLEs and scaffolds

Each entry retires when the listed phase lands.

- **GET_BOOTSPACE** (`src/lisa.c:lisa_hle_intercept`) — returns pages in
  MMU seg 16 (pass-through). Retires when MM4.GetFree returns
  high-memory pages.
- **CALLDRIVER** (`src/lisa.c:hle_handle_calldriver`) — dinit/hdinit
  pass-through with P115 SP fix-up; dcontrol dcode=20 healthy
  spare-table reply; DATTACH (fnctn=12) no-op.
- **psio HLE** (`src/lisa.c:hle_handle_psio`, `#if 1` since P128g) —
  single intercept replacing both MDDF_IO and BitMap_IO HLEs. Respects
  `nbytes` arg via RMW on trailing partial page (the pre-P128g
  fixed-512B writes overflowed the caller's 316B MDDFdb buffer into
  sysglobal pool freelist metadata, breaking GETSPACE). Retires once
  CD_PROFILE handles psio→UltraIO→DiskIO natively with IRQ completion.
- **vm HLE** (`src/lisa.c:hle_handle_vm`, `#if 1` since P128f) — covers
  sub-block reads (HENTRY_IO/FMAP_IO/pglblio paths).
- **slist_io HLE** (`src/lisa.c:hle_handle_slist_io`) — reads sentry
  directly from disk image for indexing paths vm doesn't cover.
- **MakeSGSpace HLE** (`$012970`, P124) — pool-expansion work normally
  done by memmgr process. Retires with Phase 6.
- **SEG-ALIAS-GUARD** (`src/lisa_mmu.c lisa_mem_write8`, P122) — drops
  writes where logical ≥ `$20000` aliases to phys `< $400`. Retires
  when MMU emulates Apple's segment-fault demand-paging semantics
  (currently SOR=0 translates literally to phys 0).
- **ENTER_LOADER smart HLE** (`src/lisa.c:lisa_hle_enter_loader`) —
  dispatches loader calls against `ldr_fs`. Retires when real LOADER
  runs at high RAM.
- **Phase-5 IRQ scaffold** (`src/profile.c`, `src/lisa.c`,
  `src/via6522.c`) — profile `completion_pending` → VIA1 CA1 → INT1V
  sentinel at $3F4 → Level1 HLE clears IFR + JSRs PARALLEL. Gated on
  `(IFR & VIA_IRQ_CA1)` since P121. Retires when LIBHW DriverInit body
  loads and handles IRQ end-to-end via dinterrupt → IODONE.
- **10707 suppression** (`src/lisa.c`) — masks FS_MASTER_INIT failure.
  No longer fires during boot post-P128g (FS_INIT completes naturally);
  kept as safety net for unusual paths. Retire after verifying no boot
  scenario still needs it.
- **10738/10741 suppression** (`src/lisa.c:3090`) — safety net for
  alternate boot paths. Dead on the ProFile-on-paraport default path.
- **P27** Unmapped segment writes dropped (generic MMU safety net).
- **P33** REG_OPEN_LIST: mounttable chain walk.
- **P34** excep_setup: wild `b_sloc_ptr`.
- **P35** SYS_PROC_INIT scaffolds (largely retired post-P128a–l).
- **P36/P37/P38** MEM_CLEANUP / FS_CLEANUP / PR_CLEANUP fire-only HLEs
  — milestones now reach naturally; HLEs may still cover residual
  paths.
- **P78** Signal_sem HLE guard + RELSPACE guard.
- **GETSPACE post-RTS hook** zero-fills allocated chunks (calloc
  semantics). Gated since P128f to skip out-of-pool allocations to
  avoid amplifying allocator bugs into stack corruption.
- **INTSON/INTSOFF** manage IPL for compiled OS code.
- **Loader TRAP HLE** MMU-translated reads/writes for `fake_parms`.
- **Setup_IUInfo HLE** skips INTRINSIC.LIB read loop.
- `$234` fetch bypass: IPL=7→RTE (DB_INIT skip), IPL=0→execute
  (Lisabug).
- `$4FBC` NOP-skip: illegal opcode in Workshop init loop.

See `docs/HLE_INVENTORY.md` for the complete ledger and rationale.

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

**Per-block tag file IDs** (from
`_inspiration/lisaem-master/src/tools/src/lisafsh-tool.c:66`):

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

## Phase 2 plan

1. ✅ Add `SYSTEM.BT_PROFILE` second compile target.
2. ✅ Iterate `ALL_TARGETS[]`; per-target artifacts; primary mirrors to
   bundle root for backward-compat.
3. ✅ **Step 3a–3c**: strict module filter, boot_entry placement, boot
   track carries the BT_PROFILE blob.
4. ✅ **Step 3d**: real filesystem — fsversion 15, slist, LDHASH catalog,
   per-file filemaps, 20-byte on-disk tags.
5. ✅ **Step 4a**: smart ENTER_LOADER HLE dispatching against ldr_fs.
   10738 dead.
6. ⏳ Step 4b: relink BT_PROFILE at high RAM and run real compiled LOADER
   (blocked on `compile_target_t.base_addr` linker support).
7. ✅ Step 9: SYSTEM.CD_PROFILE compiled, OBJ-wrapped, placed on disk.
8. ✅ Step 10: driver loaded into user RAM via LOADCD; relocation scanner
   rebases inter-proc absolutes; compiled driver dispatched via
   CALLDRIVER passthrough.
9. ⏳ Step 11: retire 10738/10741/10707 suppressions as the underlying
   subsystems come online (Phase 4/5 work).

## Debug infrastructure

- `DBGSTATIC` macro + `g_emu_generation`: all debug statics reset on
  power cycle.
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
bootable-image writer. LisaSourceCompilation requires manual IUMANAGER
steps on real hardware; lisaem reads DC42 images but doesn't build them.
Our Phase 2 disk-image builder is genuinely new work (using their format
specs as ground truth, not copying code).

## Lisa_Source Reference

See also:
- `docs/LISA_SOURCE_MAP.md` — complete catalog (~1,280 files)
- `docs/HARDWARE_SPECS.md` — hardware spec derived from source
- `docs/TOOLCHAIN.md` — compilation pipeline doc
- `docs/HLE_INVENTORY.md` — complete HLE ledger

Key facts:
- **Version**: Lisa OS 3.1 (Office System), circa 1983–1984
- **Languages**: Motorola 68000 assembly + Lisa Pascal (Apple's custom
  dialect)
- **~1,280 files** across OS kernel, 21 libraries, 13 applications,
  fonts, toolkit
- Contains 8 pre-compiled `.OBJ` files (68000 binaries) and 57 binary
  font files
- Build scripts in `LISA_OS/BUILD/` and `LISA_OS/OS exec files/`
  describe the full build process
- Linkmaps in `LISA_OS/Linkmaps 3.0/` show exact segment layout of every
  linked binary
- No pre-built ROM images or bootable disk images — everything compiles
  from source

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

- **Swift**: Swift 6, `@Observable` (not ObservableObject), `@State`
  (not @StateObject), modern SwiftUI APIs (`.foregroundStyle`,
  `fileImporter`, etc.)
- **C**: C17, `-Wall -Wextra`, no external dependencies beyond SDL2
  (standalone) or AppKit (Xcode)
- **Target**: Apple Silicon (arm64-apple-darwin), macOS 15+
- **`lisaOS/lisaOS/Emulator/` files are SYMLINKS** to `src/`. No copying
  — edit `src/` and Xcode picks it up automatically.

## Git Conventions

- No Claude attribution in commit messages.
- `Lisa_Source/` is gitignored (Apple license prohibits redistribution).
- `.claude/` is gitignored.
- Push after every commit unless told otherwise.
- No destructive git ops without explicit user permission.
