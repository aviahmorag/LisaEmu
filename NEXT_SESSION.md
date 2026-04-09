# LisaEmu — Next Session Plan (April 9, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source                                    # cross-compiled
build/lisaemu --image prebuilt/los_compilation_base.image   # pre-built
```

## Where We Are: OS Kernel Running, Two Critical Fixes Applied

Both paths reach deep OS initialization. The OS scheduler runs and
dispatches processes. SYSTEM.LLD and SYSTEM.OS are loaded and executing.

### Fixes Applied This Session

1. **Boot device $1B3 = 2** (was 0): FIND_BOOT now maps to cd_paraport
   on iob_lisa, correctly identifying ProFile on the parallel port.
   Result: $498 now set to $204016 (COPS/keyboard driver entry).

2. **I/O segment MMU translation**: Segment 126 (I/O space) now ignores
   SOR for address calculation. On real Lisa hardware, I/O segments map
   the 17-bit offset directly to I/O space ($FC0000 + offset). The old
   code used `phys | 0xFC0000` which corrupted the offset when SOR != 0.

### Pre-built image: $520842 scheduler + $208A06 driver dispatch
- SYSTEM.LLD loaded, initialized, COPS commands sent ($7C mouse enable)
- SYSTEM.OS loaded at segment 41 ($520000+)
- OS scheduler at $520842 runs, dispatches driver code
- $498=$204016 (COPS driver set by SYSTEM.LLD), $494=$00000000 (ProFile driver NOT set)
- CPU stuck at IPL 7 (SR=$2704) — interrupts fully masked
- BOOT_IO_INIT runs partially but INTSON(0) never called

### Cross-compiled: past POOL_INIT, deep in OS init
- POOL_INIT, INIT_FREEPOOL, GETSPACE all work correctly
- TRAP #5/#6/#7 calls active → OS initializing
- Needs same hardware as pre-built path

## Root Cause: adrparamptr ($218) Not Set

The OS initialization chain is:
```
main → INITSYS(PASCALINIT) → GETLDMAP → POOL_INIT → ... → BOOT_IO_INIT
```

PASCALINIT reads `adrparamptr` ($218) to get the loader parameter block
pointer. On the real Lisa, the full loader (LDASM) stores this via:
```assembly
MOVE.L (SP)+,adrparamptr    ; store at $218
```

But in our emulation:
1. Boot track loader at $20000 builds param block at **$0FFEA4** (verified: version=22, valid b_sysjt, l_sysjt, etc.)
2. Boot track runs DO_AN_MMU (TRAP #6 loop) which **reprograms MMU segment 0**
3. This changes where virtual $218 maps physically
4. The loader's LDASM stores paramptr to virtual $218 → goes to the NEW physical address
5. But $218 was NEVER set (no `MOVE.L ...,adrparamptr` opcode found in the binary)
6. The pre-built Lisa OS 3.1 binary uses a DIFFERENT mechanism to pass the param pointer — **$0218 is not referenced anywhere in the loaded binary**
7. GETLDMAP reads garbage → SYSTEM_ERROR → OS enters error/scheduler loop

### Evidence
- No MOVEP.L instructions execute (INIT_READ_PM never called)
- No reads to PRAM at $FCC181 (INIT_CONFIG never reached)
- 199 PROF_ENTRY reads (loader successfully reads SYSTEM.CDD, SYSTEM.CD_PROFILE)
- Parameter block at $0FFEA4 has valid data but the OS never reads it
- Boot track writes to $21C-$21F (ldbaseptr=$100000) but NOT $218-$21B

## Immediate Next Step: Fix adrparamptr

The pre-built Lisa OS 3.1 binary doesn't use `MOVE.L $218,A2` in PASCALINIT.
It must receive the parameter block address through a different mechanism.

### Investigation needed
1. **Find how the real OS receives paramptr**: Scan the pre-built binary
   for how GETLDMAP gets its `ldmapbase` argument. It could be:
   - Passed via register (A2, A5, or stack) from the boot track
   - Read from a different low-memory address
   - Embedded in the code segment header
2. **Check the boot track's final jump**: Trace what registers/stack values
   the boot track sets up right before jumping to SYSTEM.OS. The paramptr
   might be on the stack or in a register.
3. **Alternative**: Build our own parameter block during reset using the
   values from $0FFEA4 (once the boot track builds it), and inject it
   via HLE at the exact right moment.

### Parameter block values (from $0FFEA4 dump)
```
version       = 22
b_sysjt       = $008208    l_sysjt       = $000CE8
b_sysglobal   = $003C00    l_sysglobal   = $00C000
b_superstack  = $000000    l_superstack  = $000618
b_intrin_ptrs = $008028    l_intrin_ptrs = $0001E0
b_sgheap      = $008EF0    l_sgheap      = $006D10
b_screen      = $1F8000    l_screen      = $008000
b_dbscreen    = $1F0000    l_dbscreen    = $008000
b_opsyslocal  = $000000    l_opsyslocal  = $002800
b_opustack    = $000000    l_opustack    = $008800
b_scrdata     = $1E3000    l_scrdata     = $01D000
b_vmbuffer    = $004600    l_vmbuffer    = $002BF8
b_drivers     = $003C00    l_drivers     = $000A00
himem         = $00FC00    lomem         = $1E3000
l_physicalmem = $200000
```

### Key addresses
```
Loader link   $204 = $001007B8  (boot track's LDRTRAP)
fs_block0     $210 = $001E      (MDDF block)
adrparamptr   $218 = $00000000  ← ROOT CAUSE
ldbaseptr     $21C = $00100000
dev_type      $22E = $0001      (ProFile)
$494 (ProFile driver) = $00000000  ← not initialized
$498 (COPS driver)    = $00204016  ← set by SYSTEM.LLD
```

## Session Summary: 2 fixes, deep investigation

### Critical fixes:
- **Boot device $1B3=2** → FIND_BOOT correctly selects ProFile
- **I/O segment MMU** → `0xFC0000 | offset` instead of `phys | 0xFC0000`

### Key discoveries:
- Boot track loader builds param block at $0FFEA4 (valid, version=22)
- $218 (adrparamptr) is NEVER written by CPU in pre-built image boot
- Pre-built Lisa OS 3.1 doesn't contain the `MOVE.L $218,A2` instruction
- The OS receives paramptr through an unknown mechanism
- BOOT_IO_INIT starts (reads $FCC031) but INIT_CONFIG/INIT_READ_PM never reached
- GETLDMAP likely fails → SYSTEM_ERROR → scheduler loop with IPL 7

### Boot sequence achieved:
```
Pre-built: ROM → LDPROF → Loader → BOOTINIT → SYSTEM.LLD → SYSTEM.OS → Scheduler ✅
           But: INITSYS → GETLDMAP → FAIL (no paramptr) → SYSTEM_ERROR
Cross-compiled: ROM → PASCALINIT → INITSYS → POOL_INIT → INIT_TRAPV → Deep init ✅
```
