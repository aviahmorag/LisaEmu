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
| 3 | **First driver file (SYSTEM.CD_PROFILE)** | source-PROFILE.TEXT compiled as .OBJ, placed on disk with catalog entry | Not started |
| 4 | **MDDF / disk-image full FS** | removes 10707 suppression; real FS_INIT / FS_MASTER_INIT | Not started |
| 5 | **IRQ-driven I/O completion** | removes Block_Process-on-POP deadlock | Not started |
| 6 | **SYS_PROC_INIT + real processes** | removes P89i + CreateProcess HLE pile | Not started |
| 7 | **Cleanup HLEs** (FS/MEM/PR_CLEANUP) | real cleanup bodies | Not started |
| 8 | **Safety-net HLEs** (REG_OPEN_LIST, excep_setup) | remove | Not started |
| 9 | **System libraries** (SYS1LIB, SYS2LIB) | new compile targets | Not started |
| 10 | **Graphics** (LIBQD, LIBTK) | new compile targets | Not started |
| 11 | **Shell (APDM)** | full desktop | Not started |
| 12 | **Apps** | LisaWrite, LisaCalc, etc. | Not started |

## Current Status (2026-04-18, Phase 2 scoping)

**Milestones**: 21/27 kernel checkpoints reached natively (peak baseline
is 27/27 with more HLEs in place; post-P90 the peak trades milestones
for native Phase-1 correctness).

**Phase 1 complete (P90 session):** Five structural Pascal codegen
fixes landed, making `FIND_PM_IDS` return `true` natively for the
PRAM-configured ProFile-on-parallel-port boot device. The 10738
suppression no longer load-bears at INIT_BOOT_CDS. See commits
`72eb79f`, `8a254b1`, `fb17f53` and `.claude-handoffs/2026-04-18-P90-complete-NEXT_SESSION.md`
for the line-by-line detail of:

1. Signed byte-subrange sign-extension after byte reads.
2. Packed variant records: boolean packs to 1 byte when variant or
   Tnibble is present, so arms align.
3. Packed whole-byte fields: `offset += fs` so `variant_max_end` + final
   `t->size` don't miss the last field.
4. Aggregate record/array assignment via `MOVE.B (A0)+,(A1)+ / DBRA`
   block-copy loop when either side > 4 bytes.
5. `gen_lvalue_addr` saves A0 across the index expression in
   AST_ARRAY_ACCESS so WITH-field indices don't stomp the base.
6. PRAM stub now encodes `pm_slot=10` (internal `cd_paraport`) with
   corrected 32-word XOR checksum.

**Phase 2 in progress:** Step 3d (disk layout) complete as of
`9b3afa4` — the disk image now carries a real Lisa filesystem the
compiled LOADER can walk. Current blocker is now purely step 4:

- Our `ENTER_LOADER` HLE in `src/m68k.c:4846` pops args and RTSes —
  `fake_parms.result` is uninitialized stack garbage. Until this HLE
  comes out and the compiled LOADER actually takes over, LD_OPENINPUT
  still short-circuits.
- We don't yet compile source-LOADER.TEXT or source-ldlfs.text as the
  active boot-track loader (SYSTEM.BT_PROFILE links them but
  LOADER.ldrtrap isn't wired to $204 at runtime).

**The on-disk format the real loader expects** (from `source-ldlfs.text.unix.txt`):

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

**The on-disk format of a driver file** (from `source-STARTUP.TEXT:1207`
LOADCD):

Repeating block structure, each block:
```
  byte:   block type (codeblock=$FF85, endblock=$FF81, others skipped)
  byte:   counter (must be 0)
  word:   blocksize (BE)
  [if codeblock]:
    long:  reloffset
    bytes: blocksize - 8 bytes of code
```
Loop until `header == endblock`.

**Per-block tag file IDs** (ground truth from `_inspiration/lisaem-master/src/tools/src/lisafsh-tool.c:66`):

Every disk block carries a small tag (separate from its 512-byte data
payload) whose first word is a **file ID** marking the block's role:

| Tag ID | Meaning |
|---|---|
| `0xAAAA` | Boot sector — block 0 holds the LDPROF stub |
| `0xBBBB` | OS loader — blocks holding the loader body |
| `0x0000` | Free block |
| `0x0001` | MDDF (Master Directory Data File, aka superblock) |
| `0x0002` | Free-space bitmap |
| `0x0003` | S-records (slist / file descriptors) |
| `0x0004` | Directory (catalog) |

On real hardware, when the OS formats a disk it copies the first 512
bytes of `system.bt_Profile` into block 0 with tag `0xAAAA`, and the
Lisa ROM's boot routine reads block 0, verifies the tag, and jumps to
the code. The `FS_Utilities()` syscall also exposes this write-boot-
block operation at runtime. Our Phase 2 disk-image builder must do
this at image-build time since we don't run the Lisa OS to format our
own disk.

**Tag-size discrepancy to resolve**: lisaem's `libdc42.h` declares
ProFile blocks as `512 data + 20 tag = 532 bytes`. Apple's
`source-ldlfs.text.unix.txt` declares `pagelabel` as `24 bytes`.
Likely reconciliation: 20-byte physical ProFile tag + 4 bytes of
driver-added padding surfaced to the filesystem as the 24-byte
pagelabel. Verify in Phase 3 when implementing layout.

**Cross-project conclusions from inspection of `_inspiration/`:**

- `LisaSourceCompilation-main/src/MAKE/ALEX-MAKE-FULLOS.TEXT` chains
  `SYSTEMOS → CDDRIVERS → BTDRIVERS → OSPROGS` — this IS the
  orchestrator our `toolchain_bridge.c` is replacing.
- Neither reference project has a modern source-to-bootable-image
  writer. LisaSourceCompilation relies on manual IUMANAGER steps on
  real Lisa hardware; lisaem reads DC42 images but doesn't build them
  from compiled source. Our Phase 2 disk-image builder is genuinely
  new work (using their format specs as ground truth, not copying
  code).
- Apple-source patch we'll still need at some point: `SOURCE-PROFILE`
  disk-size cap `30000 → 500000` (to allow >16MB ProFile volumes).
  Not needed for minimal Phase 2 boot; note for later.
- Pattern to steal from lisaem: sorted-tag-iteration at
  `lisafsh-tool.c:689` — grouping blocks by `(fileID, abs_sector,
  next_link)` during layout is a clean design for Phase 3.

**Phase 2 plan (in order):**

1. ✅ Add `SYSTEM.BT_PROFILE` as a second compile target in
   `compile_targets.c` — matching Apple's `ALEX-MAKE-BTPROFILE.TEXT`
   9-module list (LDPROF + LDASM + PROF + SERNUM + LDUTIL + LOADER +
   LDLFS + PASMATH + OSINTPASLIB). Commit `3ef159d`.
2. ✅ Teach `toolchain_bridge.c` to iterate `ALL_TARGETS[]` — both
   targets now compile + link each iteration; per-target artifacts
   land under `<bundle>/<target->name>/linked.bin` + `.map` +
   `hle_addrs.txt`. Primary target (SYSTEM.OS) mirrors to bundle
   root for backward-compat with main_sdl.c + the lisaOS app.
   Both targets currently produce identical linked.bin because we're
   in LOOSE compile mode — strict module filtering is a Step 3-era
   concern. No boot regression (same 27/29 milestones).
3. ✅ **Step 3a–3c**: strict module filter, boot_entry placement,
   boot track carries the BT_PROFILE blob. Commits `f9388e5`,
   `84550ef`, `4764d4c`.
4. ✅ **Step 3d1**: MDDF fsversion 17 → 15 (flat-hash catalog
   path). Commit `5b082e3`.
5. ✅ **Step 3d2**: real slist (14-byte s_entry records, two
   packed 512-byte pages, sfiles 0..71). Commit `f0be36b`.
6. ✅ **Step 3d3+3d4**: LDHASH-indexed centry catalog + per-file
   filemap pages. sfile 3 = rootcat; sfiles 4+ = user files.
   Filemap format matches `source-sfileio.text:66-71`. Commit
   `6d97cda`.
7. ✅ **Step 3d5**: on-disk 20-byte tag = first 20 bytes of the
   24-byte pagelabel (`source-DRIVERDEFS.TEXT.unix.txt:207`); real
   fwdlink chaining now encoded for all multi-block extents
   (slist, rootcat, file data). Commit `9b3afa4`.
8. **Step 4** (next): bootrom-time placement — load LOADER into
   low RAM at boot, set `[$204] = @ldrtrap` so `ENTER_LOADER`
   lands in the real compiled LOADER instead of the C HLE. Remove
   the `ENTER_LOADER` HLE in `src/m68k.c:4846` in the same commit.
9. Add SYSTEM.CD_PROFILE as a third compile target (source-PROFILE.TEXT +
   source-PROFILEASM + PROF_INIT + PROF_DOWN).
10. Place the linked driver `.OBJ` on disk via `disk_add_file(db,
    "system.cd_profile", ...)`.
11. Remove the 10738 suppression in `src/lisa.c:2886` in the same
    commit as step 10.

Steps 1–8 are the heavy lift; Phase 2 is mostly done. Steps 9–11
are pattern-matching after that.

## Reference: previous session history

Full per-session history lives in `.claude-handoffs/` (one file per
handoff, archived at session start). If you need the reasoning behind
a specific HLE, codegen fix, linker rule, or structural Pascal bug —
search by date or topic there, not here. CLAUDE.md is a living
architectural doc, not an engineering log.

Key session highlights for quick reference:

- **P4–P78**: cumulative Pascal-codegen structural fixes (record
  layout, variant records, packed bit-packing, WITH blocks, goto,
  CASE multi-labels, strings, calling conventions, cross-unit type
  resolution, two-pass types pre-pass).
- **P80–P83**: static-link ABI for sibling-nested proc calls (major
  ABI fix); 8-char significant identifiers; iterative record pre-pass;
  packed boolean bit-packing; MERGE_FREE guard.
- **P84–P88**: boolean NOT for AST_FIELD_ACCESS; byte-sized params at
  offset+1; P86 linker-CONST and A5-pin relocation fix; DEL_MMLIST empty-
  list HLE guard.
- **P89**: P89d/e scheduler static-link dispatch fixes; linker LOADER-
  yields-to-non-LOADER rule for duplicate ENTRY symbols; find_proc_sig
  resolution tiering; AST_WITH handles AST_FIELD_ACCESS record
  expression.
- **P90 (2026-04-18):** Phase 1 PMEM complete — five structural
  codegen fixes above.
- **Phase 2 Step 3d (2026-04-18 PM, commits `5b082e3`..`9b3afa4`):**
  full real Lisa filesystem now written by `diskimage.c` —
  fsversion 15 (flat hash), 14-byte s_entry records in 2 slist
  blocks, 64-slot LDHASH-placed centry catalog across 7 pages,
  per-file filemap pages, and 20-byte on-disk tags matching the
  first 20 bytes of the real 24-byte pagelabel (with fwdlink
  chaining). Headless test still at 27/29 milestones — blockers
  are now all in step 4 (remove ENTER_LOADER HLE and wire the
  compiled LOADER).

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
- **ENTER_LOADER** (`src/m68k.c:4846`) — pops args + RTS; no real loader runs
- **10738..10741 suppression** (`src/lisa.c:2886`) — masks `LD_OPENINPUT` garbage return
- **10707 suppression** (`src/lisa.c`) — masks FS_MASTER_INIT failure
- `$234` fetch bypass: IPL=7→RTE (DB_INIT skip), IPL=0→execute (Lisabug)
- `$4FBC` NOP-skip: illegal opcode in Workshop init loop
- **ProFile HLE**: intercepts CALLDRIVER, reads from disk image
- **INTSON/INTSOFF**: manages IPL for compiled OS code
- **Loader TRAP HLE**: MMU-translated reads/writes for `fake_parms`
- **Setup_IUInfo HLE**: skips INTRINSIC.LIB read loop
- **GETSPACE**: zero-fills allocated blocks (calloc semantics)

See `docs/HLE_INVENTORY.md` for the complete ledger.

## Debug infrastructure

- `DBGSTATIC` macro + `g_emu_generation`: all debug statics reset on power cycle.
- Bounded print budgets on all diagnostic output.
- `VEC-FIRST` per-vector first-fire trace, periodic DIAG frame dumps.
- 256-entry PC ring for crash analysis.
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
