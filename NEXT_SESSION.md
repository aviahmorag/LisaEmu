# LisaEmu — Next Session Plan (April 9, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source                                    # cross-compiled
build/lisaemu --image prebuilt/los_compilation_base.image   # pre-built
```

## Where We Are: OS Init Polling, Interrupt Architecture Fixed

Six fixes applied this session. The interrupt architecture now matches
the real Lisa hardware. COPS keyboard data is consumed via HLE Level 2
handler. OS enters main init polling loop.

### Fixes Applied This Session

1. **COPS data queue without CA1 trigger**: Queue keyboard ID directly
   in cops_rx without via_trigger_ca1. Prevents unresolvable IRQ loop.

2. **COPS CRDY handshake (port B bit 6)**: Proper COPSCMD protocol from
   Lisa OS source. CRDY toggles: 1→1(sanity)→0(accepted)→1(ready).

3. **Vertical retrace cycle timing**: Status register bit 2 uses
   `total_cycles % CYCLES_PER_FRAME` for intra-frame vretrace.

4. **VIA2 IRQ level = 2**: Both VIA IRQs were level 1. Now VIA1=level 1
   (shared with vretrace) and VIA2=level 2 (COPS), matching real Lisa
   interrupt architecture from libhw-DRIVERS.TEXT.

5. **HLE Level 2 handler**: Binary's Level 2 handler at $2082D2 enters
   an infinite init loop. HLE intercepts it, reads VIA2 port A to
   consume COPS data, and RTEs directly.

6. **Removed FORCE_UNMASK**: Was causing nested interrupts inside handlers.
   With proper interrupt levels, it's unnecessary.

### Pre-built image status
- Boot → OS → MMU → VIAs → INTSON → COPS $7C → Level 2 HLE ✅
- Keyboard ID ($80, $2F) consumed by HLE COPS handler ✅
- **Still stuck**: OS enters vretrace polling loop at $520952. The loop
  exits periodically (every ~13K instructions when vretrace bit toggles)
  but an outer loop keeps re-entering it. The OS is in main init
  polling, waiting for something.

### Lisa interrupt architecture (from source)
```
Level 1 ($0064 = $5208A6):
  1. Check StatusRegister+1 bit 2 → VertRetrace handler
  2. Check VIA1 IFR bit 6 → Timer1 (20ms tick, alarms)
  3. Check VIA1 IFR & IER & $22 → ProFile I/O (CA1/Timer2)
  4. Check VIA2 PORTB bit 4 → Twiggy floppy
  5. Check VIA1 IER bit 2 → Shift register alarm
  6. RTE

Level 2 ($0068 = $2082D2):
  1. Save D0, raise IPL to 5
  2. Check VIA2 IFR bit 1 (CA1) → COPS handler
  3. Read VIA2 port A → COPS byte
  4. Dispatch: $00=mouse, $80=reset, else=keycode
  5. ENABLE, RTE
```

## Immediate Next Step: Main Init Polling Loop

The OS polls in a loop at $5207FE-$520822 with vretrace waits:
```
$5207FE: MOVE.B $xxxx.L,D0    ; read status byte
$520804: BTST #n,D0            ; test a bit
$520808: BEQ.S +4              ; skip if clear
$52080A: JSR $520964            ; call handler if set
...
$52081C: MOVE.W (SP)+,SR       ; restore SR
$52081E: MOVEM.L (SP)+,regs    ; restore regs
$520822: RTS                    ; return (to scheduler?)
```

### Investigation needed
1. **What status byte at $5207FE?** Decode the MOVE.B absolute long
   address to find which I/O register or memory location is checked.
   This tells us what event the OS is waiting for.

2. **VIA1 Timer1**: The Level 1 handler checks Timer1, but VIA1 Timer1
   is NOT running (t1_run=0). The OS might need Timer1 for 20ms ticks.
   Check if the OS init code starts Timer1, or if the INTSON HLE needs
   to start it.

3. **Level 1 interrupt delivery**: With vretrace IRQ pulsing after
   frame 200, Level 1 should fire. Check if the Level 1 handler at
   $5208A6 runs correctly (checks vretrace, Timer1, ProFile).

4. **ProFile driver response**: The OS might poll for ProFile I/O
   completion via VIA1 CA1. If ProFile never responds (no BSY
   transitions), the OS waits forever.

### Key addresses
```
$520952  Vretrace polling loop (main init)
$5207FE  Status byte check (what is it reading?)
$520464  RTE from init polling handler
$2082D2  Level 2 handler (HLE'd — consumes COPS data)
$5208A6  Level 1 handler (vretrace + VIA1)
```

## Boot Sequence Achieved
```
Pre-built: ROM → boot loader → SYSTEM.LLD + SYSTEM.OS →
           MMU setup (TRAP #6) → VIA1/VIA2 config →
           INTSON (IPL→0) → COPS $7C (mouse enable) →
           Level 2 IRQ → HLE reads keyboard ID ($80,$2F) →
           Main init polling (vretrace wait loop) ✅
           Stuck at: vretrace polling, waiting for next event
```
