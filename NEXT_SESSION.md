# LisaEmu — Next Session Handoff (2026-04-13)

## TL;DR

**MAJOR MILESTONE**: Boot passes ALL of INITSYS initialization and
reaches the scheduler idle loop. 600 frames, zero SYSTEM_ERRORs.
Fixed 12 codegen bugs (P1-P12) total across multiple sessions.

## Current state

The CPU runs at PC ~$A17xx (memory manager / scheduler area) in a
polling loop. SR=$2714 (supervisor mode, IPL=7). The boot successfully:

- PASCALINIT → INITSYS → GETLDMAP → REG_TO_MAPPED
- POOL_INIT → INIT_FREEPOOL (sysglobal + syslocal pools)
- MM_INIT → GETSPACE (both mmrb and mrbt allocations)
- INIT_TRAPV → INIT_SCTAB → DB_INIT
- AVAIL_INIT → MAKE_FREE → MAKE_REGION (all memory regions)
- INIT_CONFIG → GET_BOOTSPACE → GETFREE
- INIT_PROCESS → scheduler setup
- Reaches the idle/polling loop

## What's needed next

The scheduler loop at $A17xx is likely waiting for:
1. **Timer interrupts** — VIA1 T1 timer for scheduler tick. The VIA
   is initialized but interrupts may not be enabled yet.
2. **Disk I/O completion** — the OS may be trying to load the root
   process or init its file system, which needs ProFile I/O.
3. **Process creation** — the scheduler needs at least one runnable
   process. INIT_PROCESS creates the memory manager process, but
   the root process might not be created yet.

**Investigation approach:**
1. Run more frames (2000+) and check if the loop makes progress
2. Check if HLE CALLDRIVER intercepts are firing (disk I/O)
3. Check VIA interrupt state — are timer interrupts enabled?
4. Check TRAP #1 system call activity — is the scheduler dispatching?
5. Look at the PC ring buffer for the loop pattern

## Codegen fixes this session (P6-P12)

| Fix | Bug | Impact |
|-----|-----|--------|
| P6 | Boolean 1→2, const expr_size | PARMS misaligned, EXT.L |
| P7 | Interface symbol suppression | Wrong function addresses |
| P8 | Local consts, no func EXT.L | Wrong constants, zeroed returns |
| P9 | Boolean NOT | All `if not f()` always TRUE |
| P10 | Function result variable | Functions returned garbage |
| P11 | D2 stack save for nested ops | Compound conditions wrong |
| P12 | true/false inside WITH | `x := true` stored false |

## Key files

```
src/toolchain/pascal_codegen.c   All P6-P12 fixes
src/lisa.c                       Memory layout + HLE
```
