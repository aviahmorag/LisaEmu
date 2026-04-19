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

## Current Status (2026-04-19 post-P117 — scanner gate enforced, HDINIT runs natively)

**Milestones**: **23/27** SYS_PROC_INIT (stable). P117 shipped a
structural fix to the P110 driver-reloc scanner: the `is_abs_long`
gate was declared but never consulted, so 43 of every 160 scanner
hits were false positives, silently corrupting data literals inside
each compiled driver. One of those corruptions (site $20A75C,
op=$00A0, 4-byte DC-looking value $0000177C) clobbered the first
word of `MOVE.B #$3B,IER(A3)` in PROF_INIT's asm prolog — which is
exactly why P116's HDINIT passthrough attempt crashed at PC=$20A768
(ILLEGAL $00DC). With the gate now enforced, the compiled PROFILE
driver's `hdinit` body runs to completion; all 3 INIT_BOOT_CDS
devices dispatch cleanly through both fn=1 (dinit) and fn=13
(hdinit). Baseline milestone count is unchanged because hdinit's
hardware-facing VIA reads don't populate `ext_diskconfig` fully on
our emulated ProFile, but Phase-3 driver dispatch is now genuinely
operational on real compiled Apple code.

**P117 shipped**:
1. **Scanner `is_abs_long` gate enforced** (`src/lisa.c:723` inside
   the call_move case of `lisa_hle_intercept_enter_loader`). The
   flag was computed but the code patched every 4-byte operand
   whose value fell in the driver's link range, regardless of
   whether the 2-byte op looked like an abs-long instruction.
   43 false positives per load × 3 loads = 129 spurious writes
   per boot. One of them inside PROF_INIT caused the P116 crash.
   Fix: `if (!is_abs_long) continue;` before the target read.
   Patch count drops 115→75 per driver load.
2. **HDINIT passthrough enabled** (`src/lisa.c:3157`, fnctn_code
   `== HLE_DINIT || == HLE_HDINIT`). With the scanner honest, the
   same P115 SP fix-up mechanism that carries dinit through kernel
   CALLDRIVER now carries hdinit through cleanly. Fires 9× per boot
   (3 devices × [outer + nested HDINIT]).
3. **Diagnostic infrastructure** (kept, off the hot path):
   - `src/m68k.c`: `pc_ring` promoted to file-scope (`g_pc_ring`)
     so `illegal_instruction()` can dump it. Dumps 64 entries +
     bytes at last 10 driver-range PCs on fault. Also widens the
     `@PC` byte window to ±32 bytes and reports file-offset from
     driver load base when PC is in driver range.

**Observed post-P117 boot trace:**
```
P112 HLE GET_BOOTSPACE -> $200000 / $203A00 / $207400
P110: 75 sites rebased × 3 drivers   (was 115, now honestly gated)
for each of 3 devices:
  P115: CALLDRIVER(fn=1)   -> $207408 config=$CCBxxx   (dinit)
  P115: CALLDRIVER(fn=13)  -> $207408 config=$CCBxxx   (hdinit, new)
  P115 SP fix-up @ $749C2 × 3   (outer dinit + nested HDINIT + outer hdinit)
10707 suppression -> SYS_PROC_INIT 23/27 baseline tail
```

**P118 shipped (partial)**:
1. **Scanner narrowed** (`src/lisa.c` call_move reloc scan): dropped
   `$203C` (MOVE.L #imm,Dn) and `$2F3C` (MOVE.L #imm,-(SP)) from the
   abs-long whitelist. Those opcodes carry both code pointers AND
   data literals, and the scanner can't tell them apart. Concrete
   example: PROF_INIT's `num_bloks := 9720` compiles to
   `MOVE.L #$25F8,D0` — $25F8 is in the driver's link range $400..
   $3956, so the old scanner patched it to `$25F8 + fixup` = $209600,
   and `num_bloks` in `ext_diskconfig` ended up as $FFFF9600 after
   EXT.L (sign-extended `$9600` → -27648 signed longint). Now 70
   genuine JSR/JMP/PEA/LEA patches per load, zero data-literal
   corruption. Confirmed: `num_bloks` now reads correctly as 9720.
2. **P115 SP fix-up address made dynamic** (`src/m68k.c`): was
   hardcoded to `$749C2` (CALLDRIVER's post-JSR `MOVE.W (SP)+,D0`).
   Any codegen tweak that shifts SYSTEM.OS size moves this PC, so
   we now look up `CALLDRIVER` via `boot_progress_lookup` and scan
   forward for the `4E 90 30 1F` opcode pair. Cached per generation.

**P118 investigated, not shipped (regresses baseline)**:
3. **Boolean NOT codegen fix**: with-scope bare-ident booleans hit
   bitwise `NOT.W` which turns `0x0001` into `0xFFFE` (still TRUE).
   Adding a `with_lookup_field` check to the `is_boolean` gate in
   `pascal_codegen.c` correctly compiles `if not blockstructured`,
   so UltraIO no longer fires E_IO_MODE_BAD. But then bitmap_io's
   compiled path spins in `status_req` waiting for IRQ-driven I/O
   completion that our emulator's ProFile HLE doesn't generate —
   boot drops to 22/27 (FS_INIT). Confirmed via `num_bloks-set`
   PC trap: when NOT is fixed and scanner is narrowed, real_mount
   advances past the MDDF and spare checks, then bitmap_io's
   UltraIO → DiskIO chain enqueues a request and blocks forever.

**P119 investigation (layered-HLE path; NOT durable)**:
Wrote `hle_handle_bitmap_io` and parsed `bitmap_addr` + `bitmap_bytes`
from the MDDF into `ldr_fs` (`src/lisa.c`). With MDDF_IO HLE
re-enabled + BitMap_IO HLE added + narrow NOT-fix for
`blockstructured`, real_mount carries through:

  - MDDF_IO READ (1-block scaffold)
  - drivercall dcontrol spare-check (existing HLE)
  - BitMap_IO READ (new, 1208 bytes = MDDF's `bitmap_bytes`)
  - MDDF_IO WRITE (write-protect test)

real_mount then returns cleanly. fs_mount proceeds to
`open_sfile(rootsnum, ...)` — and that's where the wall hits: it
calls through slist_io / FMAP_IO / HENTRY_IO (all of which would
need HLEs) and ultimately wait_sem for request completion. Even
cutting off before open_sfile, the new real_mount progress mutates
state that our 10707 suppression unwind assumes is fresh, so boot
drops from baseline's 23/27 (clean unwind → SYS_PROC_INIT) to
22/27 (stuck in FS_INIT). Same wall P108 identified.

**Next blocker → P120 (IRQ-driven I/O completion / Phase-5)**:
The layered-HLE path can't advance past this without IRQ completion.
The structural fix is Phase-5: make our ProFile emulator generate
VIA1 interrupts when an I/O completes, and have the compiled
kernel's driver-dispatch path handle the completion on the IRQ.
Specifically:

  1. **ProFile emu IRQ line**: `src/profile.c` currently does
     polled byte-transfer via `profile_porta_read`. The real
     ProFile asserts /IRQ on command completion; our emu should
     latch a "completion pending" signal and drive VIA1 CA1/CA2
     which the kernel reads as an IRQ source.
  2. **VIA1 interrupt plumbing**: `src/via6522.c` needs to fire
     level-1 or level-2 IRQ when VIA1's IER/IFR say so. Check
     how the baseline vector table routes this (INT1V, $64).
  3. **Driver's BDR / interrupt handler**: PROFILE.TEXT's
     `BDR` (bad-disk-response) plus the asm completion code
     need to run when the IRQ fires. These are in PROFILEASM
     and probably already compile; just haven't been
     exercised.
  4. **Kernel's request completion**: the scheduler's blocked
     process (spinning in `status_req`) wakes via
     `Unblock_IO` or similar when the driver signals
     completion.

Once IRQ completion lands, the MDDF_IO / BitMap_IO HLEs retire
(both guard-gated off in `src/lisa.c` and ready to flip back to
0 in the same commit that ships IRQ completion).

Kept in-source for P120:
- `hle_handle_bitmap_io` function body (`src/lisa.c`)
- `ldr_fs.bitmap_addr` / `ldr_fs.bitmap_bytes` fields + parse
- `num_bloks-set` PC-trap diagnostic in `src/m68k.c` (useful
  for validating hdinit populates ext_diskconfig correctly
  once the full driver path works)

Meanwhile CALLDRIVER post-JSR fix-up for `dinit` still works; the
compiled driver runs hdinit to completion (now correctly
populating `num_bloks = 9720` thanks to P118 scanner narrowing).

**Previous next-blocker hypothesis (superseded)** — P118 (boolean NOT):

P118 investigation identified the real post-P117 blocker. With MDDF_IO
HLE retired and hdinit running, real_mount's `MDDF_IO → psio → lisaio
→ UltraIO` chain finally executes the compiled disk-I/O path. UltraIO
fires **E_IO_MODE_BAD (803)** at its entry check (source-VMSTUFF.TEXT:
651): `if (not blockstructured) or (hdrMode and (ptrPl = NIL))`.

Root cause (from decoded NEW_DEVICE + UltraIO in `linked.bin`):

- **NEW_DEVICE compiles** `blockstructured := (devt = diskdev)` to
  `CMP.W D1,D0 ; SEQ D0 ; NEG.B D0 ; ANDI.W #1,D0 ; MOVE.W D0,$36(A1)` —
  stores boolean as **0x0001** for TRUE.
- **UltraIO compiles** `if not blockstructured` to
  `MOVE.W $36(A0),D0 ; NOT.W D0 ; EXT.L D0 ; TST.L D0 ; BNE` — treats
  `not` as bitwise **NOT.W**, turning 0x0001 into 0xFFFE (still
  nonzero → TRUE), so the check fires for every disk read.

Two ways to fix:

1. **Boolean-aware `not`**: extend the existing `is_boolean` gate in
   `pascal_codegen.c` AST_UNARY_OP/TOK_NOT so it also triggers when
   the operand is an `AST_IDENT_EXPR` that resolves via `with_lookup_field`
   to a TK_BOOLEAN field. Tried in-session (P118 attempt) and it
   regressed to 21/27 with an ILLEGAL at PC=$CBFD98 during the
   driver-init chain — some other WITH-boolean consumer changed
   behavior when NOT.W became TST+SEQ+ANDI. Needs narrower scoping.

2. **Apple-convention boolean storage**: change the
   `SEQ ; NEG.B ; ANDI.W #1` sequence to `SEQ ; EXT.W` so TRUE stores
   as 0xFFFF. Then bitwise NOT.W works correctly. Risks: may break
   `ord(boolean)` (if codegen treats it as identity), boolean
   comparisons with literal #1, and any code that assumes
   boolean=0/1 storage. Needs audit.

Path (1) is the more local fix but needs careful scoping of
`with_lookup_field` hits. Path (2) is structurally cleaner but
wider-reaching. Investigation infrastructure (dumps of UltraIO/
NEW_DEVICE compiled bytes, devrec at MDDF_IO entry) is all in
place for next session.

## Previous Status (2026-04-19 post-P116 — MDDF_IO HLE retired)

**Milestones**: **23/27** SYS_PROC_INIT (stable). P116 confirmed the
MDDF_IO HLE is no longer reached on the current boot path — real_mount
fails BEFORE MDDF_IO would be called, so disabling the HLE is a safe
cleanup. The dinit-passthrough cascade from P115 still works; baseline
is identical.

**P116 shipped**:
1. **MDDF_IO HLE disabled** (`src/lisa.c:3948`, `#if 0`). Real_mount
   unwinds through 10707 before reaching MDDF_IO — the HLE was
   scaffolding a code path that's no longer exercised. Removing it
   doesn't change the observed trace.

**P116 investigated, reverted**:
2. **HDINIT (fnctn=13) passthrough**. Extended the dinit passthrough
   to include fnctn=13 so the compiled driver's real `hdinit` body
   would run and populate `ext_diskconfig` natively. Result: ILLEGAL
   opcode $00DC at PC=$20A768 — about 4800 bytes into the driver,
   deep in hdinit's Pascal body. Suggests either (a) another
   relocation pattern we don't scan (non-JSR/JMP/LEA/PEA absolutes),
   or (b) Pascal codegen bug specific to hdinit's internal logic
   (e.g., record-layout mismatch, pointer-arithmetic bug). Reverted.
   Tracked as P117.

**P116 finding — the real blocker post-dinit**: with dinit running
but hdinit still HLE'd ("success" without real body), ext_diskconfig
stays zeroed. real_mount's pre-MDDF checks probably fail on this
state (likely checking `blockstructured` or `fs_strt_blok`). The
10707 suppression then unwinds to SYS_PROC_INIT.

**Next blocker → P117 (debug driver hdinit body)**: to actually
populate ext_diskconfig natively, the driver's hdinit body must
run. Two sub-paths:

1. **Extend reloc scan** — dump bytes at $20A768 to see what instruction
   form caused the crash. If it's a pattern we don't handle (e.g.,
   MOVE.L #imm,An — no MOVEA.L #imm, so this pattern shouldn't
   exist in 68000; or `MOVE.L disp(PC),...` variants), extend the
   scan. Alternatively, drive toward PC-relative codegen in
   pascal_codegen.c so the driver is truly position-independent
   (Apple's approach).

2. **HLE-populate ext_diskconfig** — bypass the real hdinit body;
   have our HLE for fnctn=13 write the expected values directly
   (blockstructured=true, fs_strt_blok=<boot_track_blocks>,
   num_bloks=9720, drivetype=T_Profile, etc.) to the ext_drive_cb
   record attached to configinfo. Faster but scaffolding.

Option 1 is the structural fix and ties back to the asm68k /
pascal_codegen PC-relative emission work that'd retire the P110
reloc scanner entirely.

**Active infrastructure (all `#if 1`):**
- HLE GET_BOOTSPACE ($200000+ MMU seg 16 pass-through).
- ldr_movemultiple MMU-translated writes.
- P110 reloc scan (115 sites × 3 drivers per boot).
- CALLDRIVER(dinit) pass-through + P115 SP fix-up @ $749C2.

**Active HLEs (remaining scaffolds):**
- CALLDRIVER for fnctn=7 (dcontrol dcode=20 spare-table health).
- CALLDRIVER for fnctn=12 (DATTACH) — no-op.
- CALLDRIVER for fnctn=13 (HDINIT) — "success" without body.
- SYSTEM_ERROR(10707) suppression → SYS_PROC_INIT unwind.

**Retired in P116:**
- MDDF_IO HLE (unreachable on current boot path).

Baseline: 23/27 SYS_PROC_INIT, audit clean.

## Previous Status (2026-04-19 post-P115 — compiled driver genuinely runs)

**Milestones**: **23/27** SYS_PROC_INIT (stable). The compiled
SYSTEM.CD_PROFILE now executes NATIVELY for the full INIT_BOOT_CDS
dinit cascade — three dispatches, one per boot-device level. All
three return cleanly to the kernel. The post-SYS_PROC_INIT 10707
suppression unwind still fires (unchanged from P108 baseline), so
the milestone count doesn't advance yet; MDDF_IO HLE still scaffolds
the real-mount path. But Phase 3 is genuinely operational.

**P115 shipped:**

1. **CALLDRIVER(dinit) pass-through**
   (`src/lisa.c:hle_handle_calldriver`, now `#if 1`). When dinit is
   called with a real user-RAM entry_pt, our HLE arms
   `p115_sp_fixup_pending` in `lisa->hle` and returns false, letting
   the kernel CALLDRIVER body run. Kernel pushes its function-result
   slot (2B) + parameters ptr (4B), JSRs to the compiled driver's
   DRIVERASM entry at `$207408`.

2. **SP fix-up at $749C2** (`src/m68k.c`, inside the CPU main loop).
   Every time PC reaches kernel's `MOVE.W (SP)+,D0` (post-JSR
   unwind) while the passthrough is armed, `SP += 4` compensates
   for our Pascal codegen's caller-clean RTS (Apple's asm CALLDRIVER
   expects callee-clean from the driver). This fires exactly twice
   per dinit: once for the nested fnctn=13 (HDINIT) path, once for
   the outer return.

3. **Tiny accessors** (`src/lisa.c`):
   `lisa_hle_p115_fixup_pending()` / `lisa_hle_p115_clear()` — so
   `m68k.c` can check / clear the passthrough flag without pulling
   in the full `lisa_t` type.

**Observed boot trace:**
```
P112 HLE GET_BOOTSPACE -> $200000 / $203A00 / $207400
P110: 115 sites rebased × 3 drivers
for each of 3 devices:
  P115: CALLDRIVER(dinit) -> real driver entry=$207408 config=$CCBxxx
  DRV-ENTER  $207408              (entry, JMP DRIVER)
  DRV-EXIT   $CCB6B0               (USE_HDISK trampoline)
  DRV-ENTER  $2079E8               (USE_HDISK RTS -> driver)
  DRV-EXIT   $CCB6B6               (CALL_HDISK trampoline)
  HLE CALLDRIVER fnctn=13 (HDINIT) -- HLE'd
  P115 SP fix-up @ $749C2         (kernel CALLDRIVER post-JSR)
  DRV-ENTER  $207A06               (driver back from HDINIT)
  P115 SP fix-up @ $749C2         (kernel CALLDRIVER post-JSR, outer)
  DRV-EXIT   $749C2                (PRODRIVER final RTS, clean)
  HLE CALLDRIVER fnctn=12 (DATTACH) -- no-op HLE
HLE CALLDRIVER fnctn=7 (DCONTROL dcode=20) -- health-check HLE
10707 suppression -> SYS_PROC_INIT 23/27 (unchanged baseline tail)
```

**What this unlocks**: The boot is now architecturally ready to
retire the MDDF_IO HLE scaffold. The compiled SYSTEM.CD_PROFILE is
running its real Pascal/asm code, calling into kernel via the
DRIVRJT trampolines, going through HDINIT for hardware bring-up.
The remaining bits (populating `ext_diskconfig` natively so MDDF_IO
can use UltraIO instead of our P104 HLE) are downstream of what
P115 unblocked.

**Next blocker → P116 (retire MDDF_IO HLE)**: with dinit running
on the real driver, check whether `ext_diskconfig.blockstructured`
and `.fs_strt_blok` get populated natively. If yes, try removing
the MDDF_IO HLE and letting psio→LisaIO→UltraIO do the mount read.
If hdinit's ext_diskconfig path still fails (perhaps because the
kernel's compiled memory manager has its own issues around the
bootspace we allocated via HLE GET_BOOTSPACE), that'd expose the
next layer. Concrete steps:

1. Dump `configinfo[bootdev]^.ext_diskconfig` after INIT_BOOT_CDS
   completes — is it populated?
2. Try disabling the MDDF_IO HLE (`src/lisa.c:hle_handle_mddf_io`
   return) and see what happens.
3. If real_mount now succeeds, milestone count advances past the
   current 10707 unwind target.

Full infrastructure active (all `#if 1`, all HLE additions):
- HLE GET_BOOTSPACE ($200000+ pages in MMU seg 16 pass-through).
- ldr_movemultiple MMU-translated writes.
- P110 reloc scan (115 sites × 3 drivers).
- CALLDRIVER dinit pass-through.
- P115 SP fix-up at $749C2.

Baseline: 23/27 SYS_PROC_INIT clean. Audit: 28013 relocs, 0 unresolved.

## Previous Status (2026-04-19 post-P114 ABI finding)

**Milestones**: **23/27** SYS_PROC_INIT (unchanged). P114 completed
the crash-root-cause hunt from P113. The finding is a real structural
ABI mismatch between our Pascal codegen and Apple's kernel CALLDRIVER.

**P114 root cause (fully identified):**

Dumped the kernel dispatch stubs at `$CCB6B0` (where driver trampolines
land via DRIVRJT=$00CCB602):
```
  $CCB6B0: 4E F9 00 03 BD AE   JMP.L $0003BDAE   (USE_HDISK impl)
  $CCB6B6: 4E F9 00 07 49 82   JMP.L $00074982   (CALL_HDISK impl)
  $CCB6BC: 4E F9 00 03 C2 1A   JMP.L $0003C21A   (OKXFERNEXT impl)
  $CCB6C2: 4E F9 00 07 0E 94   JMP.L $00070E94
  $CCB6C8: 4E F9 00 07 67 5E   JMP.L $0007675E
  $CCB6CE: 4E F9 00 07 67 92   JMP.L $00076792
  $CCB6D4: 4E F9 00 07 67 BA   JMP.L $000767BA
```

So the dispatch mechanism is correct: driver's trampoline → DRIVRJT
offset lookup → kernel dispatch stub → real kernel impl. Kernel's
code runs. Problem is on the RETURN from the driver.

Cracked open `source-mover.text:636-684` — the real asm CALLDRIVER
code:
```asm
CALLDRIVER
        MOVE.L  8(SP),D0        ;CONFIG_PTR
        ...
        MOVE.L  ENT_PT(A1),D2   ;driver entry point
        ...
CD1     MOVEQ   #0,D1
        MOVE    D1,-(SP)        ;FOR FUNCTION RESULT  (2B)
        MOVE    INTPAR(A2),D0
        MOVE    FNCTN_CODE(A2),D1
        MOVE.L  A2,-(SP)        ;PARAMETERS PASSED ON STACK (4B)
        MOVE.L  D2,A0
        JSR     (A0)            ;CALL DRIVER ENTRY
        MOVE.W  (SP)+,D0        ;POP FUNCTION RESULT (2B)
CD2     MOVE.L  (SP)+,A1        ;RETURN ADDRESS (4B)
        ADDQ    #8,SP           ;skip config_ptr + parameters args
        MOVE.L  (SP)+,A0        ;ERRNUM ADDRESS
        MOVE.W  D0,(A0)         ;SAVE ERRNUM
        JMP     (A1)            ;RETURN to original caller
```

The unwind only works if the DRIVER popped its 4-byte `parameters`
push before RTS. After driver's standard `UNLK A6; RTS`, SP is 4
bytes too high:

```
Stack at line 676 (should be, for Apple's callee-clean driver):
  SP+0: function result slot (2B)     <- MOVE.W (SP)+,D0 reads this
  SP+4: retaddr_caller (4B)           <- MOVE.L (SP)+,A1 reads this
  SP+8: parameters arg, config arg, errnum arg

Stack at line 676 (what our caller-clean driver leaves):
  SP+0: parameters ptr push (4B)      <- MOVE.W (SP)+,D0 reads HIGH word of ptr
  SP+4: function result (2B)          <- MOVE.L (SP)+,A1 reads LOW word of ptr + result (garbage)
  SP+6: retaddr_caller                <- effectively lost
  ...
```

So `A1 = garbage`, `JMP (A1)` jumps to stack byte → ILLEGAL $00CB at
$CBFE00 (stack area). Matches the observed crash exactly.

**P114 findings shipped**:
- `m68k.c:illegal_instruction` dumps PC bytes + SP stack + regs.
- `m68k.c` main loop logs DRV-ENTER/EXIT at driver-range transitions
  (`$200000..$20FFFF`).
- `m68k.c` main loop dumps jumptable bytes @PC when first DRV-EXIT
  lands in kernel seg 102, plus DRIVRJT ($210) value.
- `m68k.c` main loop dumps 256-entry PC ring (driver PCs marked) on
  first jump into `$CBE000..$CC0000` (stack-jump tripwire).
- Passthrough stays `#if 0` to preserve 23/27 baseline.

**Next blocker → P115 (Pascal codegen callee-clean for asm-ABI)**:
two paths, pick one:

1. **Codegen fix** (durable, matches Apple's real toolchain).
   Modify `src/toolchain/pascal_codegen.c` function-epilogue
   emission: when a Pascal function is declared with asm-callable
   conventions (or specifically for `PRODRIVER`-style externals
   called by asm CALLDRIVER), emit callee-clean epilogue:
   ```
   UNLK A6
   MOVE.L (SP)+,A0    ;pop retaddr to A0
   ADDQ   #4,SP        ;pop 4-byte parameters arg
   JMP    (A0)         ;return
   ```
   Instead of `UNLK A6 ; RTS`. Apple Pascal's `.UPROC` /
   procedure-with-asm-conventions flag would drive this — our
   codegen needs to recognize it. PRODRIVER is declared
   `function PRODRIVER(parameters: param_ptr): integer;` without
   special markers, but Apple's toolchain somehow knows. Check
   if there's a directive in source-PROFILE.TEXT or
   source-DRIVERDEFS.TEXT.

2. **HLE CALLDRIVER replacement** (scaffolding, retires with path 1).
   Instead of letting kernel CALLDRIVER run, replace it entirely
   in the HLE: simulate its work natively (copy config_ptr into
   parameters record, read ENT_PT from config extension), then
   manually dispatch to driver with kernel's stack convention,
   intercept on driver RTS, do kernel's post-call unwind. More
   complex but keeps the Pascal codegen untouched.

Path 1 is the real fix — it's a single codegen-emission change
that'd apply to every Pascal function called from asm-ABI
context. Worth doing even if it takes a session.

Baseline clean: 23/27 SYS_PROC_INIT, 28013 linker relocs, audit
passes. Everything P112 shipped (HLE GET_BOOTSPACE + MMU-write
ldr_movemultiple + active P110 reloc scan catching 115 sites/load)
stays on and infrastructure-ready.

## Previous Status (2026-04-19 post-P113 diagnostics)

**Milestones**: **23/27** SYS_PROC_INIT (unchanged). P113 was a
diagnostic slice — instrumented the stack-jump crash (from flipping
P112's dinit passthrough on) and got the full control-flow picture.

**P113 diagnostics shipped (in `src/m68k.c`):**
- `illegal_instruction()` now dumps 16 bytes around PC + 32 bytes of
  stack + D0/D1/A0/A1/A6/A7 on every fault.
- CPU main loop prints `P113 DRV-ENTER/EXIT` transitions whenever PC
  crosses in/out of the driver's loaded range (`$200000..$20FFFF`).
- CPU main loop prints `P113 STACK-JUMP` + a 256-entry PC ring dump
  (driver PCs marked) on the first PC in `$CBE000..$CC0000`.

**Observed crash path (passthrough on):**
```
  P112: CALLDRIVER(dinit) -> real driver entry=$207408 config=$CCB72A
  P113 DRV-ENTER: PC=$207408 (prev=$749C0) A6=$CBFE4C A7=$CBFE20
  P113 DRV-EXIT:  PC=$CCB6B0 (prev=$207536) A6=$CBFE1C A7=$CBFDDE
  P113 DRV-ENTER: PC=$2079E8 (prev=$3BDD6)  A6=$CBFE1C A7=$CBFDE2
  P113 DRV-EXIT:  PC=$CCB6B6 (prev=$207540) A6=$CBFE1C A7=$CBFDD6
  HLE CALLDRIVER: fnctn=13 (HDINIT) config=$CCB72A params=$CBFE3E
  <kernel TRACE runs $681A..$68F2 for ~200 bytes>
  P113 STACK-JUMP: PC=$CBFE00 A6=$CBFE1E A7=$CBFDE6 SR=$2710
  ILLEGAL: opcode=$00CB group=0 at PC=$CBFE00
  bytes @PC-8..PC+8: 00 00 00 00 35 56 44 BE 00 CB 00 1F 00 CB FF FF
  stack @SP..SP+32: 00 01 00 01 51 86 00 CB FD F2 00 04 00 00 00 03
                    FE 7B 00 00 00 00 35 56 44 BE 00 CB 00 1F 00 CB
  D0=$1 D1=$0 A0=$216 A1=$FE3E0000 A6=$CBFE1E A7=$CBFDE6
```

**Interpretation:**
- Driver actually executes — first entry at `$207408`, then its
  DRIVERASM trampoline at `$207536` (offset $12E into driver) does
  `JMP 0(A0)` where A0 was loaded via `MOVE.L DRIVRJT,A0` = whatever
  is at absolute $210.
- Trampoline targets are `$CCB6B0` and `$CCB6B6` — kernel jumptable
  slots in MMU seg 102. So kernel HAS populated DRIVRJT with code at
  those addresses (first two slots = CANCEL_REQ and ENQUEUE dispatch
  stubs).
- Kernel runs those stubs, eventually reaches TRACE ($6806..$68F3),
  runs ~200 bytes linearly, then RTS returns to the stack at $CBFE00.
- `A0=$216` at crash = DRIVRJT+6 (ENQUEUE slot address). Suggests
  something near the end of the TRACE return chain did
  `LEA 6($210),A0` or equivalent.
- Stack pattern `$00010001 $518600CB $FDF20004 ...` shows data left
  over from a Pascal LINK frame or earlier push sequence that wasn't
  cleaned up.

**P113 gated state**: dinit passthrough reverted to `#if 0` to keep
baseline at 23/27. Re-enable to repro the above trace in one run.

**Next blocker → P114 (DRIVRJT trampoline frame ABI)**: the
kernel's CANCEL_REQ / ENQUEUE / etc. dispatch stubs at $CCB6B0+ are
executing but their return unwinds to garbage. Most likely causes:
1. Our JSR→driver→JMP-through-trampoline→kernel-stub chain pushes
   extra frames that the kernel's RTS chain doesn't fully pop.
2. The kernel dispatch stubs expect certain registers or stack
   state (e.g., A5 for kernel globals) that the driver's execution
   disturbed.
3. The driver called a trampoline whose return was supposed to
   go back to the driver, but it went back to CALLDRIVER's caller
   instead (return-address confusion).

Concrete next-session steps:
1. Dump `$CCB6B0..$CCB700` to see exactly what kernel code is at the
   jumptable slots (first 40 bytes should be 2-3 dispatch stubs).
2. Trace PCs through the full driver-to-kernel transition, not just
   the 30-entry crash window — maybe bump PC ring to 1024.
3. Check whether the driver's trampoline is supposed to be JSR
   (push retaddr) or JMP (don't push) — if it was meant to JSR
   the kernel dispatches but our compiled code emits JMP, that
   loses the return path.
4. If it's ABI-deep, consider leaving compiled-driver dispatch
   off permanently and improving the CALLDRIVER HLE to populate
   `ext_diskconfig` natively (sidestep the driver entirely).

Baseline clean: `./build/lisaemu --headless Lisa_Source 3000`
reaches 23/27 SYS_PROC_INIT. 28013 linker relocs. Audit passes.

## Previous Status (2026-04-19 post-P112)

**Milestones**: **23/27** kernel checkpoints reached natively, last
checkpoint `SYS_PROC_INIT` (unchanged). P112 shipped *all* the
Phase-3 load-infrastructure: the compiled SYSTEM.CD_PROFILE now
lives in user RAM with internal JSR/JMP/LEA/PEA/MOVE.L#imm targets
rebased to its actual load address. The `dinit` pass-through
itself is gated off pending a Pascal-return ABI fix — last step of
P113 below.

**P112 shipped (all active):**

1. **HLE `GET_BOOTSPACE`** (`src/lisa.c:lisa_hle_intercept`).
   Pascal function intercept:
   `function GET_BOOTSPACE(size: longint; lastalloc: boolean): longint;`
   Stack at entry: SP+0=retaddr, SP+4=size (4B), SP+8=lastalloc (2B).
   Caller-clean convention, result in D0. Handler returns
   addresses from `$200000` stepping up by `(size + 0x1FF) & ~0x1FF + 0x400`
   per call — pages in MMU seg 16 (`lisabugmmu`, unconfigured on our
   boot → pass-through, phys == logical). Bounds-checks against
   `LISA_RAM_SIZE` and falls through to the compiled body if
   exhausted. Retires when `MM4.TEXT:GetFree` is debugged to
   return high-memory pages (P111 root cause).

2. **`ldr_movemultiple` MMU-write** (`src/lisa.c:ldr_movemultiple`).
   Swapped raw `memcpy(&ram[dest],...)` for a `lisa_mem_write8`
   loop. Driver bytes now land at the MMU-translated physical
   page regardless of whether logical dest is above 2.25MB. With
   the P112 GET_BOOTSPACE returning seg-16 pass-through addresses
   (logical = physical = `$200000..$20xxxx`), writes stay in the
   RAM buffer and don't alias kernel globals. (Without P112, this
   alone regressed to `SYSTEM_ERROR(10726)` — P111 finding.)

3. **P110 reloc scan activated** (`src/lisa.c` ENTER_LOADER
   call_move, `#if 1`). After each bulk copy of the SYSTEM.CD_PROFILE
   code block, scan for and rebase:
   - `$4EB9` JSR abs.L
   - `$4EF9` JMP abs.L
   - `$4879` PEA abs.L
   - `$4XF9` LEA abs.L,An (any An via the `(op & 0xF1FF) == 0x41F9` mask)
   - `$2X3C` MOVE.L #imm,Dn (any Dn via `(op & 0xF1FF) == 0x203C`)
   - `$2F3C` MOVE.L #imm,-(SP)
   Targets in `[$400, $400 + count)` are rebased to
   `target + (dest - $400)`. Finds 115 sites per driver load
   (was 70 with just JSR/JMP/PEA/LEA) × 3 loads = 345 rebase
   operations per boot. Audit clean: 28013 linker relocations,
   zero unresolved.

4. **CALLDRIVER trace** (earlier committed): logs `entry_pt` +
   `kres_addr` per call so driver-dispatch state is visible in
   future traces.

**P112 gated off (pending P113):** The CALLDRIVER(dinit)
pass-through branch in `hle_handle_calldriver` is fenced `#if 0`.
With it on, the cascade works far enough to:
- Enter PRODRIVER body at `$207408`,
- Run `case fnctn_code of dinit: ...`,
- Delegate to `USE_HDISK + CALL_HDISK(error, configptr, parameters)`,
- Re-enter CALLDRIVER with fnctn=13 (HDINIT) — secondary call
  still HLE'd successfully,
- Then crash on return: `ILLEGAL: opcode=$00CB at PC=$CBFE00`
  with `SYSTEM_ERROR(10201)`, milestones drop 23→21.

Crash is a Pascal function-return ABI mismatch: PC=$CBFE00 is
STACK memory, so the driver's RTS popped a garbage return
address. Either (a) CALLDRIVER's asm dispatcher in DRIVERASM (not
kernel — it's in the driver blob itself) pushes a different
frame shape than our HLE assumes, (b) PRODRIVER's prologue/
epilogue mismatches our codegen conventions, or (c) we missed
a relocation pattern (currently catches JSR/JMP/PEA/LEA/MOVE.L
#imm/MOVE.L #imm,-(SP); may be missing DC.L data tables or
other forms). Repro: flip `#if 0` → `#if 1` near `P112` comment
in `hle_handle_calldriver`.

**Next blocker → P113 (dinit return ABI)**: with the dinit
pass-through re-enabled, diagnose the crash at PC=$CBFE00.
Steps:
1. Trace byte sequence around the crash site — what did the CPU
   try to execute, and what was the stack state at the preceding
   RTS?
2. Check if DRIVERASM.TEXT's CALLDRIVER entry (offset 90 in
   DRIVRJT) has a return convention we haven't matched.
3. Verify PRODRIVER's prologue/epilogue against our Pascal
   codegen — particularly the LINK/UNLK and result-in-D0 paths.
4. Consider extending the reloc scan to other instruction forms
   if the missed-relocation theory pans out.

Once P113 lands, `dinit` runs fully on the compiled driver;
hdinit (fnctn=13) populates `ext_diskconfig.blockstructured` and
`.fs_strt_blok` natively. That in turn lets psio→LisaIO→UltraIO
resolve without E_IO_MODE_BAD, retiring our MDDF_IO HLE +
dcontrol(20) scaffold.

Baseline clean: `./build/lisaemu --headless Lisa_Source 3000`
reaches 23/27 SYS_PROC_INIT, 28013 linker relocs, audit passes.

## Previous Status (2026-04-19 post-P110)

**Milestones**: **23/27** kernel checkpoints reached natively, last
checkpoint `SYS_PROC_INIT` (unchanged). P109 + P110 were both
investigations; no shipped fix yet, but major scoping progress on
Option B (Phase 3 — real compiled SYSTEM.CD_PROFILE driver).

**P110 findings**:

1. **`ldr_movemultiple` has a latent bug** — writes go via raw
   `memcpy(&ram[dest], ...)` with a `dest + avail <= LISA_RAM_SIZE`
   guard that SILENTLY DROPS the copy for high logical addresses.
   `GET_BOOTSPACE` returns logical addresses like `$CCCA08`, `$CC9408`,
   `$CC5E08` for the 3-device INIT_BOOT_CDS cascade; all three are
   >$240000 (2.25MB), so the driver bytes never actually land in
   physical RAM. The CALLDRIVER HLE intercepts dispatch, so the
   missing bytes didn't matter in baseline — but it's why P109's
   naive dinit pass-through saw ILLEGAL opcode $00CC at $CC6B80
   (MMU-translated reads returned whatever happened to be at the
   physical page, not driver code).

2. **The fix — write through MMU** — swapping `memcpy(&ram[dest]...)`
   for a `lisa_mem_write8(...)` loop lands the driver bytes at the
   correct physical page (the one MMU maps logical $CCCA08 to). The
   P110 reloc scanner (see below) then sees real driver code and
   rebases 70 JSR.L/JMP.L/LEA.L/PEA.L targets per driver load. But
   this regresses the baseline even WITHOUT the dinit pass-through:
   milestones drop 23→21, boot halts at `SYSTEM_ERROR(10726)` (=
   `bombed` in LDEQU — "boot device read failed") from within
   `LDR_CALL` around `ret=$015074`. Root cause: the physical RAM
   pages the MMU maps `$CC____` logical to are ALSO aliased to
   kernel-logical addresses used by some kernel data structure;
   actually writing real driver bytes there clobbers that kernel
   data. Tracked as P111 (bootspace-page reservation) — needs a
   proper bootspace allocator in the emulator's MMU/memory model.

3. **The toolchain emits absolute-long JSR/JMP/LEA/PEA** for inter-
   proc calls (`pascal_codegen.c:2184/3158` emit `$4EB9`/`$4EF9`);
   Apple's toolchain emitted PC-relative wherever possible, which
   is why their drivers were position-independent. Our driver has
   exactly 70 such sites (measured via the scan) in the 13654-byte
   SYSTEM.CD_PROFILE — all need load-time rebase. Retiring the
   scan requires either a PC-relative-preferring codegen pass or
   a real OBJ-format relocation table.

4. **CLAUDE.md's prior assumption — "apply the codeblock reloffset
   properly" — was wrong.** Apple's LOADCD (and the LOADER's
   loadseg) STORE the reloffset in the loaded image's header but
   NEVER APPLY IT. No other code in `Lisa_Source/` reads the
   reloff field. The driver relies on PC-relative code and
   kernel-jumptable absolutes (DRIVRJT=$210) for all inter-
   module references. Our driver needs load-time relocation
   because our codegen emits absolute-long where Apple's emitted
   PC-relative.

**Infrastructure shipped, gated off**:

- `hle_handle_calldriver` now logs `entry_pt` + `kres_addr` per call
  (observable: driver loaded at `$CC5E08`, config's entry_pt set
  correctly, badcall never populated — so the historic bypass guard
  is effectively dead).
- P110 reloc scan written in-source behind `#if 0` at
  `src/lisa.c:~690`. Activates alongside a future MMU-write fix.
- `ldr_movemultiple` comment documents the silent-drop behavior and
  the P111 blocker.

**Next blocker → P111 (GetFree returns kernel-globals pages)**:
instrumented MMU for the 3 driver-load destinations — all land in
MMU segment 102 (`sysglobmmu`, per MMPRIM.TEXT:79 — "mmu used to
map sysglobal data segment"). Segment 102 config at time of load:
SOR=$3DC (phys base $7B800), SLR=$700. So logical $CCCA08 → phys
$88208, $CC9408 → $84C08, $CC5E08 → $81608. Kernel A5=$CC6FE0
maps to phys $827E0 through the SAME segment, so A5-relative
kernel globals cover roughly phys $7B800..$8xxxx — overlapping
all three driver-load physical destinations. Driver bytes
overwrite kernel globals when writes actually land.

Root cause: our compiled `MM4.TEXT:GetFree` is allocating pages
from the LOW end of the free chunk instead of the HIGH end (Apple
comment: "Allocate the given number of pages from the high end of
freespace"). `page_got = $81608 / 512 = 1035` — low page — while
kernel globals already occupy pages up to ~1060. The memory-
manager free-list (built by MM_INIT / MAKE_FREE) is either (a) set
up with the free-list tail pointing at a LOW free chunk that the
kernel didn't correctly reserve, or (b) our codegen for one of
GetFree/TAKE_FREE/MAKE_FREE/MERGE_FREE has a bug that puts the
allocator into the wrong region.

There's already HLE infrastructure around this (P78 Signal_sem
guard + RELSPACE guard, P83 MERGE_FREE guard per
`feedback_hle_layers_load_bearing.md`) — the memory manager has
been fragile for a while. Specifically, we need to verify:

1. Kernel's `bothimem` (runtime — set in LOADER.TEXT:381 as
   `ldbase + ldsize + slot_code`) is correctly marking the end of
   loader-reserved memory. Our LOADER runs via HLE, so this may
   be unset or wrong.
2. `mmrb_addr` (memory manager record block) — is MM_INIT
   constructing a free-list that correctly excludes kernel
   globals?
3. `GetFree`'s `tail_sdb.freechain.bkwd_link` walk — does the
   last-free segment actually correspond to free high memory?

Once GetFree returns high-memory pages that don't alias kernel
globals:

  (a) Flip `ldr_movemultiple` to use `lisa_mem_write8` (MMU-
      translated) so driver bytes land in physical RAM.
  (b) Flip the P110 reloc scan `#if 0` → `#if 1` at
      `src/lisa.c:~690`. Rebases ~70 JSR.L/JMP.L/LEA.L/PEA.L
      targets per driver load.
  (c) Relax the CALLDRIVER bypass guard so `fnctn=1` (dinit) with
      user-RAM `entry_pt` passes through to the real compiled driver.
  (d) `dinit` delegates to `USE_HDISK + CALL_HDISK` which re-enters
      CALLDRIVER for the HDISK config (entry_pt 0 → secondary call
      still HLE'd successfully).
  (e) `hdinit` on the outer driver populates `ext_diskconfig` natively
      and retires our MDDF_IO + dcontrol(20) HLEs.

Alternative faster path: **HLE GET_BOOTSPACE** to return pages
reserved by the emulator, bypassing the compiled GetFree entirely.
Layered HLE that retires when the memory manager is debugged.

  (a) Flip the P110 reloc scan `#if 0` → `#if 1`.
  (b) Relax the CALLDRIVER bypass guard so `fnctn=1` (dinit) with a
      user-RAM `entry_pt` passes through to the real compiled driver.
  (c) `dinit` delegates to `USE_HDISK + CALL_HDISK` which re-enters
      CALLDRIVER for the HDISK config (entry_pt 0 → secondary call
      still HLE'd successfully).
  (d) `hdinit` on the outer driver populates `ext_diskconfig` natively
      and retires our MDDF_IO + dcontrol(20) HLEs.

Baseline unchanged post-P110: `./build/lisaemu --headless Lisa_Source 3000`
reaches 23/27 SYS_PROC_INIT clean, `make audit` shows 28013 relocs, 0
unresolved.

## Previous Status (2026-04-19 post-P109 diagnostic)

**Milestones**: **23/27** kernel checkpoints reached natively, last
checkpoint `SYS_PROC_INIT` (unchanged from P108). This session (P109)
was an investigation, not a codegen fix.

**P109 finding — Phase 3 readiness**: `SYSTEM.CD_PROFILE` is *already*
fully compiled, linked (13654 bytes), and placed on disk as an
OBJ-wrapped file (`system.cd_profile`). LOADCD opens and loads it
three times during INIT_BOOT_CDS (per the 3-device cascade). The
loaded driver image lands in user RAM at `$CC5E08`, and that address
makes it into `configinfo[bootdev]^` as `entry_pt`.

So the *compile + place + load* half of Option B (from the P108
handoff) was already done — I had the wrong model coming in. The
remaining gap is driver *dispatch*: our CALLDRIVER HLE at
`src/lisa.c:hle_handle_calldriver` intercepts every `fnctn=1/12/7`
call instead of letting them pass to the compiled driver's
DRIVERASM dispatcher at `entry_pt`.

**P109 experiment (reverted)**: I relaxed the HLE's bypass guard so
`fnctn=1` (dinit) with a user-RAM `entry_pt` passed through to the
real driver. Result: DRIVERASM prologue ran, but shortly after hit
`ILLEGAL opcode $00CC at PC=$CC6B80`. The `$00CC` byte is clearly
the high byte of a user-RAM pointer being executed as code — the
dispatcher jumped through a data word. Root cause (likely): our
LOADCD / smart ENTER_LOADER HLE doesn't apply the codeblock's
`reloffset` to the loaded driver image. On real Lisa, LOADCD reads
each codeblock's 4-byte reloffset (OBJ format) and relocates
absolute-address words accordingly. Our ENTER_LOADER HLE
(`src/lisa.c:640-702`, `call_move` + `call_read`) copies bytes
verbatim. Milestones dropped 23→21 (new error 10201 no longer
matches the 10707 suppression).

Change reverted. Diagnostic kept — CALLDRIVER now logs `entry_pt`
and `kres_addr` per call so the state is visible in future traces.

**Next blocker → P110**: apply OBJ-format reloffset during
SYSTEM.CD_PROFILE load. Either (a) in the smart ENTER_LOADER HLE's
call_read/call_move path (quickest — we already know which file is
being loaded), or (b) in the toolchain's OBJ wrapper in
`toolchain_bridge.c:~1393` (durable — emit pre-relocated bytes or
correct reloffset so any LOADCD-compatible loader handles it).
Option (b) is the right long-term answer; option (a) unblocks
P109's dinit-passthrough experiment in a single session.

After that: relax CALLDRIVER HLE's bypass guard (currently gated
on `badcall != 0`, which is never true on the current boot path)
so dinit passes through. Expected cascade: dinit →
`USE_HDISK + CALL_HDISK(...)` → re-enters CALLDRIVER for HDISK
(config entry_pt will be 0/BADCALL so the secondary call still
HLE-succeeds). Then `hdinit` on the outer driver populates
`ext_diskconfig`, which in turn should let psio→LisaIO→UltraIO
work natively and retire our MDDF_IO + dcontrol(20) HLEs.

## Previous Status (2026-04-19 post-P108)

**Milestones**: **23/27** kernel checkpoints reached natively, last
checkpoint `SYS_PROC_INIT`. P108 landed a structural codegen fix —
integer-to-longint arg widening now emits `EXT.L D0` before the
`MOVE.L D0,-(SP)` push when the caller's expression produces a
narrow (2-byte) value but the callee's param is 4 bytes. Prior to
the fix, BitMap_IO's `psio(..., bitmap_bytes, op)` push looked like
`MOVE.W (A0),D0 ; MOVE.L D0,-(SP)` — the MOVE.W only touches the
low word, so the pointer register's leftover high word got pushed
as the top 16 bits of nbytes. `bitmap_bytes = $04B8 = 1208` was
pushed as `$00CC04B8 = 13,370,552`, and every narrow field passed
to a longint param carried the same 16 bits of stack garbage.
Caller-side counterpart to P91's callee-side fix.

P108 fix lives in `src/toolchain/pascal_codegen.c` at the
AST_FUNC_CALL arg-push loops in `gen_expression` — both the
callee-clean (LTR) and caller-clean (RTL) branches. Conditional:
emit EXT.L only when `expr_size(arg) <= 2 && !rhs_has_wide_operand`.
Skips pointer returns, longint/pointer globals, and subexpressions
that already produced a 32-bit result.

P108 also added — then gated off — HLEs for `psio` and `vm` at
`src/lisa.c` (between `hle_handle_mddf_io` and `hle_handle_system_error`).
With psio+vm active, bitmap_io succeeds, real_mount completes, the
second MDDF_IO write runs, and fs_mount advances into open_sfile —
but wait_sem / slist_io / the cleanup path then wander into low-PC
exception cycling. Even psio alone perturbs the 10707 unwind
target: real_mount's failure shifts from bitmap_io to deeper in
fs_mount, state mutates more before 10707 fires, and the unwind
lands short of SYS_PROC_INIT (22/27). The gated HLEs stay
in-source behind `#if 0` in the dispatcher (with
`__attribute__((unused))` on the handlers to silence warnings) for
pickup next session alongside either (a) a richer HLE covering
open_sfile / wait_sem flow, or (b) Phase 3 proper (compiled
SYSTEM.CD_PROFILE populating ext_diskconfig so the real psio/vm
chain works through UltraIO).

**Next blocker (unchanged from P107)**: bitmap_io's psio chain.
The P108 codegen fix doesn't advance the milestone count — it's
latent correctness for every integer→longint arg widening going
forward. The actual boot state is identical to post-P107: same
MDDF_IO READ + dcontrol(20) + 10707-at-real_mount + unwind to
BOOT_IO_INIT caller → SYS_PROC_INIT.

## Previous Status (2026-04-19 post-P106)

**Milestones**: **23/27** kernel checkpoints reached natively, last
checkpoint `SYS_PROC_INIT`. The 2026-04-19 session landed **P106**
on top of P105 — a two-part fix that carries `real_mount` past the
`drivercall(dcontrol, dcode=20)` spare-table health check AND the
subsequent MDDF_io write-protect test. Count is unchanged but
internal advance is real: pre-P106 real_mount failed at the spare
check; post-P106 real_mount completes. The next 10707 fires from a
downstream caller (second `HLE MDDF_IO: WRITE` at a different
@ecode frame) with D2=$367 (=871 E1_SENTRY_BAD).

**P106 is two related fixes** (commit `ff38ba1`):

1. **CALLDRIVER dcontrol(dcode=20) HLE scaffold** (`src/lisa.c`,
   `hle_handle_calldriver`). real_mount calls drivercall(dcontrol,
   dcode=20) to check spare-table health; Apple's real ProFile driver
   replies with `ar10[0]=SPARES_INTACT(4), ar10[2]=available spares,
   ar10[3]=SPARE_TABLES_OK(0)`. Pre-P106 our HLE left ar10 zeroed, so
   real_mount fired E_SPARES_DAMAGED. Split HLE_DCONTROL out of the
   no-op cluster and write the healthy triple for dcode=20 only.
   Layered scaffolding HLE — retires in Phase 3 when compiled
   SYSTEM.CD_PROFILE handles dcontrol natively.

2. **WITH-field array-of-longint read size** (`src/toolchain/pascal_codegen.c`,
   `expr_size` for `AST_ARRAY_ACCESS`). Even after Fix 1, real_mount
   still fired E_SPARES_DAMAGED. Disassembly around `$068D50` showed
   the compare `ar10[0] <> SPARES_INTACT(4)`:
   ```
   MOVEQ #0,D0 ; MOVEA.L (A7)+,A0 ; MOVE.W D0,D1 ; MULU.W #4,D1 ;
   ADDA.L D1,A0 ; MOVE.W (A0),D0  ← reads 2 bytes, not 4
   ```
   The stride multiplier was correct (4 = longint size) but the
   LOAD was 2 bytes — reading only the high word of each longint
   element. Our `$00000004` stored big-endian at `ar10[0]` read back
   as `$0000`, compared vs 4 failed. Root cause: `expr_size` returned
   2 for WITH-field array accesses because the `if (arr) return es;`
   guard skipped the real size return whenever `arr =
   find_symbol_any(array_name)` was NULL — which is always true for
   a WITH-record field. Fix: extend the es-return path to WITH-field
   case when element type is non-pointer (longint/integer/subrange).
   Scoped that way per the existing comment warning about
   pointer-of-array regressions.

**Next blocker**: second `HLE MDDF_IO: WRITE` at `@ecode=$CBFF34`
(different stack frame from real_mount's `$CBFFB8`), followed by
SYSTEM_ERROR(10707) with D2=$367 (=871 = `E1_SENTRY_BAD`). The
caller of this second MDDF_IO WRITE is unknown — likely FS_INIT's
retry loop or FS_MASTER_INIT's post-mount sfile-open on an sfile
whose `hintaddr` is 0 on our disk image. Investigation for P107.

## Previous Status (2026-04-19 post-P105)

**Milestones**: **23/27** kernel checkpoints reached natively, last
checkpoint `SYS_PROC_INIT`. The 2026-04-19 session landed **P105** —
three structural Pascal-codegen fixes that let `real_mount`'s MDDF
sanity check actually pass. The boot now runs past all three MDDF
checks (fsversion, MDDFaddr, length(volname)) into `drivercall(dcontrol,
dcode=20)`, which returns zeroed spare-table state via our scaffold
CALLDRIVER HLE; that fails the next guard and fires
`E_SPARES_DAMAGED (1054)` instead of the prior
`E_FS_VERSION (866)`. Both errors still bucket to `SYSTEM_ERROR(10707)
stup_fsinit` which our existing unwind suppresses, so the milestone
count is unchanged but the execution chain is genuinely longer.

**P105 is three related codegen fixes** (committed as
`600863b ... auto: claude-turbo checkpoint` + docs follow-up):

1. **Nested WITH with pointer-deref of outer-WITH field**
   (`AST_WITH` case in `gen_expression`): `with mounttable[device]^ do
   ... with MDDFdata^ do ...` — inner WITH's `rec_expr =
   AST_DEREF(AST_IDENT_EXPR(MDDFdata))`, where `MDDFdata` is a
   FIELD of the outer WITH's record, not a standalone global. Pre-P105
   the AST_DEREF branch only called `find_symbol_any(MDDFdata)` which
   returned NULL, so the inner WITH's `record_type` stayed NULL and
   all field lookups inside it fell through to `find_symbol_any`. Fix:
   when the ident-based lookup misses and we're already inside a WITH,
   resolve the pointer via `with_lookup_field` and deref its base type.

2. **AST_IDENT_EXPR: WITH fields shadow globals even when global is
   a CONST** (gen_expression read path): Apple's
   `terror = (fsversion, fserror, ...)` enum in `source-LDUTIL.TEXT`
   registered `fsversion` as a global CONST=0. Pre-P105,
   `gen_expression` checked `find_symbol_any` BEFORE the WITH context,
   so `fsversion` inside `with MDDFdata^ do` compiled as `MOVEQ #0,D0`
   (the enum-ordinal CONST) instead of loading the MDDFdb field. P93
   had already fixed this for `gen_lvalue_addr` and `expr_size`; P105
   extends the same rule to the read path in `gen_expression`.

3. **Comma-separated record fields split into individual fields**
   (record-layout pass): The parser collapses
   `a, b, c : type` into ONE `AST_FIELD` node with name="a,b,c".
   Pre-P105 codegen registered that as a single field of size
   `sizeof(type)`, so `UID = record a,b: longint end;` came out as
   4 bytes instead of 8. This shifted `MDDFdb.MDDFaddr` from its
   canonical +130 offset to +126, and `(MDDFaddr <> 0)` in real_mount
   read `datasize=512` from +126 as if it were MDDFaddr, firing
   `E_FS_VERSION`. Fix: split `field->name` on commas and emit one
   entry per sub-name, each advancing `offset` by the type size.

4. **LENGTH() on non-pointer strings uses address-of**
   (`fn == "LENGTH"` case in gen_expression): pre-P105 LENGTH called
   `gen_ptr_expression` which evaluated the WITH-field string's
   CONTENT (first 4 bytes) and treated it as a pointer. For
   `length(volname)` in `with MDDFdata^` that loaded `$064C6973`
   (length byte + "Lis") and dereferenced it as address $00649736,
   reading a garbage length byte. Fix: if the operand resolves to a
   direct `TK_STRING` (via size > 4 or direct symbol-lookup), use
   `gen_lvalue_addr` to load `&string` into A0; otherwise keep the
   pointer-deref path.

**Next blocker**: `drivercall(@dparm)` with `dcode=20` (real_mount's
spare-table health check, sfileio2.text:340-371). Apple returns:
`cparm.ar10[0] = SPARES_INTACT (=4)`, `cparm.ar10[2] = spares
available`, `cparm.ar10[3] = SPARE_TABLES_OK (=0)`. Our CALLDRIVER
HLE zeros everything, so `ar10[0] <> SPARES_INTACT` is true and
real_mount fires `E_SPARES_DAMAGED`. Either (a) extend the CALLDRIVER
HLE to recognize `dcode=20` and fill healthy spare-table values, or
(b) proceed to Phase 3 (compile SYSTEM.CD_PROFILE so the real driver
handles this). (a) is ~5 lines, (b) is the Phase 2 step 4b/9-11 arc.

**P104 state (unchanged)**: MDDF_IO HLE bypasses psio→LisaIO→UltraIO
and reads the MDDF block directly from the ProFile image. Returns
`ecode=0` on success. MDDF layout on disk is correct; what was
broken pre-P105 was our compiler's reading-side layout of the
in-memory `MDDFdb` record.

**Previous session milestones (historical)**: The original
description of the P104 blocker (CLAUDE.md pre-P105) said *"our disk
image's MDDF bytes don't line up with Apple's MDDFdb"* — that was
partially wrong. The DISK bytes match Apple's MDDFdb byte-for-byte;
what didn't line up was our COMPILER's in-memory layout of MDDFdb
(4 bytes short due to `UID = record a,b: longint end;` being
laid out as 4 bytes). P105 fixes the compiler side; the on-disk
layout was already correct.

After SYS_PROC_INIT boot hits the 10707 suppression and halts
cleanly via STOP instruction.

**P102b (commit `10b8c71`)**: `INIT_TWIGGGLOB` (3 Gs,
STARTUP.TEXT:1906) and `INIT_TWIGGLOB` (2 Gs, the real decl in
source-twiggy.text) share the 8-char prefix `INIT_TWI`.
`INIT_TWIG_TABLE` (asm, in source-LDTWIG.TEXT) also shares it.
Pre-fix, strcasecmp-only lookup failed for the 14-char caller vs
13-char decl, so the call fell through to a generic 4-byte-arg
signature and the linker's first-match prefix picked the asm
`INIT_TWIG_TABLE`. That routine popped retAddr into A0 + error=0
into A1, then (at $7150A in the prior build) attempted a 93-word
copy from TrapTable to (A1)+. P102a blocked the copy; P102b
resolves the call properly to the Pascal `INIT_TWIGGLOB` by LCP
(10 vs 9), so the VAR err argument pushes `@error` via LEA+MOVE.L
and the JSR never reaches the `INIT_TWIG_TABLE` body.

**P102c (commit `27df41d`)**: P102b's LCP fallback used
`mlen = min(name_len, 8)`, so short names like `Dimcont` (7 chars,
a WITH-record field in SET_PREFERENCES) scanned for same-7-char
prefix and LCP-matched `DimContrast` (11 chars, LIBHW global).
SET_PREFERENCES' `SETDIMCONTRAST(DimConert(Dimcont))` was then
compiled with a spurious `JSR DimContrast` wedged between the
field-load and the real `JSR DIMCONERT`. The bogus call corrupted
the stack and — two frames later via some chain of mutations —
the sysglobal free-list fwdlink that RELSPACE walks after 10707
suppression. RELSPACE looped forever at ~$7160 and
Sys_Proc_Init was never reached, dropping milestones to 22/27.
P102c requires BOTH names to be >= 8 chars before the LCP
fallback applies. Apple's 8-char-significance rule: short names
(< 8 chars) match only via exact strcasecmp — they are distinct
identifiers from any longer name, regardless of shared prefix.

**Previous (pre-P101/102)**: 21/27 at BOOT_IO_INIT. The P97/P98/P99/P100
chain + CALLDRIVER DATTACH no-op had already cleared the internal
state:
- Zero SYSTEM_ERROR firings on the boot path
- Zero HLE suppressions needed on INIT_BOOT_CDS (10740 dead, 10741 dead,
  10758 dead)
- Zero scaffold HLEs active
- UP() recurses the real 3-device driver chain with distinct
  configinfo slots (FIND_EMPTYSLOT returns 39, 38, 37)
- CALLDRIVER(dinit) + CALLDRIVER(dattach) succeed for each level
- Post-INIT_BOOT_CDS flow advances through CONFIG_DOWN, LD_DISABLE,
  MAKE_BUILTIN(cd_scc), MAKE_BUILTIN(cd_console), PARAMEMINIT

Next blocker: **`SYSTEM_ERROR(10707)` stup_fsinit** — FS_MASTER_INIT
fails inside FS_INIT because our emulator doesn't have real disk
filesystem content the kernel expects. The existing suppression
at src/lisa.c handles the error code but boot halts via STOP
after SYS_PROC_INIT. Will need real FS_MASTER_INIT data (either
compiled+loaded disk image, or a richer HLE that satisfies
FS_MASTER_INIT's reads). Same as pre-P102b; the P102b/c cycle
preserved this as the ongoing blocker while fixing a real
resolution bug underneath.

Also outstanding: `INT1V` ($64) / `INT2V` ($68) interrupt vectors
point at the linker's `$3F8` RTE stub because LIBHW's `DriverInit`
is linked in but never called — its `Drivers` preamble procedure
at $6B8FA has zero callers. Apple's LOS 3.1 expects the `Drivers`
preamble to execute at library-load time (via DRIVERS.OBJ /
SYSTEM.LLD loading machinery we don't emulate). On real Lisa
this installs Level1/Level2 handlers at $64/$68, programs VIA1/
VIA2 IER, and sets up Trap5. Not currently load-bearing on the
ProFile-paraport boot path (no twiggy IRQs actually fire), but
will matter when we wire the real ProFile driver and its level-1
interrupt handler.

**Important debugging correction (today):** A `SYSTEM_ERROR code=0`
message that appeared right after BOOT_IO_INIT was a **false positive**
from a hardcoded diagnostic at `cpu->pc == 0x5380` in `src/m68k.c`.
Pre-P91, $5380 was SYSTEM_ERROR's entry. Post-P91/P92 codegen shifts
moved SYSTEM_ERROR to $6802 and left $5380 inside INIT_JTDRIVER's
first loop. The intercept now keys on live `hle_addr_system_error`
(commit `37fe6e9`) — no more phantom errors.

**Earlier sessions showed 24/27 with `PR_CLEANUP` as the last checkpoint,
but that was FAKE progress**: the `count := pages` codegen bug (P91
below) meant LD_READSEQ's `count` arrived at ENTER_LOADER as `65739`
instead of `1`, triggering a catastrophic 33 MB over-read that
corrupted ~everything and let the PC wander through random addresses
that happened to collide with milestone symbols. The 21 we reach now
are genuine forward motion.

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

**Phase 2 Step 4a complete (`d5e50e1`, 2026-04-18 PM):**
runtime milestones advanced **21 → 24** (+INIT_DRIVER_SPACE,
+SYS_PROC_INIT, +PR_CLEANUP). The old ENTER_LOADER bypass in
`src/m68k.c:4846` popped 12 bytes and RTSed with garbage result;
now `lisa_hle_enter_loader()` in `src/lisa.c` decodes
`fake_parms.opcode` and services every loader call
(call_open/fill/byte/word/long/move/read) natively against
`ldr_fs` (which walks the real filesystem laid down in step 3d).
LOADEM's `LD_OPENINPUT('SYSTEM.OS')` now succeeds; 10738 no
longer fires. This is a layered scaffolding HLE — still to be
replaced by the real compiled LOADER when we relink BT_PROFILE at
a non-conflicting RAM address (currently its virtual-$400 linking
collides with SYSTEM.OS at $0..$74000).

**Also this session — critical bug fix (`fa203e2`):** step 3c had
bumped `BOOT_TRACK_BLOCKS` 24→64 in `diskimage.h` but left
`lisa.h` at 24. That split made `is_real_image = (fs_block0 !=
BOOT_TRACK_BLOCKS)` evaluate `64 != 24 = true` for our disks, so
the cross-compile pre-loader branch (which drops SYSTEM.OS into
RAM $0 + writes the GETLDMAP parameter block) was never taken —
runtime milestones had silently dropped from 21 to 5. The macOS
Xcode app is still on the pre-fix binary; rebuild picks up the
aligned header and should see the same 24/27 advance.

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
8. ✅ **Step 4a** (`d5e50e1`): smart ENTER_LOADER HLE that decodes
   `fake_parms.opcode` and dispatches call_open/fill/byte/word/
   long/move/read to `ldr_fs`. 21 → 24 milestones; 10738 no longer
   fires. Step 4b (relink BT_PROFILE and run the real compiled
   LOADER) remains for a future session — blocked on giving
   BT_PROFILE a high-RAM base address via a new
   `compile_target_t.base_addr` field in the linker.
9. Add SYSTEM.CD_PROFILE as a third compile target (source-PROFILE.TEXT +
   source-PROFILEASM + PROF_INIT + PROF_DOWN). Probably necessary to
   advance past PR_CLEANUP toward the shell — the scheduler idle loop
   currently spams `call_open('')` because nothing installs a real
   driver.
10. Place the linked driver `.OBJ` on disk via `disk_add_file(db,
    "system.cd_profile", ...)`.
11. Remove the 10738 suppression in `src/lisa.c:2886` (dead after
    step 4a, but confirm it's not masking other 1073[8-41] errors
    first).

Steps 1–8a complete. Remaining Phase-2 work: step 4b (real LOADER
via BT_PROFILE relink) and 9-11 (SYSTEM.CD_PROFILE driver).

### P91 codegen fix (2026-04-18, commit `94903b9`): integer-param sign extension

Structural fix in `src/toolchain/pascal_codegen.c` at the `AST_IDENT_EXPR`
load path. When the callee reads a `TK_INTEGER` param (size=2) at a
POSITIVE frame offset (= arg slot, pushed by the caller), emit
`EXT.L D0` immediately after the `MOVE.W disp(A6),D0`. Without this:

- The caller pushes integer args as 2-byte `MOVE.W D0,-(SP)`
  (per-arg-size push, line 3332 of pascal_codegen.c).
- A later post-hoc "widening" pass (line 3054-3062) saw the MOVE.W
  and LHS=4-byte longint, and "widened" the load by patching the
  MOVE.W opcode to MOVE.L. But a 4-byte read from a 2-byte arg
  slot pulls in 2 bytes of stack junk above the slot.
- LD_READSEQ's `count := pages` then stored `$0001_00CB` into
  `fake_parms.count` instead of `1` — triggering a 65,739-page
  (33 MB) over-read in the smart ENTER_LOADER HLE's call_read
  handler, corrupting sysglobal, syslocal, the vector table, and
  SYSTEM.OS code at $002600+. Post-corruption, random PC
  wandering hit milestone symbols and made boot appear to
  reach PR_CLEANUP (the "24/27" fake number).

With the fix, `count` reads as `1` correctly, the smart HLE reads
only the requested 1 block, no corruption, and the boot reaches
BOOT_IO_INIT with real state. Next real blocker: `SYSTEM_ERROR code=204`
triggered after BOOT_IO_INIT.

Also in `94903b9`: `src/m68k.c` ENTER_LOADER PC intercept now resets
`enter_loader_addr` on each `g_emu_generation` change (previously a
plain `static int el_probed = 0` which stayed set across emu power
cycles — would have been a sticky-state bug in the lisaOS macOS app
across run/stop cycles, even though headless-per-process was fine).

### Late-day codegen & toolchain fixes (2026-04-18 evening)

After P91 it was possible to actually observe the real boot path instead
of corrupted-state PC wandering. That surfaced five more structural
bugs, all committed and pushed:

- **P92 (`ae89418`)**: `lvalue_record_type` now handles
  `AST_ARRAY_ACCESS`, so `arr[i].field` resolves its field type/offset.
  Fix for `jt[i].routine := @FUNC` in INIT_JTDRIVER (was writing 2 bytes
  at +0 instead of 4 bytes at +2). Previously caused
  `SYSTEM_ERROR(204)`.
- **P93 (`1413f71`)**: WITH-record fields shadow same-named globals in
  `gen_lvalue_addr` and `expr_size`. Apple's Pascal scope rules put
  WITH fields above globals, but our codegen checked `find_symbol_any`
  first. Caused LD_OPENINPUT's `path := inputfile` (inside
  `with cheater do`) to copy into a random global's location and leave
  `cheater.path` empty — LD_OPENINPUT got zero-length filenames.
- **P94 (`5439fcc`)**: smart ENTER_LOADER HLE redirects empty or
  `SYSTEM.CD_` prefix-only filenames to `SYSTEM.CD_PROFILE` (the only
  driver we place on disk). Layered HLE — real fix is a SYSTEM.CDD
  builder in the toolchain (TBD).
- **P95 (`bf429dd`)**: built-in `byte` is `TK_SUBRANGE` with range
  -128..127 (signed), matching Apple's `byte = -128..127;`
  declaration in SOURCE-VMSTUFF / source-LOADER / source-TWIG. Was
  TK_BYTE (unsigned) so byte reads zero-extended. Caused LOADCD's
  `header = codeblock` test (comparing byte-typed `header` with
  signed int `codeblock = -123`) to always fail on $0085 vs $FF85.
- **P96 (`bba0a6a`)**: OBJ endblock offset off-by-4 in
  `toolchain_bridge.c`. `code_block_size` already includes the 4-byte
  codeblock header, so endblock was written at
  `obj + 4 + code_block_size` (too late by 4); LOADCD's search_ptr
  advance landed on zeroed bytes before the endblock, the header-byte
  test failed, and LOADCD returned false → `SYSTEM_ERROR(10739)`.

After P96: LOADCD correctly walks the OBJ stream. call_byte returns
$85 (codeblock), $81 (endblock); call_move(13502 bytes) lands the
driver payload into RAM at $CCCA08. The boot now raises only
`SYSTEM_ERROR(10740) stup_init_boot` and `10741 stup_seq_boot`,
both from the driver-init path (`NEW_DEVICE`, `UP()`, and
`configinfo[bootdev]^.blockstructured` check). These require real
driver integration — next session.

Also in this session: the SYSTEM_ERROR diagnostic now keys on the live
`hle_addr_system_error` (commit `37fe6e9`) so it doesn't produce
false positives after codegen shifts addresses. FIND_BOOT, call_byte,
call_word, call_long HLE logging was added for future debugging.

### P92 codegen fix (2026-04-18, commit `ae89418`): array-of-record field sizing

`src/toolchain/pascal_codegen.c:lvalue_record_type` now handles
`AST_ARRAY_ACCESS` as its left child. Before: `arr[i].field` couldn't
find its type, so `expr_size` fell back to 2 bytes and
`lvalue_field_info` missed the field offset. After: array-element
records are walked, fields resolve to their real types/sizes.

Concrete impact: STARTUP's `INIT_JTDRIVER` does
```pascal
jt: array[0..maxdtable] of record jmpinstr: integer; routine: ^integer end;
for i := 0 to maxdtable do jt[i].jmpinstr := $4EF9;
jt[0].routine := @CANCEL_REQ;   { was: MOVE.W @ +0 — overwrote jmpinstr with low 16 bits
                                   of @CANCEL_REQ ($7EA2), lost upper half }
jt[1].routine := @ENQUEUE;      { same bug, 36 times }
```
Pre-P92, the driver jump table had jmpinstr zero'd by routine stores
and routine left uninitialized — every dispatch through `jt[i]` was
a JMP to a garbage 2-byte address. Root of `SYSTEM_ERROR code=204`
after BOOT_IO_INIT.

Post-P92, verified via disassembly: `jt[i].routine := @FUNC` now
emits `MOVE.L #addr,D0; MOVE.L D0,-(SP); ...; ADDA.W #2,A0; MOVE.L
(SP)+,D0; MOVE.L D0,(A0)` — correct push size, correct store size,
and correct +2 field offset for `.routine`.

### P97 codegen fix (2026-04-18 night, commit `d98ebfc`): WITH `arr[i]^` record_type

`src/toolchain/pascal_codegen.c` AST_WITH handler: when the WITH
expression is `AST_DEREF(AST_ARRAY_ACCESS(...))` — e.g.
`with configinfo[config_index]^ do` in NEW_CONFIG — the handler
previously only recognized `AST_IDENT_EXPR` or nested `AST_DEREF`
under the outer deref. With an array access inside, `record_type`
fell through to NULL, and every field reference inside the WITH body
(read OR write) missed `with_lookup_field` and emitted
`MOVE.W #0,D0` + placeholder (line 2185 "Unknown symbol" branch).

In NEW_CONFIG this silently dropped THREE field writes:
- `required_drvr := configinfo[parent]`
- `workptr := pointer(drvrec_ptr)`   (workptr ended up reading
  uninitialized stack memory; subsequent `workptr^.kres_addr :=
  cd_adr` stored to a random stack address)
- `permanent := true`

So UP() got a drvrec with garbage kres_addr and all-zero device
state → immediate 10740. Fix resolves the array element's pointer
base type for the record_type, so field lookups inside the nested
WITH work. UP() now reaches `CALLDRIVER(dinit)` → our HLE returns
error=0 → 10740 gone for good. First real forward motion past the
driver-init gate.

Scaffold HLE (`src/m68k.c` UP entry): the 3-iteration
INIT_BOOT_CDS loop still lands `required_drvr := self` on the last
config because FIND_EMPTYSLOT keeps returning the same slot (see
P98 — string compare is a separate bug). Left unchecked, UP()
recurses infinitely and blows the stack. Scaffold clears
`required_drvr == cfg_ptr` on UP entry so the compiled code can
advance. Remove when the bitbucket-init for-loop codegen is fixed.

### P101+P102 codegen fix (2026-04-18 late night, commit `d7f64c6`): exact-match type lookup + snapshot kind

Two-part fix for a cross-unit type collision. Apple-Pascal 8-char-significant
naming collapses `LogicalAddress` (HWINT, TK_LONGINT) and `logicaladr` (a
local record inside EXCEPRES's showregs, TK_RECORD) into the same 8-char
prefix "LOGICALA". Our shared_types pool ends up with both entries
side-by-side, and the type pointer cached in AlarmAssign's imported
proc sig could be misidentified when later code paths dereference it.

- P101: `find_type` prefers exact full-string strcasecmp match before
  falling back to 8-char significant match.
- P102: `register_proc_sig` snapshots the parameter's type KIND into
  `sig->param_type_kind[]` at registration time. ARG_BY_REF consults
  the snapshot instead of dereferencing the cached pointer, which
  might point at a mutated or unrelated entry after other units are
  parsed.

Concrete effect: PARAMEMINIT's
`ALARMASSIGN(param_mem.alarm_ref, ord(@PARAMEM_WRITE))` now compiles
correctly — pushes `@alarm_ref` (var arg) + `@PARAMEM_WRITE` (routine
addr) — instead of pushing `@alarm_ref` twice. Alarm handlers now
register with real code addresses instead of pointing at their own
alarm slot. Interrupts drop to IPL=0 (scheduler idle) after
PARAMEMINIT — that hadn't happened pre-P102.

### P98 codegen fix (2026-04-18 night, commit `d77c49a`): string-compare branches off-by-2

`src/toolchain/pascal_codegen.c` AST_BINARY_OP string-compare inline:
all three conditional branches in the byte-loop template had the
wrong displacement — they landed on the subsequent `BRA.S +2`
instruction instead of on `MOVEQ #1,D0` / `MOVEQ #0,D0`. D0 then
held whatever value was there before the compare (typically the
literal address loaded at `MOVE.L A0,D0`), so `TST.W D0` was
almost always nonzero and `str = 'literal'` returned TRUE
unconditionally.

- BNE after length CMP:  was $660E → fixed $6610
- BEQ after length TST:  was $670A → fixed $6708
- BNE inside char loop:  was $6606 → fixed $6608

Concrete symptom: `configinfo[i]^.devname = 'BITBKT'` in
FIND_EMPTYSLOT matched the first iteration unconditionally, so
NEW_CONFIG kept getting the same config_index each loop iteration,
producing the self-referential required_drvr that UP's scaffold is
currently masking. Post-P98, the compare returns the right
boolean — but surfaces the next bug: the bitbucket-init for-loop
doesn't populate all configinfo slots, so no slot ever has
'BITBKT' devname, FIND_EMPTYSLOT returns FALSE, and MAKE_BUILTIN
fires `SYSTEM_ERROR(sys_err_base+cdtoomany)` = 10758. Added to
the suppression list at `src/lisa.c:3090` until that codegen is
fixed.

### P102b proc-resolution fix (2026-04-18 post-23/27, commit `10b8c71`): longest-common-prefix on 8-char-significant collisions

Both `find_proc_sig` (in `src/toolchain/pascal_codegen.c`) and the
linker's `find_global_symbol` (in `src/toolchain/linker.c`) used to
fall back to a first-match-wins 8-char prefix scan on strcasecmp
miss. That made Apple's 8-char-significant identifier rule go
wrong when multiple procs shared the first 8 chars: the compiler
emitted the caller with no signature (so all args pushed as 4-byte
values), and the linker resolved the reloc to whichever symbol
appeared first in its table — which was arbitrary across builds.

The concrete victim: `INIT_TWIGGGLOB(error, index)` at
STARTUP.TEXT:1906. The declared proc is `INIT_TWIGGLOB(var err,
driv)` (in source-twiggy.text). Both truncate to `INIT_TWI`. The
asm `INIT_TWIG_TABLE(adr: absptr)` (in source-LDTWIG.TEXT) also
starts `INIT_TWI`. Pre-fix, find_proc_sig returned NULL (no
strcasecmp match) and the linker picked `INIT_TWIG_TABLE`. Callers
pushed two 4-byte values (`error=0`, `index`) and JSR'd into the
asm, which popped A0=retAddr, A1=0, then attempted a 93-word
table-copy to (A1)+ — wiping the vector table. The P102a scaffold
short-circuited this copy at $7150A.

Fix: when the exact-match tier misses, score every same-8-prefix
candidate by case-insensitive longest common prefix with the
reference name, and pick the highest-scoring one. For
`INIT_TWIGGGLOB`:
- vs `INIT_TWIGGLOB`  → LCP 10 ✓ (wins)
- vs `INIT_TWIG_TABLE` → LCP 9

Applied symmetrically in codegen + linker. find_proc_sig now
returns the right signature so the VAR err param pushes `@error`
via LEA+MOVE.L, index pushes as a 2-byte WORD, 6-byte cleanup.
The linker resolves the reloc to `INIT_TWIGGLOB` at $696DA, not
`INIT_TWIG_TABLE` at $71460. P102a scaffold retired in the same
commit — it never fires anymore.

Milestone effect: initially regressed 23/27 → 22/27 because the
LCP fallback was over-permissive (see P102c below). Post-P102c,
fully recovered to 23/27 with no downstream collateral.

### P102c proc-resolution scope fix (2026-04-18 post-P102b, commit `27df41d`): require both names ≥ 8 chars for LCP

P102b's LCP fallback used `mlen = min(len(name), 8)`, so a short
reference name like `Dimcont` (7 chars, a WITH-block field in
SET_PREFERENCES) would scan for candidates matching its full
7-char prefix and LCP-pick the best. That swept in
`DimContrast` (11 chars, a LIBHW global) because its first 7
chars match `Dimcont`. Pascal's SET_PREFERENCES
`SETDIMCONTRAST(DimConert(Dimcont))` (STARTUP.TEXT:1086) then
compiled to an injected `JSR DimContrast` between the field load
and the real `JSR DIMCONERT` — corrupting the stack and, via a
chain of mutations on the sysglobal free-list head, deadlocking
RELSPACE at ~$7160 after the 10707 suppression. Boot dropped
from 23/27 → 22/27.

Fix: require BOTH the reference AND the candidate to be >= 8
chars before the 8-char-significance prefix rule applies. This
matches Apple Pascal exactly: two identifiers are equal iff their
first-8 truncations are equal, and a name shorter than 8 chars
can only equal another name via full exact strcasecmp — never a
longer name's prefix. `Dimcont` (7 chars) thus falls through the
LCP tier entirely, find_proc_sig returns NULL for it, and the
call site emits a plain field load + no-arg call dispatch; the
spurious JSR disappears.

Triple-diff verification: the only symbol count changes between
pre-P102b (`85f129a`) and P102c (`27df41d`) are the two
intentional shifts (INIT_TWIG_TABLE −1, INIT_TWIGGLOB +1). Zero
collateral.

### P103 codegen fix (2026-04-18 later, commit `cc905c9`): preserve INTERFACE sig on asterisk-comment body decl

Apple Pascal lets IMPLEMENTATION re-declare a proc with its args
as an asterisk comment instead of a real arg list:

    procedure real_mount (* var ecode : error; device : integer *);

Our lexer correctly eats `(* ... *)` as a comment, so the parser
sees `procedure real_mount;` with no param list. `gen_proc_or_func`
then unconditionally called `register_proc_sig(cg, name, NULL, 0,
...)`, creating a *duplicate* 0-param sig that shadowed the real
sig from the INTERFACE pre-pass.

Consequence: `fs_mount`'s call to `real_mount` resolved to the
0-param sig, so the caller emitted generic 4-byte value pushes
instead of the `@ecode` pointer the var arg required. `real_mount`
read $8(A6) expecting an address, got the VALUE of `fs_mount`'s
ecode (= 0), treated it as A1 pointer, and cascaded a nil-@ecode
through `def_mount`. `MDDF_IO` never ran because `real_mount`
returned to `fs_mount` before reaching the MDDF read. Full boot
chain: `InitQVM → InitBufPool → InitFS → fs_mount → real_mount →
def_mount → [skipped MDDF_IO] → 10707`.

Fix: in gen_proc_or_func, only register a fresh 0-param sig when
no prior sig exists or the prior sig has 0 params. If the
INTERFACE already registered a real sig for this name, refresh its
nest_depth to the body's scope but keep the param descriptors.

Post-fix, the full chain runs: `InitQVM → ... → real_mount →
def_mount → MDDF_IO` with correct var-pointer semantics. Boot
still halts at 23/27 because MDDF_IO's real downstream (psio →
LisaIO → UltraIO) hits E_IO_MODE_BAD — that's what P104 targets.

### P104 MDDF_IO HLE scaffold (2026-04-18 late, commit `bb3fbec`): bypass the psio → LisaIO → UltraIO chain

The disk-IO chain under MDDF_IO requires:
1. `configinfo[bootdev]^.blockstructured = true` (else UltraIO
   aborts with `E_IO_MODE_BAD`=803). Set by NEW_CONFIG via
   `blockstructured := (devt = diskdev)`, which in turn depends on
   `cd_devt` coming from the driver's info record.
2. `configinfo[bootdev]^.ext_diskconfig.fs_strt_blok` populated
   (else LisaIO adds geography.firstblock = garbage to the block
   number and UltraIO tries to read block `$00CBFFFF`).

Both are real-driver responsibilities that only a compiled
SYSTEM.CD_PROFILE (Phase 3 work) can satisfy. Until then, P104
intercepts at `MDDF_IO`'s entry (higher than UltraIO) and reads
the MDDF block directly from the ProFile image via
`hle_read_block`. The HLE sets `*ecode=0` on success.

Current blocker after P104: MDDF_IO returns ecode=0, but
real_mount's subsequent field checks on `MDDFdata^` see
`fsversion`, `MDDFaddr`, or `volname.length` outside the expected
range (REL1/PEPSI/CUR for fsversion, 0 for MDDFaddr, ≤max_ename
for volname length) and raise `E_FS_VERSION`=866 → 10707.
Means our disk image's on-disk MDDF bytes don't align with
Apple's `MDDFdb` Pascal record layout at Apple's expected
offsets. Next session: compare `diskimage.c`'s MDDF writer
against `SOURCE-VMSTUFF.TEXT:55` (the MDDFdb record) and align
offsets.

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
  chaining).
- **Header alignment fix (`fa203e2`, 2026-04-18 PM):**
  `lisa.h:BOOT_TRACK_BLOCKS` was stale at 24 after step 3c bumped
  diskimage.h to 64. Restored runtime milestones from 5 to 21.
  (The "27/29 milestones resolved" cited in step 3 commit messages
  was symbols-in-map, not runtime — misleading.)
- **Phase 2 Step 4a (`d5e50e1`, 2026-04-18 PM):** smart
  ENTER_LOADER HLE dispatching loader calls against ldr_fs. 21 →
  24 runtime milestones (INIT_DRIVER_SPACE, SYS_PROC_INIT,
  PR_CLEANUP). 10738 dead. Layered scaffolding HLE — pending
  BT_PROFILE relink for real LOADER integration.
- **P91 codegen fix (`94903b9`, 2026-04-18 PM):** integer params
  at positive frame offsets now get `EXT.L D0` sign-extension
  after the `MOVE.W` load. Fixes the `count := pages` bug in
  LD_READSEQ that was causing `count=$000100CB` instead of `1`.
  Milestone count is now 21/27 (real) vs the prior 24/27 (fake,
  from PC wandering through corrupted state after a 33 MB
  over-read). Last real checkpoint: BOOT_IO_INIT. Next real
  blocker: `SYSTEM_ERROR code=204` — investigate in next session.

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
- **10738..10741 suppression** (`src/lisa.c:3090`) — safety net for
  alternate boot paths (PRAM failures → 10738, LOADCD failures → 10739).
  On the ProFile-on-paraport default path post-P97..P100, none of these
  fire — UP() succeeds cleanly, blockstructured comparison works,
  FIND_EMPTYSLOT finds real slots.
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
