# LisaEmu — Next Session Plan

## Current State (April 4, 2026)
- OS runs continuously (~1B+ cycles), no crashes
- 6,621 symbols, HAllocate resolved, binary 1.66MB
- TRAP vectors NOT installed — still $00FE0300 (boot ROM RTE)
- CPU stuck in scheduler loop at $097xxx with SR=$2700 (all interrupts masked)
- VIA timers not running, VIA IER=$00 (no interrupts enabled)
- Display: white (OS hasn't drawn)

## Immediate Blocker: INIT_TRAPV Not Installing Vectors

INIT_TRAPV (SOURCE-INITRAP.TEXT) should write TRAP1/TRAP2/etc handler
addresses to RAM $84/$88/etc. It's called from STARTUP/INITSYS. But
the vectors remain at $00FE0300.

**Root cause (most likely):** INIT_TRAPV is a callee-clean assembly routine:
```
MOVE.L (SP)+,A1      ; pop return PC
MOVE.L (SP)+,A2      ; pop b_sysglobal parameter
...
JMP (A1)             ; return via jump (not RTS)
```

But the Pascal codegen does CALLER-clean: after JSR, it does ADDQ.L #4,A7
to remove the 4-byte parameter. Combined with INIT_TRAPV already popping
the parameter, the stack gets double-adjusted by 4 bytes. This corrupts
the stack frame of INITSYS, making everything after INIT_TRAPV fail.

**Fix options:**
1. Don't clean up args for EXTERNAL assembly routines (detect callee-clean)
2. Make the codegen smarter about which routines are callee-clean
3. Modify the assembler to emit RTS-based wrappers for callee-clean routines

Actually, the standard Lisa Pascal convention is CALLER-CLEAN. Assembly
routines should NOT pop their own parameters. INIT_TRAPV is special —
it manipulates the stack directly because it needs to set up system
state. This one routine needs special handling.

## What was fixed this session (9 commits)
1. ADDQ stack cleanup encoding (A7 odd → crash)
2. Forward branch displacement off-by-2
3. CASE statement codegen
4. INTRINSIC SHARED parsing (+700 symbols)
5. INTERFACE functions as FORWARD
6. Variant record depth counting (unlocked UNITHZ/HAllocate)
7. Linker per-module limits 4096 → 16384
8. Parser error recovery + bailout limits
9. Function args pushed as 4-byte longwords (was 2-byte words)

## Key metrics progression
| Metric | Start of session | End of session |
|--------|-----------------|----------------|
| Symbols | 5,627 | 6,621 |
| Binary size | 1.5 MB | 1.66 MB |
| Line-F exceptions | 7,270,254 | 0 |
| CPU cycles to issue | ~150M (crash) | 1B+ (running) |
| HAllocate | NOT FOUND | Resolved ($8F6C2) |
| TRAP vectors | N/A | Still $FE0300 |
