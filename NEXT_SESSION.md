# LisaEmu — Next Session Plan

## Current State (April 4, 2026)
- OS runs continuously, no crashes, 6,621 symbols, HAllocate resolved
- TRAP vectors still $00FE0300 (boot ROM RTE) — INIT_TRAPV not effective
- CPU loops in initialization code with SR=$2700 (interrupts masked)
- Display: white

## Immediate Blocker: Type-Aware Variable Access in Codegen

INIT_TRAPV(b_sysglobal_ptr) is called but `b_sysglobal_ptr` (an absptr = 32-bit
pointer) is loaded with MOVE.W (16-bit), so only the low 16 bits are passed.
INIT_TRAPV receives garbage in the high word and writes vectors to wrong addresses.

The codegen uses MOVE.W for ALL variable loads/stores. This is correct for INTEGER
(16-bit) but wrong for:
- Pointers (^type, absptr) — 32 bits
- LONGINT — 32 bits  
- Records, arrays — various sizes

**Attempted fix (reverted):** Changing all MOVE.W to MOVE.L broke stack frames
because 2-byte variable slots overflowed into adjacent slots.

**Correct fix:** Track variable types through the codegen and use the right size:
- `type->size == 4` → MOVE.L (pointers, longint)
- `type->size == 2` → MOVE.W (integer, boolean as word)
- `type->size == 1` → MOVE.B (boolean, char)

The type info IS available in some cases (sym->type->size), but many variables
have `type = NULL` defaulting to size 2. Need to:
1. Propagate type info when declaring variables (VAR sections)
2. Use type-based MOVE size for local reads: `emit16(cg, sz==4 ? 0x202E : 0x302E)`
3. Use type-based MOVE size for local writes: `emit16(cg, sz==4 ? 0x2D40 : 0x3D40)`
4. Also update frame allocation to use 4 bytes for pointer variables

This is the single most impactful change remaining.

## Session Summary (12 commits)
1. ADDQ stack cleanup encoding
2. Forward branch displacement off-by-2
3. CASE statement codegen
4. INTRINSIC SHARED parsing (+700 symbols)
5. INTERFACE functions as FORWARD
6. Variant record depth counting (unlocked HAllocate)
7. Linker per-module limits 4096 → 16384
8. Parser error recovery
9. Function args pushed as longwords
10. EXTERNAL routines: callee-clean (skip caller cleanup)
11. Longword variable access (reverted — too broad)
12. Diagnostics for VIA/vectors/symbols
