# LisaEmu — Next Session Plan

## Current State (April 4, 2026)
- OS runs continuously, no crashes, 6,621 symbols, HAllocate resolved
- TRAP vectors still $00FE0300 — INIT_TRAPV passes 0 for b_sysglobal_ptr
- Root cause found: b_sysglobal_ptr is a USES import, not a local variable
- The codegen can't find cross-unit variables → emits MOVE.W #0,D0

## Immediate Blocker: Cross-Unit Variable Imports (USES)

`b_sysglobal_ptr` is defined in source-SYSGLOBAL.TEXT (the OS globals unit).
STARTUP declares `USES ... SysGlobal ...` to import it. But the codegen
doesn't process USES clauses — imported variables aren't in the symbol table.

When the codegen encounters `INIT_TRAPV(b_sysglobal_ptr)`, it can't find
the symbol, so it emits MOVE.W #0,D0 (zero). INIT_TRAPV receives 0 as
b_sysglobal and writes garbage to the vector table.

**Fix approach:**
1. Parse USES clauses to get the list of imported unit names
2. When compiling a file, look up variables/types from USES'd units
3. The sysglobal variables are A5-relative globals — their offsets are
   defined in the sysglobal record structure
4. Alternative: since SYSGLOBAL is compiled as its own module, its symbols
   should be in the linker's global symbol table. The codegen could
   mark unresolved identifiers as externals and let the linker resolve them.

## What was fixed this session (14 commits)
All previous fixes plus:
- Type-aware MOVE.L for local variables with sz==4 (was buggy — emitted MOVE.W)
- Callee-clean calling convention for EXTERNAL assembly routines
- Function args pushed as 4-byte longwords
- Diagnostic logging for VIA timers, vectors, symbol resolution
