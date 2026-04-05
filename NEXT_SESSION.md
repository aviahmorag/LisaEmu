# LisaEmu — Next Session Plan (April 5, 2026)

## Quick Start

```bash
make                        # Build standalone emulator
build/lisaemu Lisa_Source   # Run from source directory (builds + boots)
make audit                  # Full toolchain report
```

## Current State: OS boots deep into INITSYS with 1 minor exception

The CPU boot path works:
```
ROM ($FE0400) → set SP/A5/A6/SR → JMP $400 → STARTUP → PASCALINIT
→ copy runtime data to sysglobal → set A5 → return param ptr
→ INITSYS(ldmapbase) → INTSOFF (TRAP #7 works) → GETLDMAP (copies loader params)
→ REG_TO_MAPPED → deeper init → ... → 1 illegal instruction at $6D
```

### Runtime output:
```
Exception counts: v4=1 (single illegal instruction)
DIAG frame 120: PC=$04B87C SR=$2700 stopped=0 pending_irq=0 setup=0
A6=$4D7E9000 (corrupt frame pointer — likely another nested scope issue)
Vectors (RAM): TRAP1=$00050D82 TRAP2=$00050AE2 INT1=$00052CC0
SGLOBAL@$2A0=$00000000 (b_sysglobal_ptr not yet set)
```

## Session 2 Fixes (11 commits)

1. **Vector table installed by linker** — 17 exception vectors in output binary $0-$FF
2. **VAR parameter passing** — `cg_proc_sig_t` tracks VAR/size/external per parameter
3. **Callee-clean push order** — external assembly routines get left-to-right push
4. **Parameter size tracking** — MOVE.W for 2-byte value params, MOVE.L for 4-byte
5. **CONST resolution** — `is_const` flag, emit MOVEQ/MOVE.L #imm (not A5-relative load)
6. **Zero-arg function calls** — PASCALINIT correctly called as function-as-value
7. **Stack return convention** — external functions get MOVE.L (SP)+,D0 after JSR
8. **Loader parameter block** — version=22 at $53100, downward layout, 48-element arrays
9. **Boot ROM** — sets A5=$70000 (user area) and A6=$79000 (frame pointer sentinel)
10. **Array CONST bounds** — resolves identifiers like `maxsegments` in array[1..maxsegments]
11. **Nested scope variable access** — frame pointer chain walk for outer-scope vars
    - ROOT CAUSE of code corruption: GETLDMAP wrote through pointers using wrong A6
    - `find_local_depth()` + `emit_frame_access()` generate MOVEA.L (A6),A0 chain

## Immediate Task: Fix Remaining A6 Corruption

### The problem
A6=$4D7E9000 at DIAG time — corrupt frame pointer. This causes the illegal instruction
at $6D (UNLK restores bad A6, SP goes to vector table area). Same pattern as the
GETLDMAP corruption but in a different function deeper in init.

### What to investigate
1. **Which function corrupts A6?** — Add trace for when A6 leaves valid range ($070000-$07A000).
   The nested scope fix handles reads/writes/addr-of, but there may be other code paths
   that access outer-scope variables (e.g., pointer dereference through a frame variable,
   WITH statements, or the VAR-param case in gen_lvalue_addr).

2. **WITH statements** — Not implemented in codegen. Lisa OS uses WITH extensively for
   record field access. `WITH rec DO field := val` is syntactic sugar that the codegen
   needs to handle. Check if the parser creates AST nodes for WITH.

3. **b_sysglobal_ptr still $0** — SGLOBAL@$2A0=$00000000. INIT_TRAPV should set this
   (it does `MOVE.L A2,SGLOBAL` where SGLOBAL EQU $2A0). Either INIT_TRAPV hasn't run
   yet, or the parameter (b_sysglobal_ptr) passed to it is 0.

4. **Loader parameter block validation** — The params at $53100 (downward) may have
   wrong values for some fields. GETLDMAP copies them to INITSYS locals. Check if
   key values (b_sys_global, b_superstack, etc.) match what the OS expects.

## Key Files

- `src/toolchain/pascal_codegen.c` — All codegen: CONST, proc sigs, nested scope, alignment
- `src/toolchain/pascal_codegen.h` — `cg_proc_sig_t`, `is_const` flag on symbols
- `src/toolchain/linker.c` — Vector table installation, debug symbols, stub patching
- `src/toolchain/toolchain_bridge.c` — Shared proc_sigs, shared globals propagation
- `src/toolchain/bootrom.c` — Boot ROM with SP/A5/A6/SR init
- `src/lisa.c` — Loader parameter block (downward layout from $53100), system.os loader
- `src/lisa_mmu.c` — Memory write watchpoint at $4EE (can repurpose for other addresses)
- `src/m68k.c` — TRAP tracing, odd-PC detection, kernel escape detection, 256-entry ring buffer
- `src/main_sdl.c` — Toolchain-aware SDL frontend

## Architecture Notes

### Calling Convention (implemented)
- **Callee-clean (assembly)**: Left-to-right push, MOVE.W for 2-byte params, callee pops
- **Caller-clean (Pascal)**: Right-to-left push, caller adjusts SP
- External functions return results on stack (MOVE.L (SP)+,D0 after JSR)
- Proc signatures propagated via shared_proc_sigs table across compilation units

### Nested Scope Access (implemented)
- `find_local_depth()` returns 0 for current scope, 1 for parent, etc.
- `emit_frame_access(depth)` generates A0 = chain of (A6) dereferences
- Used in gen_expression (read), gen_lvalue_addr (address-of), gen_statement (assign)

### Memory Layout
```
$000000-$0003FF: Vector table (17 vectors from linker, rest = stub)
$000400-$052xxx: OS code (system.os, ~331KB)
$053100-down:    Loader parameter block (version=22, memory map)
$054000-$05A000: System global (sysglobal)
$05A000-$05E000: Supervisor stack
$05E000-$066000: Sysglobal heap
$066000-$06A000: Syslocal
$06A000-$06E000: User stack (outer process)
$070000:         Initial A5 area (PASCALINIT copies 32 bytes to sysglobal)
$079000:         Initial SP/A6 (supervisor stack pointer)
$07A000-$0FF800: Screen buffer + free memory
```

## Toolchain Metrics
```
Parser:    405 Pascal, 360 OK (99.5%)
Assembler: 105 files, 100% success
Codegen:   Proc signatures, CONST resolution, nested scope, array bounds
Linker:    97.2% JSR to real code, 17 vectors, ~83 stub symbols
Output:    ~331KB system.os, boots with 1 minor exception
```
