# LisaEmu — Next Session Plan (April 7, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source                    # Cross-compiled OS
build/lisaemu build/lisa_boot.rom image.img  # Pre-compiled OS from disk image
```

## Where We Are

**Clean boot from cross-compiled source — zero crashes, zero exceptions.**
Stuck at CALLDRIVER because boot device config never created (config_ptr=NULL).

**Pre-compiled disk image found** but not yet booting — needs prof_entry
PROM routine to read blocks during LDRLDR boot.

## The Two Paths (continue both)

### Path A: Cross-compiled OS (the vision)
- 319 files compile, assemble, link into 855KB kernel
- Boots cleanly through PASCALINIT → GETLDMAP → deep INITSYS
- Blocked at CALLDRIVER: config_ptr=NULL, 3 specific blockers identified
- Need: fix INIT_CONFIG/MAKE_BUILTIN to create boot device config

### Path B: Pre-compiled OS (validation)
- "AOS 3.0" disk image at _inspiration/LisaSourceCompilation-main/
- Decompress: gunzip the .cpgz → cpio extract → 48MB raw ProFile
- Format: 94,208 blocks × 532 bytes (20 tag + 512 data)
- MDDF at block 46, system.os at sfile 25 (~276KB)
- Need: implement prof_entry PROM routine for LDRLDR boot
- This validates hardware emulation independently of codegen

## What Was Fixed This Session (35+ commits)

### Codegen (the big ones)
- **ORD() on pointers** returned 2 not 4 → ROOT CAUSE of code overwrite
- **WITH statement** completely missing → implemented (206 OS instances)
- **30+ type-size fixes**: fields, arrays, ops, calls, FOR/CASE, intrinsics
- **Cross-unit type resolution**: dangling pointers, aliases, forward params

### Assembler
- MOVEM register list, @-label scoping, branch size consistency

### Boot/Loader
- smt_base moved above OS code, driver JT wired to DRIVERASM
- Loader stub with Lisa filesystem reader
- COPS pram handler, bootdev fix, PROM checksum

### Key Architectural Findings
- LisaEm uses pre-compiled OS, NOT cross-compilation
- LisaEm HLE is speed optimization, not functional necessity
- Pascal string[32] = 34 bytes (not 33) — shifts MDDF field layout
- blocksize=536 (24-byte page labels), slist_packing=36
- Lisa Pascal boolean = 1 byte in records

## Immediate Next Steps

1. **For Path B**: Implement prof_entry at $FE0090 that reads a ProFile
   block using our profile.c state machine (or direct image access).
   Then LDRLDR can load the main loader → system.os boots natively.

2. **For Path A**: Trace why INIT_CONFIG doesn't create device config.
   The HLE CALLDRIVER intercept is at wrong address ($CBDF8 vs GENIO
   entry). Fix intercept point or trace MAKE_BUILTIN codegen.

3. **For both**: Fix diskimage.c MDDF layout to match real image
   (34-byte strings, blocksize=536, slist_packing=36).
