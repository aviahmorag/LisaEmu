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

## Current Status

**Prebuilt-image boot — real Lisa OS runs, but lands in Lisabug.**
Running the Xcode macOS app with `prebuilt/los_compilation_base.image`
executes real Lisa OS through MMU init (215 DO_AN_MMU calls), COPS
handshake, ProFile driver init, framebuffer paints, and into the
driver polling loop. On the native Xcode build it then lands in
Lisabug with a "Level 7 Interrupt" banner at PC=$1027FC. **That
banner is NOT a real CPU exception in our emulator** — it is Lisa
OS / Workshop Lisabug code drawing to the framebuffer along the
normal execution path. Proof, via `src/m68k.c:take_exception`
per-vector first-fire instrumentation added this session: across a
1500-frame native Xcode run, only **two** vectors ever fire, both
expected — v38 (TRAP #6, MMU accessor, at boot) and v37 (TRAP #5,
HW-interface dispatcher, COPS polling). `VEC-HIST` at frame 1500
shows `v37×135932` and nothing else. No bus error, no address
error, no illegal inst, no line-1010/1111, no spurious interrupt.
The sandbox SDL run produces a byte-identical CPU trace, so
**there is no native-vs-sandbox CPU-level divergence** — the
difference is screen-only. The banner's own SR=$2010 has IPL=0,
confirming "Level 7" is just Lisabug's hard-coded header text, not
an actual level-7 interrupt.

The previously-suspected `SP=$414` transient at PC=$20820C is also
**benign**: the sequence is `LEA ($0450).W, A7` (load — not push;
old SP discarded) followed by `MOVEM.L D0-D7/A0-A6, -(A7)` at
$208210 which pushes 15 longwords = 60 bytes = $3C, landing SP at
$450 - $3C = $414, **above** the vector table (ends at $3FF). This
is normal **process-creation prologue** for Lisa OS process #4
(matches `P#=00004` in the banner). No vector is corrupted.

After the dump is drawn the CPU continues to `PC=$52045C` (COPS
polling loop) and sits there cleanly for as long as the app runs —
verified to frame 1500 in Xcode. Typing `G` at the Lisabug `>`
prompt eventually lands on a `*** SYSTEM ERROR 10738 ***`
(`stup_find_boot`, `SOURCE-CD.TEXT:70`) but that is a downstream
consequence of Lisabug's Go-path, not the initial "crash".

**Source of the banner**: Correlating the CPU register state at
`SP DELTA: $000FD914 → $00000450 at PC=$20820C op=$4FF8` with the
on-screen register dump proves Lisabug is drawn by the code at
`$208xxx`. Every register in the displayed dump
(`D0=00522014 A0=001027F2 A5=000FFEE0 A6=000FD9FC`) matches the
CPU state at that moment exactly. The code at `$208xxx` is reached
via `$520464 → $208752 → $2081F4..$20820C` — i.e., from the
TRAP #5 HW-interface dispatcher with a specific selector, NOT via
`jmp enter_macsbug` ($234). Lisabug has been loaded into an MMU
segment (LOADSEG from SYSTEM.DEBUG in `source-LOADER.TEXT:869`)
and its code mapped into virtual `$208xxx`, entered via a TRAP #5
selector, not via the low-memory `$234` trampoline.

The entry trigger is `SOURCE-STARTUP.TEXT:302` — `DB_INIT` procedure
has `MACSBUG;` gated by `if debugmode then`, the intentional
Workshop developer-entry breakpoint ("Mend your (debugging) fences
now."). After the banner draws, Lisa OS is idle in Lisabug's
keyboard-and-mouse-poll loop (`TRAP5-SEL: d7=148×N d7=158×N`
alternating) waiting for the user to type `G`.

**Existing bypass infrastructure (both gated, both harmless, both
currently ineffective on this image)**:

- `src/lisa_mmu.c:lisa_mem_write16` intercepts writes of `$4EF9`
  (JMP abs.L opcode) to `$234` and replaces with `$4E73` (RTE —
  correct per `NMIHANDLER:311 lisabugentry` comment "emulate a
  level 7 interrupt to get there", which pushes SR+PC as a
  synthetic exception frame; RTE pops both, RTS was wrong).
  Commit `cae21c9`. Does NOT fire in practice on this image —
  Lisa OS never writes `$4EF9` to `$234` on this boot path.
- `src/m68k.c:m68k_execute` belt-and-braces fetch-time intercept:
  when `PC == $234`, call `op_rte(cpu)` directly and skip decode.
  Also does NOT fire — CPU never reaches `$234` at all in either
  sandbox or native, confirming the MACSBUG call path does not go
  through the low-memory trampoline on this image.

**Latest state (2026-04-11 evening, cae21c9 bypass active):**
The `$234` RTE fetch intercept is now firing for real. A run with
the fix applied advances dramatically further than prior runs:

- `v=39` TRAP #7 fires at `PC=$221004` (first ever) — this is
  the `macsbug` userstate path (`trap #7` to get onto supervisor
  stack). Normal behavior, previously we never reached this.
- `INIT_NMI_TRAPV` now runs — the `$234` write intercept fires
  exactly once (`[HLE] $234 JMP→RTE intercept installed`), which
  previously never happened in any run.
- Fetch-time bypass at `PC=$234` fires **three times** during
  boot, each time popping a clean synthetic-level-7 exception
  frame via `op_rte` and resuming. Bypass works structurally.
- MMU segment programming extends to 262+ TRAP #6 calls
  (previous max ~215), proving Lisa OS is running init code we
  never reached before.
- **First real emulator divergence**: `ILLEGAL op=$4FBC` at
  `PC=$302790`. `$4FBC` is `LEA #imm, A7` which is not a valid
  68000 opcode. Something set PC to there expecting code.
  Handled by Lisa OS's `hard_excep` Pascal path — on-screen banner
  reads "ILLEGAL INSTRUCTION in system code ! sr = 4 pc = 3155858"
  which is decimal for `$302452` (saved-pc offset from the
  `$302792` in our trace).
- **Cascade after the illegal**: exception manager at `PC=$2E007C`
  triggers A7-nuke to `$0`, PC falls through the vector table,
  `v=10` (Line-A) fires at `PC=$17`, then `v=5` (ZERO DIVIDE) at
  `PC=$A2002E72 SR=$2014` — which matches the second on-screen
  dump "ZERO DIVIDE in system code ! sr = 8212 pc = -1577046414"
  exactly (8212 = `$2014`; -1577046414 signed = `$A2002E72`).
- CPU settles into a tight loop in `$460100-$460166` for
  thousands of frames, firing only v=37 occasionally. That's
  Lisa OS in exception-recovery idle after the cascade.

See `.claude-tmp/post_g_summary.md` for the event-order cheat
sheet.

**Updated understanding (2026-04-11 late evening): `$302790` is NOT
a bug — the real blocker is the `$234` bypass being too aggressive.**

The on-screen banner on the latest Xcode run is the smoking gun:
```
*** SYSTEM ERROR   10738 ***
ILLEGAL INSTRUCTION in system code !
sr =       4  pc =  3155856         ← decimal, = $302790 exactly
 saved registers at 13369278
Going to Lisabug, use OSQUIT.
```

Lisa OS's Pascal `hard_excep` / `showregs` receives `pc = $302790`
EXACTLY (proving the new `cpu->pc -= 2` rewind in
`src/m68k.c:illegal_instruction` is correct — before the fix the
banner showed `3155858` = `$302792`, off by 2). Then it displays
"ILLEGAL INSTRUCTION in system code, Going to Lisabug, use OSQUIT."
**This is Lisa OS's NORMAL reaction to a system-code illegal
instruction** — not a crash. Real hardware would show the same
banner and wait for the user to type `OSQUIT` at the Lisabug prompt
to return to the OS.

The "crash" / A7-nuke cascade we observe AFTER the banner is caused
by our `$234` JMP→RTE bypass (`cae21c9`). That bypass was added for
the **DB_INIT developer-breakpoint** path (Workshop image calling
`MACSBUG` from `SOURCE-STARTUP.TEXT:302` boot init). It synthesizes
an RTE to skip the debugger entry. **But Lisa OS also jumps to
`$234` from the `hard_excep` path** — when a real system-code
exception wants to drop the user into Lisabug. Our bypass fires for
BOTH cases. For DB_INIT it works (the stack frame is the synthetic
level-7 frame the bypass expects). For `hard_excep` it does NOT
work (the stack frame is an actual Lisa OS exception frame that
Lisabug was supposed to parse, display registers, and wait for
`OSQUIT`). The RTE pops junk, A7 ends up at 0, CPU falls into the
vector table, cascades to illegal at PC=0.

**So the remaining blocker is: gate the `$234` bypass so it only
fires on the DB_INIT boot path, not on exception-driven Lisabug
entries.** Options:
- Match on the level-7 synthetic-frame SR the DB_INIT path uses
  (DB_INIT pushes SR with specific IPL=7), vs `hard_excep`'s SR
  (which is the user process's SR at trap time)
- Check PC-before-234 to see if we came from NMIHANDLER (DB_INIT)
  vs EXCEPASM's `go_to_macsbug` path
- Or drop the bypass and instead pre-queue an auto-`OSQUIT`
  response via COPS when the $234 entry is detected

**The `$302790` illegal itself is legitimate — the bytes are on
disk ($4FBC $000C confirmed in `prebuilt/los_compilation_base.image`
at offset 0x69DB4E), and Lisa OS's handler correctly dispatches it.
Our emulator just needs to handle the debugger-entry side properly.**

### PC-ring forensic findings (all captured by V4-TARGET dump)

- **Caller A** at `$302726` (`LINK A6, #-8`) — initializes a data
  table. Not in our ring, but visible in the code window dump.
- **Function B** at `$3025B6` (`LINK A6, #0`) — called from caller A
  at `$302786` (`JSR $3025B6` via PC-relative `$4EBA $FE2E`). B is
  a big sequence of 16 PUSH-params + JSR calls to function C.
- **Function C** at `$302570` (`LINK A6` / `MOVEM.L A3-A4`) — takes
  5 byte/word parameters + A6, writes entries into an array indexed
  by A4+offset. Returns via `MOVEA.L (A7)+,A0 / ADDA.W #$C,A7 /
  JMP (A0)` (manual param-cleanup return). 12 bytes of params.
- **For-loop at `$30278A..$3027A4`**: `CLR.B D7 / BRA test /
  EXT.W D7 / $4FBC $000C / MOVE.L D7,D0 / ASL.W #1,D0 /
  MOVE.W #1,$70(A4) / ADDQ.B #1,D7 / CMPI.B #$0C,D7 / BLE.S loop`.
  Runs D7 from 0..12. The body contains `$4FBC $000C` which is
  either (a) Pascal `INLINE(...)` output, (b) a custom opcode handled
  via the illegal-instruction vector by Apple's Lisa OS, or (c) the
  compiler emitted something the linker was supposed to patch and
  didn't. Regardless of which, Lisa OS's `hard_excep` handler IS
  wired to receive it and behaves correctly.
- **The `$4FBC $000C` byte pattern exists exactly once in the 50MB
  disk image** at offset `0x69DB4E`, with matching surrounding bytes
  — Apple's release really contains those bytes.

### Old/superseded hypotheses (for context, now disproved)

1. **`PC=$302790 op=$4FBC`** (ORIGINAL HYPOTHESIS, now refined) — the
   forensic dump captured this session via `[V4-TARGET]`
   instrumentation in `src/m68k.c` main dispatch loop (fires
   pre-dispatch when `PC==$302790`). PC ring proved it was a
   **sequential fall-through**, not a miscomputed jump or corrupted
   RTS target:
   ```
   [55] $302720  UNLK A6            ─┐ caller function
   [56] $302722  MOVE.L (A7)+, SP   │ epilogue (pop params
   [57] $302724  RTS                ─┘  then RTS)
   [58] $30278A  CLR.B D7              ← clean return here
   [59] $30278C  BRA $3027A0           skip to test
   [60] $3027A0  CMPI.B #imm, D7       test
   [61] $3027A4  BLE.S $30278E         loop back
   [62] $30278E  EXT.W D7              loop body start
   [63] $302790  $4FBC  ← ILLEGAL
   ```
   Textbook Pascal `for D7 := 0 to N do` codegen with a valid
   structure. SSP=$00F7EB24, USP=$00F7EADA — plenty of headroom,
   no stack corruption. Ruled out: (a) miscomputed JSR target,
   (b) RTS to garbage. Remaining hypothesis: **MMU returns wrong
   bytes for virtual `$302790`**, or physical RAM at the mapped
   address was corrupted by an earlier errant write. `$4FBC` is
   genuinely illegal on 68000 (not LEA, CHK, MOVEM, or any 68020
   extension — verified against all group-4 decodings). Loop body
   from `$302790` onward contains `$4FBC $000C $2007 $E340 $39BC
   $0001 $0070 $5207` — likely corrupt, not just one bad word.
   **Next instrumentation** (in place): `lisa_dump_mmu_for_vaddr()`
   in `src/lisa.c` walks all 5 MMU contexts for segment 24, prints
   SLR/SOR/changed, physical base, and raw physical bytes
   alongside virtual-read bytes. Called from the `[V4-TARGET]`
   block in `src/m68k.c`. Rerun needed to collect the mapping.
2. **`PC=$2E007C ir=$2C56`** — the A7 nuke is in Lisa OS's
   exception-handler reentry path. Probably a secondary issue
   caused by (1), but could be its own bug.

These are **emulator-core bugs**, the same kind the source-compile
track will surface. Fixing them helps both tracks equally.

Fixes landed this session (all in one commit + one pending):

- `src/m68k.c` group-0 dynamic bit-op dispatch accepts `type=3`
  (BSET Dn,<ea>). The old guard `(op & 0x00C0) != 0x00C0` was
  rejecting every `BSET` with bits 7:6 == 11 and falling through
  to `illegal_instruction`. Classic Level-7 trigger on the COPS
  polling path.
- `src/lisa.c:lisa_run_frame` now divides accumulated CPU cycles by
  10 before passing to `via_tick`. VIA 6522 is clocked at Φ2 =
  CPU/10 (~500 kHz on 5 MHz Lisa 2); previously everything VIA-timed
  (jiffy counter, key repeat, vretrace) was running 10× too fast.
- `src/lisa.c:lisa_key_down/up` COPS bit sense was inverted. Per
  `libhw-DRIVERS.TEXT` COPS0, `$80|keycode` is DOWN, `keycode&$7F`
  is UP. We had it backwards, which made tapping any key produce
  a phantom auto-repeat that never released.
- `src/lisa.c:lisa_mouse_move` now prepends a `$00` header before
  Dx,Dy so the COPS0 handler takes the "mouse data follows" branch
  instead of interpreting Dx as a random keycode.
- `lisaOS/lisaOS/EmulatorViewModel.swift:mapKeyCode` and
  `src/main_sdl.c:sdl_to_lisa_key` now use authoritative Lisa
  keycodes from `Lisa_Source/LISA_OS/LIBS/LIBHW/LIBHW-LEGENDS.TEXT`
  (`_FinalUS` Primary section). Previous values were a hand-rolled
  guess that bore no relation to Lisa's physical keycode table.
- `lisaOS/lisaOS/LisaDisplayView.swift:keyDown` drops macOS
  auto-repeats via `event.isARepeat`. Lisa OS runs its own repeat
  timer from `libhw-KEYBD RepeatTable`; forwarding OS-level repeats
  was compounding them.
- `src/m68k.c:take_exception` has a per-vector first-fire trace
  (`static bool ff_seen[256]`) that logs `[VEC-FIRST]` once per
  unique vector with PC/SR/SSP/USP/handler. Used to disprove the
  "Level 7 = real exception" theory.
- `src/m68k.c` main dispatch loop has an "A7 INTO VECTORS" guard
  that tracks `a[7]` across instructions and logs when SP drops
  below `$1000`, identifying the instruction that moved it. Used
  to prove the `SP=$414` event is a benign `LEA abs.W,A7` +
  `MOVEM.L -(A7)` process-creation prologue, not stack corruption.
- `src/main_sdl.c` has `--headless [frames]` mode that skips SDL
  init and runs the emulator loop printing CPU state every 50
  frames. Used for fast sandbox baseline runs vs native Xcode.
  **Known limitation**: headless boot of `los_compilation_base.image`
  reaches Lisabug's transient register-dump routine at `$20820C`
  once via TRAP5 very early, then settles into the COPS polling
  loop (`$520xxx`) indefinitely without triggering DB_INIT's
  MACSBUG. v=39 TRAP #7 and the `$234` JMP→RTE intercept never
  fire in headless, so the `$302790` divergence is only reachable
  via the native Xcode app (user-driven G+Return at the Lisabug
  prompt). Sandbox-vs-native timing divergence is a separate
  bug, not pursued here.
- `src/m68k.c` main dispatch loop has a one-shot `[V4-TARGET]`
  forensic dump that fires pre-dispatch when `PC==$302790`.
  Prints all D/A registers, SR, SSP, USP, illegal-vector handler,
  full **256-entry** PC ring with opcodes, wide code window
  `$302500..$302800` via `cpu_read16` (shows function C, function
  B, and caller A in full), 32-long stack dump, and (via
  `lisa_dump_mmu_for_vaddr` in `src/lisa.c`) the MMU segment
  mapping across all 5 contexts plus raw physical RAM bytes vs
  virtual-read bytes. One-shot, gated behind `static int
  v4_target_dumped`. **Decisive in proving `$4FBC` bytes are
  legitimate Apple-release code and the `$234` bypass is the
  real blocker.**
- `src/m68k.c:illegal_instruction` now rewinds `cpu->pc -= 2`
  before calling `take_exception`, so the stacked PC on
  illegal-instruction exceptions is the address of the faulting
  instruction itself (per M68000 PRM), not PC+2. Lisa OS's
  `hard_excep` Pascal handler reads the stacked PC to show in the
  "ILLEGAL INSTRUCTION in system code!" banner, and the banner
  now shows `pc = 3155856` = `$302790` exactly (before the fix it
  showed `3155858` = `$302792`, off by 2). Line-A/Line-F handlers
  already did this rewind; illegal didn't.
- `lisaOS/lisaOS/LisaDisplayView.swift:LisaDisplayNSView` now
  overrides `viewDidMoveToWindow()` to call
  `window?.makeFirstResponder(self)` on the main queue after
  mount, so the emulator view grabs keyboard focus automatically
  when the SwiftUI overlay mounts it after "Power On". No more
  having to click into the view before typing.

**Toolchain (source → image pipeline)** — green but does NOT yet boot
end-to-end. **This is the real product track** — the prebuilt-image
work above is a validation fixture for the emulator core.

- Parser: **100%** (317/317 Pascal files)
- Assembler: **100%** (103/103)
- Linker: **Link OK: YES**, 8527/8527 symbols resolved, 2.2 MB output,
  97.2% of JSR abs.L targets point at real code
- `build/lisaemu Lisa_Source` runs the full pipeline: compiles from
  `Lisa_Source/`, writes `build/lisa_profile.image` (5.1 MB, 58 files,
  9728 blocks) + `build/lisa_boot.rom`, plus a raw
  `build/lisa_linked.bin` (870 KB) for offline disassembly, then starts
  executing the compiled 68000 code. Early-boot TRAPs 37/39 take the
  real handlers; TRAP #6 (MMU accessor) currently hits an RTE stub at
  `$3F8`. CPU then spins in `libfp-FPMODES` around `PC=$097A**`.

**Blocker**: Pascal codegen bug. The spin pattern
`MOVE.W 8(A6),D0 / MOVEA.L D0,A0 / MOVE.W (A0),D0` strongly suggests
VAR parameters are being loaded as 16-bit pointers (truncating the
high half of the address) instead of the correct 32-bit load. Fix
site is in `src/toolchain/pascal_codegen.c` — VAR param dereference
emission. Likely more codegen bugs follow this one.

**Why this track matters more**: our toolchain does not link
SYSTEM.DEBUG, so source-compiled boots have **no Lisabug in the way
at all**. Boot goes straight through kernel init → STARTUP → APEW →
Desktop (APDM) without the Workshop developer breakpoint that
dominates the prebuilt-image path. Every codegen bug fixed here
moves the shipping product forward; Lisabug bypass work on the
prebuilt image gets thrown away.

**Emulator core** — verified end-to-end against the prebuilt fixture:
CPU, MMU, VIA1/VIA2, COPS, keyboard, video, interrupts, exception
dispatch, TRAP #5 HW-interface dispatcher, Lisabug debugger shell all
work. First macOS-native interactive boot with live keyboard as of
this session.

See `NEXT_SESSION.md` (gitignored) for the live handoff and
`.claude-handoffs/` for per-session archives.

See `.claude-handoffs/` for per-session handoffs. Run `make audit` for
toolchain metrics.

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
