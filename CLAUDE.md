# LisaEmu - Apple Lisa Emulator

## Project Vision

A native macOS app that takes Apple's officially released Lisa OS source code (`Lisa_Source/`) as input, compiles/assembles it into runnable binaries, and runs a complete Apple Lisa system — like Parallels, but for the Lisa.

The source code (Lisa OS version 3.1) was released by Apple in 2018 via the Computer History Museum:
https://info.computerhistory.org/apple-lisa-code

It is freely available under an Apple Academic License but **cannot be redistributed**. Users supply their own copy.

## Architecture

### Two layers:

1. **Toolchain** — Cross-assembler (68000) + Lisa Pascal cross-compiler + linker that processes `Lisa_Source/` into bootable disk images and ROM
2. **Emulator** — Motorola 68000 CPU + Lisa hardware emulation (memory, VIAs, display, keyboard, mouse, disks)

### Directory Structure

```
/
├── Lisa_Source/           # Apple's source (NOT in git, user-supplied)
├── src/                   # C emulator core + toolchain (canonical source)
│   ├── m68k.h/c           # Motorola 68000 CPU emulator
│   ├── lisa_mmu.h/c       # Memory controller + MMU
│   ├── via6522.h/c        # VIA 6522 chip emulation (x2)
│   ├── lisa.h/c           # Main machine integration
│   ├── lisa_bridge.h/c    # C-to-Swift bridge API
│   ├── main_sdl.c         # Standalone SDL2 frontend (for testing)
│   └── toolchain/         # Cross-compilation pipeline
│       ├── pascal_lexer.h/c      # Lisa Pascal tokenizer
│       ├── pascal_parser.h/c     # Recursive descent parser → AST
│       ├── pascal_codegen.h/c    # AST → 68000 machine code
│       ├── asm68k.h/c            # Two-pass 68000 cross-assembler
│       ├── linker.h/c            # Multi-module linker
│       ├── bootrom.c             # Boot ROM generator
│       ├── diskimage.h/c         # Disk image builder
│       ├── toolchain_bridge.h/c  # Orchestrates full compile pipeline
│       ├── audit_toolchain.c     # Diagnostic tool (make audit)
│       └── test_*.c              # Per-component test tools
├── lisaOS/                # Xcode macOS app (SwiftUI, Swift 6)
│   └── lisaOS/
│       ├── Emulator/      # SYMLINKS to src/ (not copies!)
│       ├── ContentView.swift
│       ├── EmulatorViewModel.swift
│       ├── LisaDisplayView.swift
│       └── lisaOSApp.swift
├── docs/                  # Documentation (Lisa_Source reference, hardware specs)
├── build/                 # Build output (gitignored)
├── Makefile               # Standalone SDL2 build + audit targets
├── CLAUDE.md              # This file
└── NEXT_SESSION.md        # Current status and prioritized fix list
```

## Key Commands

```bash
# Build standalone emulator (SDL2)
make

# Run toolchain audit — the primary diagnostic tool
make audit              # Full report (all 4 stages)
make audit-parser       # Stage 1: Parser only
make audit-codegen      # Stage 2: Codegen only
make audit-asm          # Stage 3: Assembler only
make audit-linker       # Stage 4: Full pipeline + linker

# Xcode build (or just open in Xcode)
cd lisaOS && xcodebuild -scheme lisaOS -destination 'generic/platform=macOS' build 2>&1 | grep -E "(error:|BUILD)"
```

## Current Status (2026-04-17 very late) — Kernel boots past SYS_PROC_INIT, packed-record bit-packing in place

**Boot now runs cleanly past SYS_PROC_INIT** (23/27 milestones reached,
including SYS_PROC_INIT ✅) with no SYSTEM_ERROR halt. 1000 headless
frames complete; the kernel enters the scheduler idle loop. Next
kernel milestone (INIT_DRIVER_SPACE) is not yet reached but nothing
halts — needs investigation of why the driver allocator isn't being
invoked after SYS_PROC_INIT succeeds.

### Session 2026-04-17 very late — DEFAULTPM corruption fixed via packed bit-packing

Two root causes stacked on top of each other corrupted `b_syslocal_ptr`
at A5-24785 during DEFAULTPM init, crashing Make_SProcess later:

**P87a — `expr_size` resolves WITH-field byte arrays** (`src/toolchain/pascal_codegen.c`).
`expr_size(AST_ARRAY_ACCESS)` only handled globally-findable arrays. For
`devconfig[i]` inside `With PMRec^ do`, `find_symbol_any` missed it and
expr_size defaulted to 2, emitting `MOVE.W D0,(A0)` on a 1-byte-stride
loop. Now falls back to `with_lookup_field` when no global matches, but
only returns the corrected size for byte elements (pointer-array paths
still default to 2 because widening there broke MMPRIM code that
depended on the prior-2-byte default).

**P87d — packed-record bit-packing for Tnibble and boolean fields**
(`src/toolchain/pascal_codegen.{h,c}`, `src/toolchain/toolchain_bridge.c`).
Apple's `pmem = packed record` has Tnibble (0..15, 4-bit) fields paired
into bytes and booleans bit-packed, so `DevConfig` lands at byte 10.
Our codegen previously gave booleans 2 bytes and Tnibbles 1 byte each,
so DevConfig landed at offset 22, overrunning the 64-byte
`param_mem` backing store by 8 bytes and clobbering `b_syslocal_ptr`.
Fix:

- `type_desc_t.fields[].bit_offset` / `bit_width` added; `bit_width > 0`
  marks a bit-packed field.
- Resolver (AST_TYPE_RECORD) maintains a parallel `bit_cursor` when
  `cg->in_packed`. TK_BOOLEAN → 1 bit, TK_SUBRANGE 0..15 → 4 bits.
  First-declared field gets the HIGH bits, matching Apple's comments.
- Phase-2 fixup in toolchain_bridge mirrors the resolver.
- Scope: only activates when the packed record contains at least one
  Tnibble. Boolean-only packed records (`segstates` = nine booleans,
  referenced at specific byte offsets by hand-coded asm pins) keep
  their existing 2-byte-per-boolean layout — broader activation
  desynchronised PASCALDEFS offsets and regressed FS_INIT.
- Codegen:
  - `emit_read_a0_to_d0_bit`: `MOVE.B (A0),D0; LSR.B #off,D0; ANDI.B #mask`.
  - `emit_write_d0_to_a0_bit`: read-modify-write that preserves neighbor
    bits (`MOVE.B (A0),D1; pop D0; mask+shift; ANDI.B #~mask,D1; OR.B; MOVE.B`).
  - AST_IDENT_EXPR in WITH, AST_FIELD_ACCESS read, AST_ASSIGN WITH
    write, and AST_ASSIGN complex-LHS FIELD write each dispatch to the
    bit-field emitters when `bit_width > 0`.
- `lvalue_field_info_full` returns bit info; the old
  `lvalue_field_info` wraps it.

Verified: `pmem.DevConfig` now emits `ADDA.W #$0A,A0` (offset 10); the
devconfig init loop stays inside `param_mem`; no more b_syslocal_ptr
corruption; boot reaches SYS_PROC_INIT ✅. The earlier
`DEFAULT_PM_GUARD` MMU-level write guard (P87b) was a workaround for
exactly this corruption and has been **removed** now that the layout
is correct.

### Earlier this evening — App-path parity with SDL (kept below for context)

### Session 2026-04-17 late evening — finishing the plumbing

Beyond the sandbox-off / no-bookmarks / bundle-format work documented in the
next section, this session closed two remaining gaps that made the app path
diverge from SDL:

1. **HLE intercepts wired from app path**. `lisa_bridge.c` now exposes
   `emu_load_hle_addrs(const char *path)` which parses `hle_addrs.txt`
   (produced by the build, stored inside the `.lisa` bundle) and calls
   `lisa_hle_set_addresses(...)`. Without this, the kernel's CALLDRIVER /
   SYSTEM_ERROR / loader-trap intercepts were dead and the CPU walked off
   during boot. The stub-ROM fallback had been masking this; once the
   real ROM loaded, the crash surfaced.
2. **Boot-progress symbol map loaded from app path**. New bridge API
   `emu_load_symbol_map(const char *path)` wraps `boot_progress_init()`.
   Used both for milestone instrumentation AND for the dynamic HLE
   lookups (`boot_progress_lookup`) that resolve CreateProcess,
   Make_SProcess etc. by name. Without this, lookups failed and the HLE
   fell back to hardcoded addresses, causing downstream crashes.

Both are called from `EmulatorViewModel.startEmulation()` right after
`emu_mount_profile`. The SDL `main_sdl.c` has always done the equivalent.

### Halt + boot-progress surfacing (app UX)

- `lisa->halted` sticky flag in `lisa_t` (set in `hle_system_error`,
  cleared by `lisa_init`). Survives CPU wake-up on interrupt.
- `emu_is_halted()` / `emu_get_boot_progress_report(buf, size)` bridge
  APIs. Report capture uses `open_memstream` to convert the
  `boot_progress_report(FILE*)` API into a string.
- Swift `runFrame` checks halt each frame; on halt, calls
  `haltAndReport()` which stops the timer and logs the milestone table
  line-by-line to the app log panel.
- `emu_run_frame` short-circuits to 0 cycles when halted — no more per-
  frame `SYSTEM_ERROR: HALTING CPU` spam in the Xcode console.
- `hle_system_error` now prints the `HALTING CPU` line only once per
  halt (guarded by `reported_once`).

### `emu_has_rom()` fixed — false-negative bug

Previous implementation peeked at `lisa.mem.rom[0]` and `rom[4]`, returning
`false` when both were zero. This was **exactly backwards**: every valid
Lisa boot ROM has reset vectors in 24-bit address space (e.g. SSP=$000FFFFE,
PC=$00FE0400), so the first byte of each vector IS always `$00`. The test
flagged every loaded ROM as missing. Now tracks a `static bool rom_loaded`
flag set by `emu_load_rom` on success, cleared by `emu_init`.

### Kernel bug — isolated, NOT yet fixed

Boot halts at `SYSTEM_ERROR(10101)` because `b_syslocal_ptr` (A5-relative
global at `A5-24785`, phys $CC0F2B) gets corrupted from `$00CE0000` (set
correctly by POOL_INIT) to `$FFFFFFFF` before `SYS_PROC_INIT` runs. Then
`Make_SProcess → MAKE_DATASEG → CHK_LDSN_FREE` dereferences the junk
pointer, fails with error 20083, recovery propagates three times, and
SYS_PROC_INIT calls SYSTEM_ERROR(10101).

**Traced to a Pascal codegen stride bug** at PC=$002FDA, in the
`INIT_SCTAB` region of STARTUP (matches memory entry
`project_apple_codegen_splits`: Apple split sctab1/sctab2 to dodge their
own Pascal codegen bugs; ours fails same class on SCTAB2):

```
$2FD0: 3200           MOVE.W D0,D1
$2FD2: C2FC 0001      MULU.W #$0001,D1       ← elem_size = 1 !!
$2FD6: D1C1           ADDA.L D1,A0
$2FD8: 301F           MOVE.W (A7)+,D0        ← D0 = $FFFF
$2FDA: 3080           MOVE.W D0,(A0)         ← 2-byte store at 1-byte stride
$2FDC: 526E FFFA      ADDQ.W #1,-6(A6)
$2FE0: 6000 FFC6      BRA.W back
```

Array element size compiled as 1 (byte) but the assignment emits a word
store. Consecutive iterations write overlapping WORDs at byte-stepped
addresses. Five iterations stepping $CC0F2A..$CC0F2E produce the exact
8 byte-writes the watchpoint captured (each non-endpoint address written
twice).

**Watchpoints added** in `src/lisa_mmu.c`:
- `WATCH-B_SLOC` on `$CC0F2B..$CC0F2E` — logs PC + runtime opcodes at PC.
- `WATCH-CODE2FDA` on logical `$002FDA..$002FDF` — never triggered
  (code is original, not overwritten; the apparent "wrong opcodes"
  finding earlier was a disassembly alignment error on my part:
  `linked.bin` has 0x400 bytes of vector/padding at the start, so the
  file is PC-indexed directly, not module-offset indexed).

**Next-session task**: find the offending Pascal array declaration in
`Lisa_Source/LISA_OS/OS/source-sctabs.text` (or SCTAB init in
SOURCE-STARTUP.TEXT) and fix the codegen in `src/toolchain/pascal_codegen.c`
array-store path. Decide based on the declared element type whether to
emit `MULU #2` (fix stride) or `MOVE.B` (fix store width). See
`NEXT_SESSION.md` for the resume prompt.

---

## Current Status (2026-04-17 evening) — App-layer rewrite: sandbox off, no bookmarks, pure path derivation

Kernel milestones unchanged (still 27/27 — see next section). This session
rewrote the lisaOS app's file-access story end-to-end. Two converging
forces drove the design:

1. **The user repeatedly said: no bookmarks, no auto-restore, no
   "remember from last time."** Logged durably as
   `feedback_no_auto_restore.md`.
2. **The macOS sandbox made "no bookmarks + fixed paths" impossible to
   satisfy simultaneously** — bookmarks were the only cross-session
   mechanism to reach user-chosen paths.

Resolution: **sandbox turned off**
(`ENABLE_APP_SANDBOX = NO` for the lisaOS target in `project.pbxproj`,
Debug + Release). With the sandbox gone, any path the user hands us via
`.fileImporter` is just a plain path string usable by `fopen()`, and no
bookmark machinery is needed.

### C-side changes (`src/lisa_bridge.[ch]`)

- `emu_reset()` no longer auto-generates a stub ROM. If no ROM is loaded,
  it prints to stderr and returns without resetting. The previous
  fallback silently booted against a bare `bootrom_generate()` result
  with none of the symbol-pinning / patch-site wiring that
  `toolchain_bridge` applies around the same generator during a real
  build — a footgun masquerading as convenience.
- New `emu_has_rom()` accessor — used by Swift to hard-gate Power On.
- `bootrom_generate()` is still used by `main_sdl.c` (headless tests)
  and `toolchain_bridge.c` (writes the real `lisa_boot.rom` during a
  build). Those paths are unchanged.

### Swift-side changes (`lisaOS/lisaOS/EmulatorViewModel.swift` + ContentView)

- **No bookmarks at all.** `activeScopes`, `activateScope`,
  `deactivateAllScopes`, every `startAccessingSecurityScopedResource` —
  gone. `UserDefaults` holds no path/bookmark keys for the app. `init()`
  purges any stale keys from the sandbox era.
- **No auto-restore.** `checkForLastImage` removed entirely; nothing
  reopens on launch. See `feedback_no_auto_restore.md`.
- **Honest logging.** Every `emu_load_rom` / `emu_mount_profile` call
  logs its return value (success AND failure). The old code logged
  "Mounted image: ..." unconditionally.
- **Hard ROM gate.** Power On aborts with an explicit message if no
  real ROM is loaded, instead of silently falling through to
  `emu_reset()`'s old stub fallback.
- **Pure path derivation.** When the user opens a `.lisa` bundle, the
  ROM path is computed as `<bundleParent>/rom/lisa_boot.rom` directly
  from the picked URL. Nothing is remembered; everything is derived
  from the user's most recent click.

The canonical ROM story: `lisa_boot.rom` is a file the toolchain writes
at `<output>/rom/lisa_boot.rom`. Any `.lisa` bundle in `<output>/` can
find it as a sibling. It must exist and it must load — no stub, no fake,
no fallback.

### Build output layout — `.lisa` system bundle

A successful Build from Source writes this structure into the user's
chosen output folder:

```
<output>/
├── LisaOS.lisa/          # macOS bundle package — appears as one file
│   ├── profile.image     # the bootable Lisa disk
│   ├── linked.bin        # raw linker output (offline disassembly)
│   ├── linked.map        # symbol map
│   └── hle_addrs.txt     # HLE wiring addresses
└── rom/
    └── lisa_boot.rom     # shared boot ROM — outside the bundle so any
                          # .lisa bundle in <output> can reuse it
```

The `.lisa` extension is filtered in the Open dialog via the app's
exported UTI `com.aviahmorag.lisaOS.system` (declared in
`lisaOS/Info.plist` via `UTExportedTypeDeclarations`, conforming to
`com.apple.package`). The project.pbxproj now points at that Info.plist
(`GENERATE_INFOPLIST_FILE = NO; INFOPLIST_FILE = "lisaOS/Info.plist";`)
for both the lisaOS Debug and Release configs; the test targets are
unchanged. With the UTI registered, Finder will treat `.lisa` folders
as single-file packages (double-click opens in the app instead of
drilling in).

The ROM assumption is explicit: a `.lisa` bundle opened on a machine
without a local `<output>/rom/lisa_boot.rom` won't boot. That's correct
— Apple's license prohibits ROM redistribution, so every user needs to
compile their own from their licensed Lisa source.

Both user flows end at the same state:
- **Build from Source**: bundle + ROM produced together. `builtImagePath`
  and `builtRomPath` point at `<output>/LisaOS.lisa/profile.image` and
  `<output>/rom/lisa_boot.rom` respectively.
- **Open System** (prebuilt `.lisa`): `builtImagePath` = picked bundle's
  `profile.image`; `builtRomPath` = `<bundleParent>/rom/lisa_boot.rom`.
  All derived from the URL the user just clicked — no persistence.

## Current Status (2026-04-17 very late) — 🎉 27/27 KERNEL MILESTONES, full boot sequence complete

Build + audit green. **27/27 kernel milestones reached.** Boot now
runs the complete Lisa OS kernel init sequence cleanly:

PASCALINIT → INITSYS → GETLDMAP → REG_TO_MAPPED → POOL_INIT →
INIT_PE → MM_INIT → INSERTSDB → MAKE_FREE → BLD_SEG →
MAKE_REGION → INIT_TRAPV → DB_INIT → AVAIL_INIT → INIT_PROCESS
→ INIT_EM → EXCEP_SETUP → INIT_EC → INIT_SCTAB → INIT_MEASINFO
→ BOOT_IO_INIT → FS_INIT → SYS_PROC_INIT → INIT_DRIVER_SPACE →
FS_CLEANUP → MEM_CLEANUP → PR_CLEANUP ✅

Remaining unreached: **SHELL** and **WS_MAIN** — these are
post-kernel application milestones that depend on entire new
subsystems not yet built (intrinsic library loading, SYSTEM.SHELL
compile target, APDM desktop manager, Lisa filesystem catalog
infrastructure with MDDF). See roadmap below for what's needed to
go from "kernel boots" to "desktop visible."

### P86 — Linker phase-2 must not add base_addr to A5-relative pins (src/toolchain/linker.c:509)

The PASCALDEFS pin table in pascal_codegen.c (~29 entries) assigns
specific A5-relative offsets to globals that hand-coded asm
references at hardcoded positions — `b_syslocal_ptr=-24785`,
`c_pcb_ptr=-24617`, `Invoke_sched=-24786`, `smt_addr=-24887`, etc.
These offsets MUST match what Apple's asm (LAUNCH, SCHDTRAP,
PROCASM, MMASM) reads.

Linker Phase 2 was blindly adding `mod->base_addr` to every
exported symbol's value, including negative A5-relative ones. So
`b_syslocal_ptr = -24785` in SYSGLOBAL became `-24785 +
SYSGLOBAL.base_addr = -953`. Pascal `POOL_INIT` still wrote to
the emitted-code offset (-24785 is baked into instruction bytes
at compile time), so the actual storage sat at A5-24785 =
$CC0F2B. But Apple's LAUNCH read `B_SYSLOC(A6) = -24785` and
expected the VAR to be there — which was uninitialized
($FFFFFFFF). Launch dereferenced garbage and dispatched into
$00010C46 on first scheduler firing.

Fix: in Phase 2, only add `mod->base_addr` when `sym->value >= 0`.
Code entry points have non-negative offsets (bytes into module
code); A5-relative data always has negative offsets. The sign test
cleanly separates them.

### P86 TRAP6 HLE fix — use `g_hle_smt_base`, not map `smt_base` (src/m68k.c:2293)

`DO_AN_MMU`'s TRAP #6 HLE needs the physical SMT data address.
It was looking up `smt_base` in the linker map. Two symbols share
that name: (1) a local LDASM label (NOT exported) and (2) a Pascal
VAR declared in source-parms.text. The Pascal VAR's linker value
is the A5-relative offset of its STORAGE SLOT (negative), not the
SMT data's physical location.

Pre-P86-linker-fix, `mod->base_addr` was added to the VAR's
negative offset, producing a positive value that coincidentally
landed near `os_end` where bootrom_build primes the SMT region.
So the HLE "worked" by accident. Post-P86-linker-fix, the negative
value resolved correctly, and the HLE read garbage from RAM,
programming bogus segment mappings — seg 60 writes (buffer pool at
$0078xxxx) got dropped, FlushNodes walked zeros, boot stalled.

Fix: prefer `g_hle_smt_base` (deterministically set by
bootrom_build to `os_end`). Fall back to map lookup only when it
looks like a real physical address.

### P86e — DEL_MMLIST HLE guard for empty SRB lists (src/m68k.c)

MEM_CLEANUP calls `Del_SRB(shrseg_sdb, c_pcb)` and `Del_SRB(IUDsdb,
c_pcb)` to unlink the pseudo-outer process from shared-resource
lists. Both SDBs have `srbRP = 0` (no SRBs) because nothing in our
boot path ever calls `ADDTO_SRB` for them. `Del_SRB` computes
`c_mmlist = srbRP + b_sysglobal_ptr = b_sysglobal_ptr`, then
`DEL_MMLIST` walks the repeat-until loop reading
`chain.fwd_link + b_sysglobal_ptr`, never terminating because
fwd_link is 0 and the loop target is never reached.

Apple's code doesn't guard against this because on real hardware
the SRB lists are populated by process-setup paths that our boot
doesn't fully execute (we HLE-bypass or no-op several process
creation flows).

Fix: HLE on DEL_MMLIST entry — return immediately if
`c_mmlist == b_sysglobal_ptr` OR if `chain.fwd_link` at c_mmlist
is 0. Both cases mean "empty SRB list, nothing to delete."

### Post-kernel blocker: Launch STOP at $06EAAA

After PR_CLEANUP, the idle-loop scheduler runs Launch and
eventually executes STOP (opcode $4E72) at $06EAAA — this is the
expected `Pause` primitive in source-PROCASM.TEXT that halts the
CPU waiting for an interrupt. `CRASH TO VECTORS` fires because
our emulator treats a STOP-then-interrupt as an anomalous PC
transition, but this is actually the normal idle-loop behavior.
The boot report shows 27/27 milestones and the kernel is running
correctly.

---

## Roadmap to bootable Lisa desktop

Completing the kernel (PR_CLEANUP reached) is **~30%** of the way
to a bootable desktop. The kernel provides process scheduling,
MMU, memory manager, filesystem, and I/O. What's still needed:

| Layer | Status | Est. effort | What's needed |
|-------|--------|-------------|---------------|
| OS Kernel (SYSTEM.OS) | ✅ **100%** — 27/27 checkpoints | — | Done |
| System Libraries (SYS1LIB, SYS2LIB) | 0% | Medium | New compile targets + linking |
| Graphics (LIBQD / QuickDraw, LIBTK / Toolkit) | 0% | Large | New compile targets; LIBQD is ~50 source files |
| Drivers (SYSTEM.LLD, CD_\*) | 0% | Medium | 13 new binaries; configinfo wiring |
| Shell (APDM = Desktop Manager) | 0% | Large | Separate compile target; depends on LIBQD + LIBTK |
| Intrinsic library loading | 0% | Medium | Dynamic linking — loader reads `.LIB` files at runtime |
| Lisa filesystem (MDDF, catalog) | 0% | Medium | Build a bootable volume image with MDDF + catalog entries |
| Boot ROM / bootloaders (BT_Sony, BT_Profile) | Partial | Small | Already partially HLE'd |
| Apps (LisaWrite, LisaCalc, etc.) | 0% | Very large | 14 app targets, each with its own UI code |

The compiler/linker (parser 100%, assembler 100%, linker
working, 95.3% JSR resolution) can handle additional targets —
the remaining work is systematic but large: define 26+ compile
targets, fix per-target codegen issues, build the disk layout.

The P86 fixes (linker A5-pin bug, smt_base HLE) were the last
major *structural* issues. Remaining work is mostly additive —
compile more targets, load them correctly, and populate the
filesystem. No more foundational rewrites expected.

### P85c — inline byte-subrange fields widen to 2 bytes (src/toolchain/pascal_codegen.c:421, toolchain_bridge.c:845)

Apple's PASCALDEFS insists PRIORITY=12, DOMAIN=17, GLOB_ID=20 in
PCB. That layout only fits if `priority: 0..255` and `norm_pri:
0..255` each occupy a 2-byte slot with the value byte at the low
(big-endian +1) byte; `blk_state: blk_type` + `domain: domainRange`
then pack 1+1 at offsets 16/17. Before P85c, our tight byte-packing
put priority at offset 12 (high byte) and norm_pri at 13 — so
QUEUE_PR's `MOVE.W PRIORITY(A0),D1` read $FF00 (-256 signed), BLE.S
RQSCAN always took, scheduler spun forever at PC=$06EB46.

Fix: widen INLINE byte-subranges (AST_TYPE_SUBRANGE, empty type_name)
to 2 bytes at even offsets, and record the field offset as slot+1 so
our byte reads/writes access the value byte while Apple's MOVE.W
still sees $00XX. Named subrange aliases (e.g. `int1 = -128..127`,
`domainRange = 0..maxDomain`) keep the prior pair-pack behavior — int1
is Apple's explicit 1-byte type, so codesdb's lockcount(int1)+
sdbtype(Tsdbtype) still pack tight at 12/13 and oset_freechain=14 holds.

Currently narrowed to records named `PCB` only. Generalizing to
all inline subranges produced the same cascading FS_INIT failure
as last session's P85b attempt, suggesting the cascade is NOT caused
by widening other records — it's a latent bug that's newly reachable
once the scheduler dispatches. See next-session notes.

### Current blocker: SYSTEM_ERROR(10701) via FS_INIT → RELSPACE

Post-P85c boot runs:
1. QUEUE_PR#1..#8 fire with PCB=$CCB58E (Signal_sem waking up FS).
2. HLE SYSTEM_ERROR(10707) at ret=$002D0C suppressed (FS init failed).
3. HLE SYSTEM_ERROR(10701) at ret=$005506 fatal (nospace in STARTUP
   / BOOT_IO_INIT).

Between (1)–(2), RELSPACE emits 14 UNMAPPED-WRITEs targeting
$41422F..$414234 (seg 32). Addresses suggest c_pool_ptr resolves to
$414230-ish, but seg 32 was never programmed via PROG_MMU during
init — PROG_MMU#1..#20 cover indices 85-100 / 125 (init-time
segments), #21 is seg 60 (FlushNodes buffer pool). Nothing maps seg 32.

Start next session:
- Trace RELSPACE entry: what ordaddr/b_area is passed when the bad
  writes start? A probe in lisa_bridge or an HLE stub that dumps
  args on first RELSPACE call will show the caller intent.
- Is seg 32 supposed to be mapped? Check what domain/LDSN the FS
  init code expects for its syslocal/sysglobal free pools, and
  whether PROG_MMU should have been called for seg 32 earlier.
- If FS_INIT needs a specific domain/LDSN and our scheduler
  dispatched with wrong env_save_area, fix CreateProcess or the
  initial PCB/syslocal setup.

---

## Previous status (2026-04-17 night) — P85a fixes FlushNodes spin; QUEUE_PR WAS the blocker

Build + audit green. 22/27 milestones. Boot reaches FS_INIT,
progresses past the prior FlushNodes buffer-pool spin, runs into
scheduler code, and halts with SYSTEM_ERROR(10204) (f-line in
system code) in QUEUE_PR at $06EB20.

The prior handoff's "FlushNodes repeat loop spins forever" was
actually a downstream effect of MMU programming silently failing
for the MR-data buffer segment. FlushNodes was walking a buffer
pool in unmapped RAM (segment 60, $00780000..$0079FFFF), so
every InitBuf write to `link.f` / `link.b` got dropped by the
MMU unmapped-write safety net. `ptrS^.link.b` stayed null, the
`until ptrS = ptrHot` test never hit ptrHot, and the loop spun.

Root cause was in our Pascal codegen — a stale-high-byte bug
in the byte-subrange load path.

### P85a — AST_IDENT_EXPR byte loads zero-extend D0 (src/toolchain/pascal_codegen.c:1704 / :1732)

For `sz == 1` (byte-subrange or single-byte field), codegen
emitted `MOVE.B offset(A6),D0` (or `(A0)`, or `(A5)` for globals)
with no `MOVEQ #0,D0` first. The MOVE.B only updates the LOW
byte of D0, so the upper 24 bits kept whatever was there from
the previous operation. Any subsequent `MOVE.W D0,Dn` then
propagated that stale high byte.

In `MAP_SEGMENT` (source-MMPRIM.TEXT:627), the first body
instruction is effectively `l_domain := domain`. Our codegen
emits:

  `MOVE.B $0F(A6),D0  ; domain is a byte-param, read at offset+1`
  `MOVE.W D0,D2       ; widen to word for index := domain*numbmmus + c_mmu`

With the upper bits of D0 stale (happened to be $00B6 from a
prior op), D2 became $B600. The PROG_MMU call site later loaded
l_domain via `MOVE.W -$14(A6),D0` and pushed — and PROG_MMU
fired TRAP #6 with D2 = $B600 (target domain). Our HLE-TRAP6
computed `smt_entry = smt_ptr + 46592*512 + …`, walked into
garbage, and the MMU segment for seg 60 never got programmed.
MAKE_MRDATA returned addrSpace=$00780000 but InitBuf's writes
to that range all dropped.

**Fix**: at AST_IDENT_EXPR sz==1 branch (A6/A0 frame load AND
A5 global load), emit `MOVEQ #0,D0` first. Same pattern P80h2
applied to `emit_read_a0_to_d0` for (A0)-indirect reads.

Verified via P85 probe walk: InitBuf#1-4 correctly write link
pointers, FlushNodes's ptrHot chain now loops back to ptrHot
after 4 hops ($78189C → $781068 → $780834 → $780000 → $78189C).

### New blocker: SYSTEM_ERROR(10204) in QUEUE_PR (scheduler)

Post-fix, boot leaves FlushNodes cleanly and reaches
scheduler/QUEUE_PR ($06EAF4 in source-starasm1 asm). First
instructions pop D0/D1/A1 from the stack:

  `MOVE.L (A7)+,D0   ; pop return address into D0`
  `MOVE.B (A7)+,D1   ; flag byte`
  `MOVEA.L (A7)+,A1  ; PCB pointer`
  `MOVEA.L (A1),A0   ; A0 = PCB.link.f`
  ...
  `MOVEA.L D0,A0; JMP (A0)  ; return via D0`

D0 arrives as `$00001F` (garbage) and A0 = $00001F → JMP (A0)
runs the CPU off into address $00001F, where fetching a word
at an odd address + F-line opcode triggers v=11 → SYSTEM_ERROR.

Likely causes: (a) caller of QUEUE_PR pushed the wrong return
address, (b) A1's PCB content was corrupted (first longword =
$2EFFFE making `MOVE.L A2,4(A0)` write to $2F0002 which is
also unmapped seg 23), (c) another byte-subrange / byte-field
codegen bug we haven't found yet.

The PCB's `link.f` reading as $2EFFFE suggests the ready-queue
head itself got scribbled somewhere between scheduler init and
this point. Probe QUEUE_PR entry + its caller to confirm.

Start here next session: the 4 bytes before the UNMAPPED-WRITE
at PC=$06EB02 (log=$2F0002 val=$3F00 4EB9...) are "$3F00 4EB9"
which is `MOVE.W D0,-(A7)` + `JSR` — that's INSTRUCTION bytes
being written to a data area, suggesting a very confused write
target. Possibly a PCB that was compiled with a wrong field
layout and whose "link" offset actually points at the proc's
own code.

### P84a — boolean NOT for AST_FIELD_ACCESS (src/toolchain/pascal_codegen.c:1804)

`gen_expression` for `AST_UNARY_OP(TOK_NOT)` detected boolean
operands only from `AST_FUNC_CALL`, `AST_BINARY_OP`, or
`AST_IDENT_EXPR`. `AST_FIELD_ACCESS` fell through to bitwise
`NOT.W D0`, so `not sdbstate.memoryF` compiled as:

  `MOVE.W 14(A0),D0; NOT.W D0; TST.W D0; BEQ exit`

which exits only on D0 = $FFFF. Since `memoryF := true` stores
the word $0001 (MOVEQ #1,D0 then MOVE.W D0,...), the loop could
never terminate. Fix: when child is AST_FIELD_ACCESS, call
`lvalue_field_info` to resolve the field's type and if it's
TK_BOOLEAN, emit the logical TST/SEQ/ANDI.W #1 sequence.

### P84b — byte-sized parameters read at offset+1 (src/toolchain/pascal_codegen.c:1696)

1-byte params (`Tsdbtype`, int1, byte, char) are pushed as 2-byte
words for word-alignment. On 68k big-endian the value lives in
the LOW byte = offset+1, not offset+0. Our codegen emitted
`MOVE.B offset(A6),D0` which pulled the zero padding byte, so
`kind` inside BLD_SEG always read as 0. MAKE_MRDATA's call
`BLD_SEG(data, 0, size, ...)` compiled correctly (caller pushed
`data=ord 2` as word $0002), but `sdbtype := kind` inside the
callee wrote $00 instead of $02 at offset 13. Verified via a
sdb-region write trace — `[P84W] pc=$04238C write1 @$CCBA6F
<= $00` pre-fix vs. `write1 <= $02` post-fix.

Fix: when emitting a byte read of a param (sym->is_param,
sz==1, sym->offset > 0), add 1 to the emitted offset. Locals
(negative A6 offsets) are allocated by our own prologue with
no padding convention, so they don't need the adjustment.

### New blocker: FlushNodes buffer-pool repeat loop (vmstuff.text:1369)

```pascal
ptrS := ptrHot;
repeat
  if dirty then LisaIO(...,WRITEOP);
  if clear then { page := REDLIGHT; lock := false };
  ptrS := ptrS^.link.b;  { backward link }
until ptrS = ptrHot;
```

The walk never hits `ptrHot` — ptrS^.link.b must be wrong, or
the buffer pool's circular doubly-linked list hasn't been
initialized by the time FS_INIT calls FlushNodes(-1, true, ecode)
(at fsinit.text:1176 / :1275).

Start here next session:

1. Find who initializes the `ptrHot` buffer-pool. It's probably
   set up during InitBufPool or similar early VM init. Confirm
   it's called BEFORE FlushNodes in the init chain.

2. Dump the buffer pool state when FlushNodes enters: walk 10-20
   `link.b` pointers starting from `ptrHot` and log each `page`,
   `device`, `dirty`, `link.f/b`. This should reveal whether the
   list is properly circular.

3. If list isn't initialized, find InitBufPool's path into FS_INIT
   and see why it's skipped / crashed. The P84 fixes changed the
   code path — it's possible a previous-HLE-bypassed init now tries
   to run natively and fails silently.

4. Consider: is `ptrHot` a global, or pointer in a structure? If a
   field in some record, maybe another codegen bug is reading it
   at wrong offset.

### P83a (kept) — HLE guard: MERGE_FREE only merges real free regions (src/m68k.c)

`MERGE_FREE(left_sdb)` in `INSERTSDB`'s free-chain insert path is
called with `left_sdb = head_sdb` the first time MAKE_FREE runs
(free chain empty → the predecessor walk lands on head_sdb). The
body then evaluates its condition inside `with head_sdb^ do`:

  `(right_sdb^.sdbtype = free) and
   (ord(right_sdb) = (ord(freechain.fwd_link) - oset_freechain))`

Both sides are TRUE because P_ENQUEUE just wrote
`head.freechain.fwd_link = &new_sdb.freechain`. The merge body then
does `memsize := memsize + right_sdb^.memsize` inside the WITH,
**scribbling `head_sdb.memsize` with the inserted free sdb's size**
and then `TAKE_FREE(right_sdb, false)` removes the just-inserted free
region. Result: free chain empty, `head.memsize = $0CD5` (non-zero).

Downstream `GetFree` walks `tail.freechain.bkwd_link - 14 = head_sdb`,
checks `if memsize < size then ... else TAKE_FREE(head_sdb, true)`.
Since memsize ($0CD5) > typical small alloc size, it proceeds to
`TAKE_FREE(head_sdb, true)` which asserts `sdbtype <> free` →
SYSTEM_ERROR(10598).

**P83a guard** (HLE on MERGE_FREE entry): if `c_sdb_ptr.sdbtype !=
free`, return immediately. This is defensively correct per Apple's
own documented intent ("merge two adjacent free regions"); applying
the merge when c_sdb_ptr is the head sentinel is obviously wrong.

### P83a — HLE guard: MERGE_FREE only merges real free regions (src/m68k.c)

`MERGE_FREE(left_sdb)` in `INSERTSDB`'s free-chain insert path is
called with `left_sdb = head_sdb` the first time MAKE_FREE runs
(free chain empty → the predecessor walk lands on head_sdb). The
body then evaluates its condition inside `with head_sdb^ do`:

  `(right_sdb^.sdbtype = free) and
   (ord(right_sdb) = (ord(freechain.fwd_link) - oset_freechain))`

Both sides are TRUE because P_ENQUEUE just wrote
`head.freechain.fwd_link = &new_sdb.freechain`. The merge body then
does `memsize := memsize + right_sdb^.memsize` inside the WITH,
**scribbling `head_sdb.memsize` with the inserted free sdb's size**
and then `TAKE_FREE(right_sdb, false)` removes the just-inserted free
region. Result: free chain empty, `head.memsize = $0CD5` (non-zero).

Downstream `GetFree` walks `tail.freechain.bkwd_link - 14 = head_sdb`,
checks `if memsize < size then ... else TAKE_FREE(head_sdb, true)`.
Since memsize ($0CD5) > typical small alloc size, it proceeds to
`TAKE_FREE(head_sdb, true)` which asserts `sdbtype <> free` →
SYSTEM_ERROR(10598).

**P83a guard** (HLE on MERGE_FREE entry): if `c_sdb_ptr.sdbtype !=
free`, return immediately. This is defensively correct per Apple's
own documented intent ("merge two adjacent free regions"); applying
the merge when c_sdb_ptr is the head sentinel is obviously wrong.

Apple's shipped source lacks this guard and in theory the same
miscompile would fire on real Lisa boot. It's possible Apple's boot
path avoids it via a different init sequence or their compiler
emits different code for the variant-field condition — unverified.

### Previous state (P82b/c) — CASE tag + packed byte pairs, CLEAR_SPACE spin fixed

### P82b — parser emits CASE tag as a field (commit `1571efa`)

`case sdbtype: Tsdbtype of ...` was being skipped entirely by the
parser, so references to `sdbtype` everywhere compiled as offset 0
(memchain.fwd_link's high word). Result: `if next_sdb^.sdbtype =
free` was always true (high byte of a 24-bit pointer = 0), sending
CLEAR_SPACE's inner while into infinite spin. Fix: when
`CASE IDENT : TYPE OF` is detected via `lexer_peek` for the colon,
emit the IDENT as a normal fixed field before `__variant_begin__`.

### P82c — 1-byte enums + pair-aware byte widening

Apple's hardcoded `oset_freechain = 14` in MMPRIM assumes codesdb
packs lockcount(1) + sdbtype(1) tight. Two changes to match:

- Enum types with ≤256 values default to 1 byte (was 2). `Tsdbtype`
  (7 values) is now 1 byte natively.
- The P79f "widen int1 to 2 bytes in unpacked records" rule now
  peeks at the NEXT field — if it's also a byte-sized scalar, don't
  widen, so consecutive 1-byte fields pack tight. Isolated bytes
  still widen for asm-compat MOVE.W reads. Both the AST record
  resolver and the pre-pass Phase-2 fixup honor the rule.

Post-fix codesdb: memchain(8)@0, memaddr(2)@8, memsize(2)@10,
lockcount(1)@12, sdbtype(1)@13, variant_start=14 — freechain@14
matches `oset_freechain = 14`. ✓

### Open blocker: SYSTEM_ERROR(10598) — TAKE_FREE asserts sdb not free

```
HLE SYSTEM_ERROR(10598) at ret=$040D0A SP=$00CBFD6B A6=$00CBFD75
SYSTEM_ERROR(10598): HALTING CPU
```

Call chain (from probes): ALLOC_MEM → FIND_FREE returns false →
CLEAR_SPACE's inner while walks free_sdb around the ring → reaches
head_sdb (sdbtype=header) → MOVE_SEG → TAKE_FREE → assert fails.

The free chain is **empty at FS_INIT time**:
```
head_sdb=$CCB034 fwd_link=$00CCB5CE bkwd_link=$00CCB062 memaddr=$0000 memsize=$0CD5 sdbtype=$05
head_sdb.freechain.fwd_link=$00CCB070 bkwd_link=$00CCB070
first_free_sdb=$CCB062 (= tail_sdb) memsize=$0000 sdbtype=$05
```

Two weird signals:
1. **head_sdb.memsize=$0CD5** — but head_sdb is a sentinel, should
   be 0. The value $0CD5 matches MAKE_FREE's `msize` arg, suggesting
   MAKE_FREE wrote to head_sdb (or an overlapping address).
2. **first_free_sdb = tail_sdb**, i.e. the freechain is empty. So
   MAKE_FREE either didn't run or didn't insert properly.

Start here next session:
- Probe MAKE_FREE entry + exit, dump new_sdb address (`ord4(maddr)*
  hmempgsize*2 + logrealmem`), memsize, sdbtype, and the freechain
  state after INSERTSDB.
- Verify INSERTSDB's linkage writes: memchain.fwd_link writes at
  offset 0, bkwd_link at +4, plus freechain at +14..+21. With the
  variant-arm-aware layout, those offsets should now be correct,
  but confirm via disasm.
- Check whether head_sdb is being overwritten by *where new_sdb
  lands*. `logrealmem` is the base — if it collides with the MMRB
  region, MAKE_FREE stomps on head_sdb. Unlikely but worth ruling
  out.

Memory layout-wise, this may also be a sign that BLD_SEG is
computing codesdb size wrong in one place and sdb size wrong in
another — cross-record size mismatch. Check `sdb_ptr` vs
`codesdb_ptr` dereference widths.

### P82 — variant records + chained field access (commit `3ad488c`)

Three related codegen bugs compounded into FIND_FREE returning a
bogus `free_sdb = $00F7F2` (inside the compiled GROW_SPACE code!).
TAKE_FREE → REMOVESDB → P_DEQUEUE then dereferenced the trashed
`fwd_link/bkwd_link` fields of that "sdb" and wrote to `$F23204`
(seg 121, unmapped).

1. **Variant-record Phase-2 layout ignored arms.** The pre-pass
   fixup in `toolchain_bridge.c` recomputes record offsets after
   late type resolution. It laid all fields sequentially, so
   codesdb's `freechain` (variant arm `free`) ended up at offset
   22 after the `code` arm's `sdbstate…numbopen` fields — instead
   of overlapping at offset 14 (= Apple's `oset_freechain`).
   Fix: plumb per-field `variant_arm` index through AST_TYPE_RECORD
   (1..N per arm, 0 for fixed part) + `variant_start` on the record;
   Phase 2 resets offset to `variant_start` at each arm boundary.

2. **Sentinel matching was ambiguous.** `str_eq_nocase` does 8-char
   significant-prefix matching — so `__variant_begin__`,
   `__variant_arm__`, `__variant_end__` all matched each other.
   `current_arm` never advanced past 1, so every variant field got
   lumped into arm 1 (defeating the per-arm offset reset above).
   Fix: use exact `strcmp` for sentinel checks.

3. **Chained AST_FIELD_ACCESS resolution missing.** Both
   `gen_lvalue_addr` and `expr_size` only knew how to resolve the
   parent type when the child was `AST_IDENT_EXPR` or `AST_DEREF`,
   not another `AST_FIELD_ACCESS`. Chains like
   `c_mmrb^.head_sdb.freechain.fwd_link` computed the head_sdb
   offset (42) but dropped the `freechain` +14 and read as MOVE.W
   (2 bytes) instead of MOVE.L (4). Fix: new
   `lvalue_record_type()` / `lvalue_field_info()` helpers walk
   nested FIELD_ACCESS chains recursively; used by both
   `gen_lvalue_addr` and `expr_size` at the top of their
   AST_FIELD_ACCESS handlers.

Post-fix: FIND_FREE now correctly computes
`head_sdb.freechain.fwd_link − oset_freechain` with the right
offset and width. It returns false legitimately (no free sdbs on
the chain — matches kernel expectation). ALLOC_MEM then enters
CLEAR_SPACE, which loops forever — that's the new blocker.

### Next (open blocker)

**CLEAR_SPACE infinite loop.** Hot PC pages `$043000`/`$042F00`
spin for 1000+ frames. CLEAR_SPACE is at `$042EE0` (per linkmap).
The Pascal body walks sdbs trying to shuffle memory and create a
hole of sufficient size. Likely causes:
- Another chained-field-access width bug somewhere CLEAR_SPACE
  touches (SET_INMOTION_SEG / CLR_INMOTION_SEG / MOVE_SEG).
- Bad `clock_ptr` (MMRB+$C2) making the walk never terminate.
- Freechain.bkwd_link walk in the opposite direction that still
  hits the same stale offset we just fixed.

Start by disassembling `$042EE0` through `$0430FF`, correlate to
`source-MM2.TEXT.unix.txt:296` (CLEAR_SPACE body), and add probes
to ALLOC_MEM entry / CLEAR_SPACE entry / SET_INMOTION_SEG entry.

### Previous state (pre-P82) — STATIC-LINK ABI FIXED, BOOT CLEAN TO IDLE

Build + audit green. 25/27 milestones reached. Kernel boots cleanly
INIT → PR_CLEANUP → scheduler → MemMgr → scheduler idle loop. 1000
headless frames run without a single SYSTEM_ERROR halt (v=4/v=11
faults that previously blocked progress are gone).

**P81 fix — Pascal static-link ABI for sibling-nested procedure calls.**
The handoff's "MMU mapping" hypothesis was incorrect. MemMgr's first
16 user-mode instructions at $04413C ran fine; the real bug was in
the Pascal code generator. When a proc nested inside a top-level proc
calls a *sibling* nested proc (e.g., MOVE_SEG → SET_INMOTION_SEG, both
nested in CLEAR_SPACE), `emit_frame_access` walked the *dynamic* link
(saved A6 from LINK) to reach outer-scope locals. But dynamic link =
caller's A6 ≠ static parent's A6 for sibling calls: SET_INMOTION_SEG's
MOVEA.L (A6),A0 returned MOVE_SEG's A6 instead of CLEAR_SPACE's,
then MOVE.L disp(A0),D0 at disp=-14 straddled MOVE_SEG's saved-A6 and
return-PC bytes, loading garbage ($DD980004) that got stuffed into A0
via MOVEA.L D0,A0 → JMP (A0) → vector 4 crash.

**Fix (src/toolchain/pascal_codegen.[ch]):**
- Each `cg_proc_sig_t` now carries `nest_depth` + `takes_static_link`.
- Procs at depth ≥ 2 reserve a static-link slot at `-4(A6)`; locals
  start below that. After `LINK A6,#-N` the prologue emits
  `MOVE.L A2,-4(A6)` to save the caller-provided static link.
- Each call site emits `emit_static_link_load(cg, sig)` right before
  the JSR. Walk count = caller_depth − callee_depth + 1:
    - 0 → `MOVEA.L A6,A2` (caller is direct static parent)
    - 1 → `MOVEA.L -4(A6),A2` (caller is sibling)
    - N → first hop then (N-1) `MOVEA.L -4(A2),A2` hops
- `emit_frame_access` walks the static chain via -4(A6) / -4(A0), not
  (A6) / (A0).
- A2 is used as the caller-saved static-link register (no prior use).
- Applied at three call sites: AST_IDENT_EXPR (zero-arg call),
  AST_FUNC_CALL, AST_CALL. Indirect calls via procedure params are
  not yet static-link-aware (uncommon enough to defer).

### P81a — trimmed 6 of 7 HLE bypasses the static-link fix made redundant

Commit `6b6d9b0`. With the static-link ABI in, natural kernel code
now runs cleanly through the paths these HLEs were substituting.
Removed:

- CHK_LDSN_FREE (P80e) — natural errnum resolution works now
- MAKE_SYSDATASEG (P79/P80f/g) — natural DS_OPEN / GetFree / MMU-program
- Move_MemMgr (P80d) — MM free-space bookkeeping now coherent
- Wait_sem (P43) — byte-subrange D0 fix (P80h2) had the real cause
- Signal_sem (P78) — corrupt wait_queue was a static-link downstream
- CreateProcess + ModifyProcess + FinishCreate (P80g/h2) — natural
  body populates PCB / syslocal / env_save_area correctly now

Boot: 50 boot_progress checkpoints reached in 200 frames, clean through
PR_CLEANUP into the scheduler idle loop. No SYSTEM_ERROR halts, no
vector 4 / vector 11 faults.

### P81b — parameterless nested procs now get sigs registered

Commit landed alongside the exit() investigation. Previously
`gen_proc_or_func` only called `register_proc_sig` when the proc had
an AST_PARAM_LIST child. That left every parameterless nested proc
(e.g. `SET_INMOTION_SEG`) without a sig entry, so callers couldn't
look it up and didn't emit `emit_static_link_load` — A2 stayed stale,
the callee's prologue saved that stale A2 to -4(A6), and the static
chain was poisoned.

Result: **26/27 milestones** (up from 25/27). INITSYS is now reached.

### P81c — all remaining process/memory/FS HLEs trimmed (one commit each)

Each removal verified individually with `make && ./build/lisaemu
--headless Lisa_Source 200` staying at 26/27 resolved milestones.
Commits `1cd425f` through `c8dc5d4`:

- **MM_Setup** (`1cd425f`) — syslocal setup; natural body works now.
- **excep_setup** (`ae641a9`) — wild b_sloc_ptr was a CreateProcess
  static-link symptom.
- **MEM_CLEANUP** (`2fa0f3c`) — SYS_PROC_INIT's args are properly
  initialized now.
- **REG_OPEN_LIST** (`743f01f`) — sentinel-init was static-link.
- **Make_File** (`be0d4bf`) — DecompPath/SplitPathname garbage-write
  path was static-link.
- **PR_CLEANUP** (`9505ada`) — natural body unlinks c_pcb_ptr and
  enters scheduler correctly.
- **HLE-SelectProcess** (`c8dc5d4`) — earlier removal attempts failed
  because other HLEs upstream were still poisoning state; natural
  body runs cleanly now.

### P81d — exit(CurrentProc) codegen fix (`f010d4f`)

`EXIT(init_boot_cds)` inside INIT_BOOT_CDS (nested depth 2) was
unwinding its caller BOOT_IO_INIT too, because our `exit()` codegen
walked `scope_depth-1` hops of the dynamic link unconditionally.
That hit `MOVEA.L (A6),A6` once, landing on BOOT_IO_INIT's frame,
then UNLK+RTS blew BOOT_IO_INIT away along with INIT_BOOT_CDS —
skipping BOOT_IO_INIT's for-loop and its `case 4:` dispatch to
FS_INIT.

Fix: when `exit(Name)` target matches the current scope's proc_name,
emit UNLK+RTS with **no walk**. For `exit(Named)` where the target
is an enclosing proc, keep the old walk-to-outermost behavior —
lexically not perfectly correct but changing it cascades regressions
via the name-collision / flat-find_proc_sig issues.

**Result: FS_INIT now reached** for the first time (27th resolved
milestone visible). But FS_INIT's natural body crashes downstream
(writes to `$F23204` from P_DEQUEUE with an unmapped segment), so
SYS_PROC_INIT etc. no longer fire — boot stops at 22/27.

### Still kept: FS_CLEANUP (was 26/27's holdout)

Unchanged: natural body still regresses boot, needs FS infrastructure
work (mounttable / catalog state) before it can run. Currently
subsumed by the bigger FS_INIT-crashes issue above — fixing FS_INIT
likely also unblocks FS_CLEANUP since both depend on the same FS
state machinery.

### Next

The big remaining kernel blocker: **FS_MASTER_INIT fails with
unmapped writes at `$F23204` from P_DEQUEUE** (part of the FS's
sysglobal queue manipulation). Almost certainly a memory-manager
bookkeeping mismatch — natural FS_INIT calls FS_MASTER_INIT which
walks/updates queue structures that our current state doesn't
populate with valid pointers. Needs diagnosis of P_DEQUEUE's caller
and the syslocal/sysglobal fields it reads.

After that, remaining work to a booting Lisa:

1. **FS state population** (MDDF / mounttable / catalog entries in
   the disk image, so FS_MASTER_INIT finds what it needs).

2. **SYSTEM.SHELL as second compile target** (multi-target build +
   intrinsic-library loader + disk-image catalog entries). Unlocks
   SHELL and WS_MAIN milestones and the actual Lisa desktop.

### P80h2 session fixes (scheduler dispatch plumbing)

- **Byte-subrange loads zero-extend** (`src/toolchain/pascal_codegen.c:1067`):
  `emit_read_a0_to_d0` now emits `MOVEQ #0,D0; MOVE.B (A0),D0` instead
  of bare `MOVE.B (A0),D0`. Without the zero-extend, D0's upper 24 bits
  retained whatever was there (often a PCB pointer). A subsequent
  `MOVE.W D0,D2` then `CMP.W D1(0),D0` looked at the polluted low word
  — for priority=250 with a candidate pointer $CCB880, `SGT` saw a
  negative 16-bit value and `candidate^.priority > 0` evaluated FALSE,
  causing SelectProcess to always return nil. This was the reason the
  scheduler dispatched nothing after Launch. Keep an eye on byte-load
  sites — this is a general codegen fix, not scoped to the scheduler.
- **Priority fields are 1-byte in PCB** (`src/m68k.c:3546`): CreateProcess
  HLE now writes `priority`/`norm_pri` as bytes at offsets 12/13, not
  16-bit words. Matches the compiler's layout for `0..255` subranges.
- **HLE-SelectProcess**: our Pascal codegen for `exit(SelectProcess)` emits
  a buggy `MOVEA.L (A6),A6; UNLK A6; RTS` sequence that walks the static
  link when it shouldn't — clobbers A7 and crashes. Tried fixing the
  codegen (NOP or walk-target-aware), both caused early-boot regressions
  the OS depended on. Chose instead to bypass the whole proc: at
  `$05B59A` the emulator picks the highest-priority PCB in the ready
  queue, writes it to Scheduler's `candidate` local at `-6(A6)`, sets
  `b_syslocal_ptr ← candidate's syslocal` (so Launch's SETREGS reads
  the right env_save_area), then RTSes. (`src/m68k.c:3069`)
- **PCB → syslocal tracking**: CreateProcess HLE now stores (pcb, sloc)
  pairs so SelectProcess can resolve which syslocal to point
  b_syslocal_ptr at for the dispatched process.


- **ord(@proc) emits proc-address relocation**: `AST_ADDR_OF` now detects
  procedure identifiers via `find_proc_sig` and emits `MOVE.L #imm32,D0`
  with a linker relocation, instead of falling through to
  `gen_lvalue_addr` which emitted a bogus `LEA offset(A5),A0`. MemMgr's
  start_PC now resolves to `$043FB4` (the real code entry) instead of
  `$CCB802` (a stale A5-relative global slot).
  (`src/toolchain/pascal_codegen.c:2262`)
- **Proc-sig registration for all decls**: parameterless Pascal bodies
  like `procedure MEMMGR;` are now registered in `proc_sigs`, not only
  external ones. Without this, `find_proc_sig` returned NULL for all
  body-decl procs and `@MEMMGR` fell back to variable-lookup.
  (`src/toolchain/pascal_codegen.c:3320`)
- **CreateProcess HLE populates env_save_area**: writes PC=start_PC,
  SR=0, A5=sysA5, A6=A7=stack top, plus SCB fields and
  sl_free_pool_addr, so the scheduler's SETREGS/RTE launches into a
  runnable register state.  (`src/m68k.c:3515`)
- **FinishCreate HLE does priority-sorted queue insert**: doubly-linked
  walk from `@fwd_ReadyQ` finds the insertion point, maintains both
  next/prev pointers.  (`src/m68k.c:3632`)
- **PR_CLEANUP HLE unlinks stale PCBs and redirects to Scheduler**:
  walks the ready queue, unlinks any PCB with priority&lt;0 or ≥255
  (covers STARTUP's pseudo c_pcb whose priority got garbled), then jumps
  directly to the Scheduler body at `$05B832`.  (`src/m68k.c:2978`)

### P80 session fixes (20+ structural codegen + HLE fixes)

**Structural codegen:**
1. **8-char significant identifiers** (P80): Lisa Pascal truncation rule
2. **Iterative pre-pass record fixup** (P80b): 27 records corrected
3. **Imported type preservation** (P80c): prevents full-pass offset corruption
4. **Non-local goto A6 restore** (P80e): follows static link chain
5. **Non-local exit() A6 restore** (P80g): same fix for exit(proc) calls
6. **Boolean NOT for function calls** (P80e): TST/SEQ instead of NOT.W
7. **Enum/const priority** (P80f): enum ordinals don't overwrite CONSTs
8. **Generalized record repair** (P80f): auto-detect/replace corrupt records
9. **Anonymous record repair** (P80f): match by first field name
10. **find_type imported preference** (P80g): prefer imported records with valid offsets
11. **Post-creation record repair** (P80g): copy offsets from imported at resolve_type

**HLE mechanisms:**
12. **MAKE_SYSDATASEG bypass** (P80f/g): all segment creation as resident
13. **CreateProcess/ModifyProcess/FinishCreate bypass** (P80g)
14. **CHK_LDSN_FREE bypass** (P80e): system LDSNs allowed
15. **Move_MemMgr bypass** (P80d)
16. **INIT_FREEPOOL pool repair** (P80c)
17. **SYS_PROC_INIT crash unwind** (P80d)

### P79 session fixes (6 structural codegen improvements)

1. **Record layouts** (P79): string word-padding, CONST pre-pass export
2. **Push direction** (P79c): prefer sig's is_external
3. **Proc sig pre-pass** (P79d): export during types pre-pass
4. **Enum constants** (P79e): register ordinal values
5. **Byte-subrange sizing** (P79f): range<=255 → size=1
6. **Record-field array stride** (P79f): resolve element type for field arrays

### Next: fix process environment for dispatch

Scheduler dispatches but processes crash immediately. Two issues:
1. **ord(@proc) codegen**: `ord(@MemMgr)` generates $CCB802 (global var
   offset) instead of $043F56 (code address). Need to fix address-of
   for procedure identifiers.
2. **Environment save area**: CreateProcess HLE needs to set up the
   syslocal's env_save_area with correct A5, PC, A6, A7, SR values
   so Launch can restore registers and jump to the entry point.

### Roadmap to fully bootable Lisa desktop

**Current: Kernel 90% complete, full desktop ~25-30%**

| Layer | Status | What's needed |
|-------|--------|---------------|
| OS Kernel (SYSTEM.OS) | 90% — 27/27 checkpoints | Fix SYS_PROC_INIT body |
| System Libraries (SYS1LIB, SYS2LIB) | 0% | New compile targets + linking |
| Graphics (LIBQD, LIBTK) | 0% | New compile targets |
| Drivers (SYSTEM.LLD, CD_*) | 0% | 13 new binaries |
| Shell (APDM = Desktop Manager) | 0% | Separate compile target |
| Apps (LisaWrite, LisaCalc, etc.) | 0% | 14 app targets |
| Intrinsic library loading | 0% | Dynamic linking support |
| Lisa filesystem (MDDF, catalog) | 0% | Disk image infrastructure |
| Boot ROM / bootloaders | Partial | BT_Profile, BT_Sony |

Full source code is available for ALL components. The toolchain (parser 100%, assembler 100%, linker working) can handle additional targets — the remaining work is systematic: define 26+ compile targets, fix per-target codegen issues, build the disk layout.

### Key structural codegen fixes (cumulative, P4–P78)

These are durable improvements to the Pascal cross-compiler, not one-off patches. All are in git history. Major classes:

- **Field layout**: subrange default word-size in unpacked records, variant records, PACKED propagation, PASCALDEFS pin table (29 A5-relative globals)
- **Pointer arithmetic**: EXT.L skip for wide operands (`rhs_has_wide_operand()`), narrow→wide sign-extension on stores
- **Type resolution**: two-pass compile (types pre-pass), cross-unit type propagation, proc-local TYPE/CONST registration
- **Control flow**: goto/numeric-label support, case multi-labels + selector preservation
- **String ops**: byte-compare for string equality
- **WITH blocks**: nested WITH field bases, double-deref, address-of fields, true/false/nil inside WITH
- **Calling conventions**: non-primitive value params as by-ref, prefer Pascal body sig over external, proc-sig type remap by name

### Active HLE bypasses

- **P33** REG_OPEN_LIST: mounttable chain walk
- **P34** excep_setup: wild `b_sloc_ptr`
- **P35** SYS_PROC_INIT: full system-process creation
- **P36** MEM_CLEANUP: fires milestone, bypasses body
- **P37** FS_CLEANUP: fires milestone, bypasses body
- **P38** PR_CLEANUP: fires milestone, bypasses body
- **P42** Dynamic HLE lookup via `boot_progress_lookup` (cached per `g_emu_generation`)
- **P78** Signal_sem HLE guard + RELSPACE guard

### Key HLE mechanisms

- `$234` fetch bypass: IPL=7→RTE (DB_INIT skip), IPL=0→execute (Lisabug)
- `$4FBC` NOP-skip: illegal opcode in Workshop init loop
- ProFile HLE: intercepts CALLDRIVER, reads from disk image
- INTSON/INTSOFF: manages IPL for compiled OS code
- Loader TRAP HLE: MMU-translated reads/writes for fake_parms
- ENTER_LOADER HLE: mode-switch bypass (supervisor→user A7 swap issue)
- Setup_IUInfo HLE: skips INTRINSIC.LIB read loop
- GETSPACE: zero-fills allocated blocks (calloc semantics)
- Unmapped segment writes dropped (P27 generic MMU safety net)

### Debug infrastructure

- `DBGSTATIC` macro + `g_emu_generation`: all debug statics reset on power cycle
- Bounded print budgets on all diagnostic output
- `VEC-FIRST` per-vector first-fire trace, periodic DIAG frame dumps
- 256-entry PC ring for crash analysis
- `boot_progress_lookup(name)` public accessor for linker symbol table

### Inspiration projects

- `_inspiration/LisaSourceCompilation-main/`: 2025 working compilation of LOS 3.0 on real Lisa hardware. `scripts/patch_files.py` catalogs source patches.
- `_inspiration/lisaem-master/`: Reference for SCC/VIA/COPS/ProFile/floppy emulation.

## Lisa_Source Reference

See `docs/LISA_SOURCE_MAP.md` for the complete catalog (~1,280 files).
See `docs/HARDWARE_SPECS.md` for hardware specifications derived from source.
See `docs/TOOLCHAIN.md` for the compilation pipeline needed to build from source.

Key facts:
- **Version**: Lisa OS 3.1 (Office System), circa 1983-1984
- **Languages**: Motorola 68000 assembly + Lisa Pascal (Apple's custom Pascal dialect)
- **~1,280 files** across OS kernel, 21 libraries, 13 applications, fonts, toolkit
- Contains 8 pre-compiled .OBJ files (68000 binaries) and 57 binary font files
- Build scripts in `LISA_OS/BUILD/` and `LISA_OS/OS exec files/` describe the full build process
- Linkmaps in `LISA_OS/Linkmaps 3.0/` show exact segment layout of every linked binary
- No pre-built ROM images or bootable disk images — everything must be compiled from source

## Hardware Specs (from source analysis)

| Component | Specification |
|-----------|---------------|
| CPU | Motorola 68000, 5 MHz |
| RAM | 1 MB (24-bit address bus) |
| Display | 720 x 364, monochrome bitmap |
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
- **Emulator/ files are SYMLINKS**: `lisaOS/lisaOS/Emulator/` contains symlinks to `src/`. No copying needed — edit `src/` and Xcode picks it up automatically.

## Git Conventions

- No Claude attribution in commit messages
- Lisa_Source/ is gitignored (Apple license prohibits redistribution)
- .claude/ directory is gitignored
