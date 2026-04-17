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

## Current Status (2026-04-17) — STATIC-LINK ABI FIXED, BOOT CLEAN TO IDLE

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

### Still kept: HLE-SelectProcess

The natural body does `exit(SelectProcess)` (a named exit to the
current proc). Our `exit()` codegen walks `MOVEA.L (A6),A6`
`scope_depth-1` times regardless of target — one level too far when
target == current proc, corrupting A6 before UNLK.

Naive fix attempted: skip the walk when target matches current scope's
proc_name. Regressed 8 checkpoints: something else relies on the old
"always unwind to outermost" behavior. The correct behavior is
`walk = current_depth − target_depth`, but multiple callers seem to
rely on the buggy behavior (probably calling `exit(SomeEnclosingProc)`
expecting it to unwind further than lexically indicated). Needs deeper
analysis before retrying.

### Next

Two threads:

1. **Fix `exit()` codegen properly.** Audit all `exit(Name)` call sites
   in the linked OS, determine actual Pascal semantics the code relies
   on, then land a codegen that matches. Once done, HLE-SelectProcess
   comes off and the kernel is 100% self-hosting.

2. **SYSTEM.SHELL as second compile target.** Adds a second binary to
   the disk image, with an intrinsic-library loader. Unlocks SHELL and
   WS_MAIN milestones and the actual Lisa desktop. Larger scope —
   toolchain_bridge needs multi-target pipeline, linker needs separate
   output files, disk image layout needs MDDF entries for both.

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
