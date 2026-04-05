# LisaEmu — Next Session Plan (April 5, 2026)

## Quick Start

```bash
make                    # Build standalone emulator
build/lisaemu Lisa_Source   # Run from source directory
make audit              # Full toolchain report (all 4 stages)
```

## Current State: OS boots past INTSOFF, executes deeper into INITSYS

The CPU executes this path successfully:
```
ROM ($FE0400) → JMP $400 → STARTUP → BRA $4CE0 → INITSYS ($4C00)
→ LINK A6 → load globals → call INTSOFF ($4D7A4) → TRAP #7 → TRAP7 handler
→ returns to INITSYS → continues execution...
→ eventually hits illegal instruction in vector table area ($6D)
```

### Fixes applied this session:
1. **Vector table installed by linker** — 17 exception vectors pre-installed in output binary $0-$FF
2. **VAR parameter passing** — procedure signatures track VAR params, push address (not value)
3. **Callee-clean push order** — external/assembly routines get left-to-right parameter push
4. **Parameter size tracking** — value params use MOVE.W (2 bytes), pointers use MOVE.L (4 bytes)
5. **SDL frontend runs toolchain** — `build/lisaemu Lisa_Source` now builds from source and boots

### Runtime logs from last run:
```
Exception counts: v4=1 v11=1
Vectors (RAM): TRAP1=$000507E6 TRAP2=$00050546 INT1=$00052724
DIAG frame 120: PC=$04B2E0 SR=$2140 stopped=0 pending_irq=0 setup=0
SGLOBAL@$2A0=$00000000
```

## Immediate Tasks

### 1. Fix constant resolution for procedure parameters
INTSOFF(allints, dummy) — the `allints` constant ($700) is not resolving correctly.
The codegen evaluates it to $005A instead of $0700. This means:
- Interrupt mask not set to level 7 (all off)
- SR ends up as $205A instead of $2700
- `gen_expression` for constant identifiers needs to look up shared globals correctly

### 2. Investigate Illegal Instruction at $6D
After INTSOFF returns, execution eventually jumps to $6D (inside vector table).
Something in the INITSYS flow makes a bad jump. Need to trace where the jump originates.

### 3. b_sysglobal_ptr still zero
SGLOBAL@$2A0=$00000000 — either INIT_TRAPV hasn't run yet, or the value isn't set.
Need to verify if INIT_TRAPV is reached after INTSOFF.

## Key Files

- `src/toolchain/pascal_codegen.c` — Procedure signatures, VAR params, push order
- `src/toolchain/pascal_codegen.h` — `cg_proc_sig_t` struct with is_external, param_size
- `src/toolchain/linker.c` — Vector table installation in output binary (Phase 5)
- `src/toolchain/toolchain_bridge.c` — Shared proc_sigs accumulation across units
- `src/m68k.c` — Exception tracing, TRAP handler diagnostics
- `src/lisa.c` — System.os loader (no longer overwrites vector table)
- `src/main_sdl.c` — Now supports `build/lisaemu Lisa_Source` (toolchain + boot)

## Toolchain Metrics

```
Parser:    405 Pascal files, 360 OK (99.5% real code, 2 edge cases)
Assembler: 105 files, 100% success, 0 errors
Codegen:   93.2% symbol resolution (rest resolve at link time)
Linker:    97.2% JSR to real code, ~108 stub relocations (~39 truly missing symbols)
Output:    ~331KB system.os (kernel only, fits in 1MB RAM)
Vectors:   17 exception vectors pre-installed in output binary
```

## Architecture

### Calling Convention Fix (this session)
- **Callee-clean (assembly)**: Parameters pushed left-to-right, callee pops
  - VAR params: push address via gen_lvalue_addr + MOVE.L A0,-(SP)
  - Value params: push value via gen_expression + MOVE.W/MOVE.L D0,-(SP)
- **Caller-clean (Pascal)**: Parameters pushed right-to-left, caller adjusts SP
- Procedure signatures (`cg_proc_sig_t`) track: VAR flags, param sizes, is_external
- Signatures propagated across compilation units via shared_proc_sigs table

### Key fixes history
- **Vector table**: Linker installs 17 vectors in $0-$FF of output binary
- **TRAP #7 works**: INTSOFF → TRAP #7 → TRAP7 handler → returns correctly
- **VAR parameters**: Signature-based VAR detection, correct address passing
- **Parameter push order**: Left-to-right for external, right-to-left for Pascal
- All previous session fixes (CONST resolution, module_idx, kernel separation, etc.)

### Remaining toolchain issues (not blocking boot)
- 39 truly missing symbols (print system, SU runtime, etc.)
- Clascal vtable dispatch not implemented
- {$I} 3 files not found (Apple didn't release them)
- 2 parser edge cases (MATHLIB param syntax, LCUT conditional)
- Constant identifiers in procedure call arguments may not resolve correctly
