# LisaEmu — Next Session Plan

## Current State (April 4, 2026)
- 276/276 Pascal, 96/96 assembly compile, **6,327 symbols** linked
- Binary: 1.6MB, OS reaches TRAP #1 syscall layer
- Exception counts: v4=241 v11=2 v33=86 (was 7.2M Line-F before this session)
- Still 9,973 JSR $000000 patched to stub (down from original but up because more code compiled)
- UNITHZ INTERFACE parses but IMPLEMENTATION blocked by `:=` typed-constant error at line 307

## Session Summary (what was fixed)
1. **ADDQ stack cleanup** — `arg_bytes/2` → `arg_bytes&7` in ADDQ encoding
2. **Forward branch displacement** — removed erroneous `-2` from IF/WHILE/FOR patches
3. **CASE statement** — implemented if-else chain dispatch (was stub)
4. **INTRINSIC SHARED** — parser now handles qualifiers after INTRINSIC keyword
5. **Linker limits** — per-module symbols/relocs raised from 4096 to 16384
6. **Parser error handling** — continue codegen on partial ASTs instead of discarding
7. **Parser bailout limit** — raised from 20 to 200

## Priority 1: Fix UNITHZ IMPLEMENTATION Parsing

**Problem**: Parser hits `expected =, got :=` at line 307 of UNITHZ. This is INSIDE the
IMPLEMENTATION section's function body code, meaning `parse_declarations` on the INTERFACE
section consumed past `IMPLEMENTATION` into actual code.

**Root cause**: Something in the INTERFACE declarations (lines 31-206) confuses the parser,
causing it to consume tokens past the IMPLEMENTATION keyword. Most likely a FUNCTION/PROCEDURE
declaration in the INTERFACE that gets misparsed, causing the parser to treat IMPLEMENTATION
as part of a declaration body.

**Investigation**: Check what's around lines 140-206 in UNITHZ INTERFACE — specifically:
- FUNCTION declarations with complex parameter lists (records, sets, function pointers)
- `{$IFC fOS}` conditional blocks where fOS might not be defined
- USES clause references

**Impact**: Fixing this unlocks HAllocate, FreeH, HzInit, and 80+ other heap/memory functions.

## Priority 2: TRAP #1 Handler (OS System Calls)

The OS now reaches TRAP #1 instructions (vector 33). These are Lisa OS system calls
(MOVE_INFO, MAKE_PROCESS, etc.). The boot ROM handler at $FE0300 just does RTE,
which doesn't handle syscalls. The OS startup code uses TRAP #1 to set up the initial
exception vectors via the OS's own exception handler.

**Fix**: The EXCEPASM assembly module (already compiled) contains the real TRAP handlers.
The OS needs to install its own exception vectors early in boot. Check if INIT_TRAPV
(which sets up the vector table) is being called and if its code is correct.

## Priority 3: Remaining Unresolved Symbols

Now seeing LIBSU unresolved symbols (FreeBk, AllocBk, PMapN, etc.) — these are from
newly-compiled units that were previously empty due to INTRINSIC SHARED bug.

**LogCall** — appears ~40 times across LIBDB, LIBPR, LIBSU. Never defined in source.
Should be a no-op stub added to the linker.

## Key files
```
src/toolchain/pascal_parser.c   — INTRINSIC SHARED fix, parse_unit()
src/toolchain/pascal_codegen.c  — CASE, branch displacement, stack cleanup
src/toolchain/linker.c          — limits, relocation patching
src/toolchain/linker.h          — per-module array sizes
src/m68k.c                      — 68000 CPU emulator
```
