# LisaEmu — Next Session Plan (April 9, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source                                    # cross-compiled
build/lisaemu --image prebuilt/los_compilation_base.image   # pre-built
```

## Where We Are: OS Init, COPS Handshake Working

Three fixes applied this session. The OS now gets past INTSON, sends COPS
commands, and enters the interrupt-driven scheduler phase.

### Fixes Applied This Session

1. **COPS data queue without CA1 trigger**: Queue keyboard ID ($80, $2F)
   directly in cops_rx without calling cops_enqueue, which would trigger
   VIA2 CA1 interrupt. The OS polls VIA2 port B for data availability
   (polling), not via interrupt. This prevents an unresolvable IRQ loop
   where the handler couldn't clear CA1 because the COPS driver wasn't
   initialized yet.

2. **COPS CRDY handshake (port B bit 6)**: Implemented proper COPSCMD
   protocol from Lisa OS source (libhw-DRIVERS.TEXT). CRDY (bit 6) starts
   high (ready), stays high for the first read after a command write
   (sanity check), then goes low (COPS busy/accepted) for a few reads,
   then returns high. Previously CRDY was tied to cops_rx.count, which
   was always wrong.

3. **Vertical retrace status bit timing**: Changed status register
   ($FCF801) bit 2 from frame-based toggle to cycle-based position within
   the frame. Uses `total_cycles % CYCLES_PER_FRAME` to determine
   vretrace window (last 10% of frame). Previously only toggled between
   frames, causing the OS vretrace polling loop to hang within a frame.

### Pre-built image status
- Boot loader reads 199 ProFile sectors ✅
- OS initializes with hardcoded values at $52051C ✅
- VIA1 configured for ProFile, VIA2 for COPS ✅
- INTSON HLE fires, lowers IPL, sets up ProFile driver ✅
- COPS command $7C (mouse enable) sent with proper handshake ✅
- Vretrace polling loop at $520952 works (exits/re-enters) ✅
- **Still stuck**: OS oscillates between vretrace wait ($520952) and
  COPS handler init code ($208Axx). The interrupt handler runs COPS
  init code (copies ROM data, sets up driver tables) and includes
  vretrace waits. Exits via RTE at $520464 to COPS dispatch at $208Axx.
  System cycles through this but doesn't complete COPS init.

### Key discovery: COPSCMD protocol
From Lisa OS source `LISA_OS/LIBS/LIBHW/libhw-DRIVERS.TEXT`:
- D2 = 6 (CRDY bit number — bit 6 of port B, NOT bit 4)
- Write command to IORA2 (register $0F = ORA no-handshake)
- Check CRDY=1 (sanity), then poll 16× for CRDY=0 (accepted)
- Set DDRA to output, spin 10 cycles, set back to input
- The OS uses vretrace-timed polling loops for COPS status

## Immediate Next Step: COPS Driver Init Completion

The system is in the COPS interrupt handler initialization phase. It
copies ROM code to RAM, sets up driver dispatch tables, and processes
COPS data from an internal buffer. To advance:

### Investigation needed
1. **Why does COPS init take so many frames?** The handler copies data
   with DBRA loops ($208AF6: MOVE.L (A0)+,(A1)+ / DBRA D1,...). These
   run for many iterations. Check if the copy length is correct or if
   it's reading invalid data/addresses.

2. **VIA2 port A reads**: The handler NEVER reads VIA2 port A to get
   COPS data. It processes data from internal buffers at $208Axx. The
   queued keyboard ID ($80, $2F) is never consumed. This may be correct
   (the interrupt handler would read port A in the Level 2 interrupt
   handler, but Level 2 doesn't fire because VIA2 CA1 was never set).

3. **COPS response to $7C**: The mouse enable command ($7C) gets no
   response queued. On the real Lisa, COPS just starts sending mouse
   delta data. The handler may be waiting for the first COPS interrupt
   (VIA2 CA1) to confirm the mouse is active. We may need to trigger
   CA1 with a mouse-ready response after the $7C command.

4. **Check if the COPS init ever completes**: The handler code at
   $208Axx processes bytes from a buffer (CMP.B #$0D at $208A78).
   It may be walking through a driver table. Check if it terminates
   or loops due to bad data.

### Key addresses
```
$520952  Vretrace polling loop (MOVE.L D0,$FCE018; BTST #2,$FCF801)
$520464  RTE from exception handler
$208A06  Entry to COPS dispatch (MOVEM.L regs,-(SP))
$208AF6  COPS data copy loop (MOVE.L (A0)+,(A1)+ / DBRA D1,...)
$2089C4  Handler registration check (CMPA.L $0094,A0)
$204016  COPS driver entry ($498)
$208390  ProFile driver entry ($494)
```

### VIA2 port B bit assignments (from Lisa OS source)
```
Bit 0: Data available from COPS (1=data ready)
Bit 6: CRDY — COPS ready for command (1=ready, 0=busy)
       (NOT bit 4 as previously assumed)
```

## Session Summary

### Critical fixes:
- COPS data queue without CA1 → prevents unresolvable IRQ loop
- COPS CRDY handshake on bit 6 → COPSCMD protocol works
- Vretrace cycle-based timing → polling loop exits within frames

### Boot sequence achieved:
```
Pre-built: ROM → LDPROF → SYSTEM.LLD → SYSTEM.OS → MMU setup →
           INTSON → COPS $7C → Vretrace wait → Handler init ✅
           But: COPS handler init loops, doesn't complete
Cross-compiled: ROM → PASCALINIT → INITSYS → POOL_INIT → Deep init ✅
```
