# LisaEmu — Development Plan

## Goal

A native macOS app that takes Apple's Lisa OS 3.1 source code as its sole input, compiles everything from source, and boots a fully working Apple Lisa emulator. No external ROM dumps or disk images needed.

## Phase 1: Toolchain Correctness (CURRENT)

Run `make audit` to see live metrics. Current status:

| Stage | Status | Key Metric |
|-------|--------|------------|
| Parser | **Done** | 100% of real source files parse clean |
| Codegen | **93.0%** symbols resolved | 767 unresolved, improving |
| Assembler | **~99%** real files | 1 actual failure (LIBWM-ASM) |
| Linker | **92.5%** JSRs to real code | 2,166 to stub (6.5%), down from 2,702 |

### Remaining work (priority order):

1. **FP wrapper path resolution** — `{$I}` includes for LIBFP implementation files need deeper path mapping. ~400 FP refs.

2. **Clascal vtable dispatch** — Polymorphic method calls need method dispatch table lookup instead of direct JSR. ~500 refs.

3. **`%` identifier export** — Runtime functions with `%` prefix (%_InObCN, %_HALT, %f_ADD) need to flow through codegen→linker. ~100 refs.

4. **WRITELN/READLN mangling** — Emit JSR to %W_LN/%_WriteLn instead of stub. ~354 refs.

5. **Skip LIBHW include fragments** — 7 files assembled via DRIVERS master need exclusion.

### Acceptance criteria:
- `make audit` shows >98% JSR to real code
- All assembly files pass (minus documented skips)

## Phase 2: Runtime Correctness

Once the toolchain produces a clean binary:

1. **Boot sequence** — ROM → $400 (STARTUP) → PASCALINIT → INITSYS → ENTER_SCHEDULER
2. **TRAP vector installation** — INIT_TRAPV must write correct handler addresses
3. **Interrupt system** — VIA timers, STOP instruction, scheduler heartbeat
4. **Memory management** — HAllocate, heap initialization
5. **Display** — 720×364 monochrome framebuffer at $7A000

## Phase 3: Disk Image & App Integration

- Build disk image builder (ProFile/Sony format, Lisa filesystem)
- Wire toolchain into Xcode app UI ("Build from Source" workflow)
- Cache compiled output for instant subsequent launches

## Phase 4: Full System

- Boot to Lisa desktop
- Mouse and keyboard input
- Application launch (Filer, LisaCalc, LisaDraw, LisaWrite)
