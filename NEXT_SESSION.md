# LisaEmu — Next Session Plan (April 5, 2026)

## Quick Start

```bash
make                        # Build standalone emulator
build/lisaemu Lisa_Source   # Run from source directory
make audit                  # Full toolchain report
```

## Current State: OS executes INITSYS, makes TRAP #1 syscalls

The CPU boot path:
```
ROM ($FE0400) → set SP/A5/SR → JMP $400 → STARTUP → PASCALINIT
→ copy runtime data to sysglobal → set A5 → return param ptr
→ INITSYS(ldmapbase) → INTSOFF (TRAP #7 works) → GETLDMAP
→ ... deeper init → TRAP #1 syscalls → recursive error loop
```

### Key progress this session:
1. **TRAP vectors work** — 17 exception vectors installed by linker
2. **VAR parameter passing** — procedure signatures track VAR/size/external
3. **Callee-clean push order** — external assembly routines get correct convention
4. **CONST resolution** — emit immediate values, not memory loads
5. **Zero-arg function calls** — PASCALINIT correctly called as function-as-value
6. **Stack return convention** — external functions return results on stack
7. **Loader parameter block** — memory map at $A00, downward Pascal layout
8. **Boot ROM sets A5** — required by PASCALINIT
9. **SDL frontend** — `build/lisaemu Lisa_Source` builds from source and boots

### Runtime logs from last run:
```
TRAP #1 at PC=$04F2D0: vector[$84]=$0005064E (syscall happening!)
Exception counts: v11=1 (single Line-F)
DIAG frame 120: PC=$005A88 SR=$2118
SP=$FFFFFFF6 (stack overflow — recursive TRAP #1 error loop)
```

## Immediate Task: Fix TRAP #1 Error Loop

The OS reaches TRAP #1 (system calls) but enters an infinite recursive loop:
- TRAP #1 handler at $5064E is called
- Handler itself triggers TRAP #1 again (likely SYSTEM_ERROR)
- SP wraps around: $FFFFFFF6 → $FFFFFFE0 → ...

This is likely a legitimate OS error:
- A loader parameter may be wrong (memory regions, sizes)
- GETLDMAP may copy incorrect values → bad pointer dereference
- The OS may try to call an unimplemented function

### What to investigate:
1. What code triggers the first TRAP #1? Add breakpoint at $4F2D0.
2. What's the TRAP #1 handler doing? Why does it recurse?
3. Are the loader params at $A00 correct? Check GETLDMAP's copies.
4. Is the supervisor stack (b_superstack) set up correctly?
5. What's at PC=$5A88 where the DIAG frame shows?

## Key Files

- `src/toolchain/pascal_codegen.c` — CONST resolution, proc sigs, function-as-value
- `src/toolchain/pascal_codegen.h` — `cg_proc_sig_t` with is_const, is_external
- `src/toolchain/linker.c` — Vector table installation, debug symbols
- `src/toolchain/bootrom.c` — Boot ROM with A5 init
- `src/lisa.c` — Loader parameter block, system.os loader
- `src/m68k.c` — TRAP tracing, kernel escape detection, PC ring buffer
- `src/main_sdl.c` — Toolchain-aware SDL frontend

## Toolchain Metrics

```
Parser:    405 Pascal, 360 OK (99.5%)
Assembler: 105 files, 100% success
Linker:    97.2% JSR to real code, ~83 stub symbols
Output:    ~331KB system.os, 17 exception vectors
```
