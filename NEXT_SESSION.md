# LisaEmu — Next Session Plan (April 9, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source                                    # cross-compiled
build/lisaemu --image prebuilt/los_compilation_base.image   # pre-built
```

## Where We Are: OS Running, Interrupts Enabled

Three fixes applied this session. The OS scheduler runs and dispatches
to SYSTEM.LLD driver code via interrupts.

### Fixes Applied

1. **Boot device $1B3 = 2** (was 0): FIND_BOOT selects ProFile on parallel port
2. **I/O segment MMU translation**: Ignores SOR for I/O segments ($FC0000 + offset)
3. **INTSON HLE**: Lowers IPL from 7→0 when scheduler starts at $520840

### Pre-built image status
- Boot loader reads 199 ProFile sectors (SYSTEM.LLD, SYSTEM.OS, CDD, drivers)
- OS initializes with HARDCODED values at $52051C (not GETLDMAP/param block)
- VIA1 configured for ProFile: PCR=$48, ORB=$CA, Timer loaded
- VIA2 configured for COPS: sends $7C mouse enable
- Level 1 interrupt handler installed at $5208A6
- After INTSON HLE, interrupts fire → driver dispatch at $208A06 runs
- **Still stuck**: driver dispatch checks $494 (ProFile ptr=0), loops

### Key discovery: OS 3.1 uses hardcoded initialization
The pre-built OS does NOT use the source code's GETLDMAP/PASCALINIT/ENTEROP
mechanism. Instead:
- $52051C clears $CC0000-$CC0868 (sysglobal) and writes constants directly
- $520620-$5206E8 configures VIA1 (ProFile) and VIA2 (COPS) hardware
- $5206D4 installs interrupt handler at vector $64 (level 1 = $5208A6)
- $520700+ sends COPS commands, sets up interrupt vectors
- No PRAM read, no CDD loading, no CALLDRIVER — all hardcoded

The ENTEROP code exists at physical $10036C (MOVE.L (SP)+,$218) but
is never executed. The boot track bypasses the full LOADER and jumps
directly to SYSTEM.OS.

## Immediate Next Step: ProFile Driver Initialization

$494 (ProFile driver pointer) is zero. The hardcoded OS init configures
VIA1 for ProFile but doesn't set up the driver entry point.

### Investigation needed
1. **Find where $494 should be set**: Search the OS binary for writes
   to $494. The hardcoded init might set it from a specific code path
   that we're missing (e.g., a branch we take incorrectly).
2. **Check VIA1 interrupt setup**: The OS configures VIA1 PCR=$48 but
   VIA1 IER=$00 (no interrupts enabled). ProFile requires VIA1 CA1
   interrupt (BSY transition) to work. The init code might enable
   VIA1 interrupts at a later point.
3. **Trace the interrupt handler at $5208A6**: When a level 1 interrupt
   fires, the handler at $5208A6 dispatches to $208A06 which checks
   $494/$498. If $494 is set, it would call the ProFile driver.
4. **Check I/O board detection branch**: At $520680, the code tests
   $FCC031 bit 7. For iob_lisa (our value 0x00), bit 7=0 → branches
   to $5206AC. The skipped code ($520694-$5206A9) writes to sysglobal
   fields. Maybe the skipped code is needed for ProFile setup.

### Key addresses
```
$494 (ProFile driver ptr) = $00000000  ← needs fixing
$498 (COPS driver ptr)    = $00204016  ← working
Loader link   $204 = $001007B8
INT1 vector   $064 = $005208A6
VIA1: PCR=$48 IER=$00 IFR=$00  ← no interrupts enabled!
VIA2: IER=$02 IFR=$82          ← CA1 interrupt pending
```

### Option A: Find the missing ProFile driver init
The hardcoded OS must set $494 somewhere. Search for the instruction
that writes to $494 (MOVE.L xxx,$0494.W). It might be conditional on
a hardware check that fails in our emulation.

### Option B: HLE the driver pointer
Set $494 to a ProFile driver entry point in SYSTEM.LLD. The driver
code is loaded; we just need to find its entry and wire it up.

## Session Summary: 3 fixes, major architectural discovery

### Critical fixes:
- Boot device $1B3=2 → ProFile on parallel port
- I/O segment MMU → correct I/O address translation
- INTSON HLE → interrupts enabled after OS init

### Key discoveries:
- Pre-built OS 3.1 uses HARDCODED init (not GETLDMAP/param block)
- Boot track bypasses ENTEROP, jumps directly to OS
- ENTEROP code exists at $10036C but never executes
- $218 (adrparamptr) never written — OS doesn't use it
- OS init: clear sysglobal, configure VIAs, install interrupt handler
- Interrupts work after INTSON HLE — level 1 handler dispatches correctly

### Boot sequence achieved:
```
Pre-built: ROM → LDPROF → SYSTEM.LLD → SYSTEM.OS → Scheduler → IRQ dispatch ✅
           But: ProFile driver not initialized ($494=0)
Cross-compiled: ROM → PASCALINIT → INITSYS → POOL_INIT → Deep init ✅
```
