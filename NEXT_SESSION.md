# LisaEmu — Next Session Handoff (2026-04-13)

## Accomplished this session

### Codegen fixes (src/toolchain/pascal_codegen.c)

| Fix | Line(s) | Description |
|-----|---------|-------------|
| P6 | :150, :570 | Boolean size 1→2 bytes; expr_size checks is_const before type |
| P7 | :2362 | Interface proc/func declarations no longer export linker symbols |
| P8 | :2461, :1064 | Procedure-local CONST processing; no EXT.L on function call results |
| P9 | :1049 | Boolean NOT: bitwise NOT.W → logical TST/SEQ/ANDI |
| P10 | :2520-2550, :2574-2588 | Function result local variable + load into D0 before RTS |
| P11 | :1091-1118 | Nested binary ops save D2 on stack when right operand is complex |
| P12 | :1019-1032 | true/false/nil recognized inside WITH block fallthrough path |

### Memory layout fixes (src/lisa.c)

| Line(s) | Fix |
|---------|-----|
| :1628 | himem = b_dbscreen (below screen buffers, ~$230000) |
| :1630 | bothimem = lomem (no resident loader to protect) |
| :1611 | l_sgheap = $7E00 (page-aligned, fits signed int2) |

### Linker (src/toolchain/linker.c)

| Line | Fix |
|------|-----|
| :858 | Added MAKE_FREE, INSERTSDB, AVAIL_INIT, etc. to debug symbol list |

### Result

**MILESTONE**: Boot passes ALL of INITSYS initialization and reaches
the scheduler idle loop at ~$A17xx. 600 headless frames, zero
SYSTEM_ERRORs. Previously stuck at SYSTEM_ERROR(10709).

## In progress

The scheduler loop at ~$A17xx runs but doesn't progress. The CPU
polls at SR=$2714 (supervisor mode, IPL=7 — all interrupts masked).

Likely needs:
1. **Timer interrupts** — VIA1 T1 should fire scheduler ticks.
   The VIA is initialized (see HLE ProFile init ~lisa.c:3030) but
   IPL=7 masks all interrupts. The OS should lower IPL via INTSON
   at some point during initialization.
2. **Disk I/O** — the OS may be waiting for ProFile I/O to load
   the root process. HLE CALLDRIVER is active but may not be
   getting invoked from the polling loop.
3. **TRAP #1 system calls** — check if the scheduler is dispatching.
   INIT_SCTAB sets up the syscall table. The idle loop may be
   waiting for a TRAP #1 that never fires.

## Blocked / open questions

- The boot runs in **supervisor mode** throughout (started by
  REG_TO_MAPPED's ANDI #$DFFF,SR but something switches back).
  IPL=7 masks all interrupts. Need to understand when the OS
  expects interrupts to be enabled.
- The `(* params *)` pattern (commented-out parameter lists in
  IMPLEMENTATION bodies) works via proc_sig import, but some
  functions may still have wrong parameter offsets if their
  signatures weren't imported. Watch for parameter mismatches.
- The D2 stack-save fix (P11) adds 4 bytes per nested binary op.
  This increases code size. If any jump displacements overflow
  16-bit signed range ($7FFF), BRA.W/BEQ.W would fail silently.

## Pick up here

> Boot reaches the scheduler idle loop with zero errors. The CPU
> polls at $A17xx with IPL=7 (all interrupts masked). Run 2000+
> headless frames and check: (1) is HLE CALLDRIVER being invoked?
> (2) are any TRAP #1 syscalls firing? (3) what does the PC ring
> buffer show? The most likely blocker is that the OS never lowers
> IPL to enable timer interrupts — check the INTSON calls after
> INIT_PROCESS and see if they execute.
