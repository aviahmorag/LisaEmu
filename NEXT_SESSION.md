# LisaEmu — Next Session Plan (April 6, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source
```

## Current State: INITSYS RUNS, SCREEN DRAWS!

The Apple Lisa OS cross-compiles from 420+ source files, links with
the correct 46-module kernel, and boots through PASCALINIT → INITSYS.
The screen shows visual output (artifacts during init).

### Runtime:
```
PATCHED %initstdio at $0DBED6 to RTS (temporary bypass)
INITSYS entry at $4ABE (LINK A6, frame_size=684)
SYSTEM_ERROR(0) × 5 from $6C90C (investigating)
Screen: visible artifacts (OS writing to display)
```

### Key remaining issues:
1. **%initstdio bypass** — patched to RTS because SETCUR/SETA1 loops.
   The TRAP #5 screen handler works but something in the initio→SETCUR
   path still has issues. Need to debug the exact instruction that fails.
2. **SYSTEM_ERROR(0)** — called from $6C90C during init.
3. **ProFile disk I/O** — not reached yet (BOOT_IO_INIT needs debugging).

## Session 3+4 Summary (57 commits)

### Critical breakthroughs:
1. Multi-param parsing `(a,b,c:type)` → separate params (EVERY function)
2. $SELF relocation for local assembly labels
3. PC-relative `*+N` expressions (PASCALINIT trap7hand return)
4. MAX_SHARED_GLOBALS 16384→65536 (critical MMU constants)
5. TRAP #5 handler with ScreenAddr/AltScreenAddr
6. %initstdio bypass → INITSYS runs, screen draws

### Architecture:
- Kernel = 46 OS modules + LIBPL/LIBHW/LIBFP/LIBOS
- MMU: 5 contexts, segments 0-20, 85-105, 123 pre-programmed
- ProFile: protocol-accurate module ready (src/profile.c)
- MDDF: fsversion=17 (correct for LOS 3.1)
- All critical constants propagate (sysglobmmu=102, maxpgmmu=256, etc.)
- Zero parse errors, zero unresolved kernel symbols

## Next Steps

### 1. Fix %initstdio properly
Debug why SETCUR/SETA1/traptohw loop. The TRAP #5 handler returns the
screen address correctly. The issue might be in the MOVEM save/restore
or in the initio TRAP #7 interaction.

### 2. Debug SYSTEM_ERROR(0) at $6C90C
Error code 0 is unusual — might be a fall-through or bad error number.
Find what function is at $6C90C and why it triggers.

### 3. ProFile disk I/O
Once BOOT_IO_INIT runs, the ProFile driver will attempt VIA handshake.
Protocol module ready in src/profile.c.

### 4. Complete INITSYS
BOOT_IO_INIT → FS_INIT → SYS_PROC_INIT → ENTER_SCHEDULER → desktop
