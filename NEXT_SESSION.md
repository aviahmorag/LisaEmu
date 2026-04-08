# LisaEmu — Next Session Plan (April 8, 2026)

## Quick Start

```bash
make
build/lisaemu --image prebuilt/los_compilation_base.image   # pre-built
build/lisaemu Lisa_Source                                    # cross-compiled
```

## Where We Are

### Pre-built image: past BOOTINIT, loading SYSTEM.LLD
The Pascal boot loader completes BOOTINIT (200+ MMU segment writes via
DO_AN_MMU/TRAP #6). LOADSYS begins reading SYSTEM.LLD blocks from disk.
Crashes at PC=$2A (bad jump to vector table) after ~30 disk reads.

### Cross-compiled: POOL_INIT codegen
POOL_INIT receives correct parameters. INIT_FREEPOOL has codegen bugs
(parameter loaded as constant, undersized LINK frame).

## Fixes This Session (8 fixes, 7 commits)

1. **MMU stack SOR** — hw_adjust for stack segments
2. **32-bit multiply** — inline 32×32→32 partial products
3. **32-bit divide** — inline shift-and-subtract loop
4. **Pre-built image boot** — `--image` flag, prof_entry HLE
5. **ProFile block reads** — no deinterleave needed (physical block order)
6. **RAM address wrapping** — wrap at physical memory boundary
7. **MMU context mirroring** — context 1→0 for setup mode access
8. **RAM 2.25MB** — eliminate mmucodemmu physical address aliasing

## Current Blockers

### Pre-built image: crash after SYSTEM.LLD read starts
After 30 PROF_ENTRY reads, crashes at PC=$2A. The 2nd round of disk reads
loads sectors to $7BFE+ (safe place area). Need to investigate:
- Whether asm_read_block double-interleaves (also applies interleave like ldr_read_block)
- Whether the loaded data lands at the correct addresses
- What triggers the jump to $2A

### Cross-compiled: INIT_FREEPOOL codegen
Unchanged from earlier — parameter access generates constant load.

## Boot Sequence (pre-built image)
```
✅ ROM → LDPROF → load boot track → Pascal loader entry
✅ BOOTINIT → SETVARS → BGETSPACE → INITMMUTIL
✅ INITMMUTIL → program mmucodemmu, install TRAP #6, copy DO_AN_MMU
✅ BOOTINIT loop → 200+ PROG_MMU calls via TRAP #6 (all 512 SMT entries)
✅ LOADSYS begins → spare table query → starts reading blocks
💥 Crash at PC=$2A after ~30 disk reads
❌ LOAD_LLD → complete SYSTEM.LLD loading
❌ LOAD_OS → read SYSTEM.OS
❌ ENTEROP → jump to OS entry point
```

## Key Insight: All Fixes Help Both Paths

| Fix | Cross-compiled | Pre-built | Why |
|-----|:---:|:---:|-----|
| MMU stack SOR | ✅ | ✅ | Same hardware MMU |
| 32-bit mul/div | ✅ | — | Codegen fix |
| ProFile reads | ✅ | ✅ | Same disk I/O |
| RAM wrapping | ✅ | ✅ | Same memory controller |
| Context mirroring | ✅ | ✅ | Same setup mode behavior |
| RAM 2.25MB | ✅ | ✅ | Same address space |
