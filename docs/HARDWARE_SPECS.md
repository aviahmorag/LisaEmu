# Apple Lisa Hardware Specifications

Derived from analysis of the Lisa OS 3.1 source code (`Lisa_Source/`).

## CPU

- **Processor**: Motorola MC68000
- **Clock**: 5 MHz (Lisa 2); 7.8336/2 = 3.9168 MHz (Lisa 1)
- **Data bus**: 16-bit
- **Address bus**: 24-bit (16 MB addressable)
- **Registers**: D0-D7 (data), A0-A7 (address, A7 = stack pointer)
- **Modes**: Supervisor and User mode
- **Interrupts**: 7 priority levels (autovectored)

## Memory Map

```
$000000 - $0FFFFF   RAM (1 MB, expandable)
$FC0000 - $FCFFFF   I/O Space (64 KB)
$FE0000 - $FEFFFF   Boot ROM (16 KB, mirrored)
```

### I/O Space Detail ($FC0000 base)

```
Offset      Component                   Notes
──────      ─────────                   ─────
$0000       Disk shared memory          Floppy controller communication
$D01C       Contrast Latch              Display contrast (write)
$D801       VIA1 Port B                 ProFile hard disk interface
$D803       VIA1 Port A                 ProFile data bus
$D805       VIA1 DDRB                   Port B direction
$D807       VIA1 DDRA                   Port A direction
$D809       VIA1 T1C-L                  Timer 1 counter low
$D80B       VIA1 T1C-H                  Timer 1 counter high
$D80D       VIA1 T1L-L                  Timer 1 latch low
$D80F       VIA1 T1L-H                  Timer 1 latch high
$D811       VIA1 T2C-L                  Timer 2 counter low
$D813       VIA1 T2C-H                  Timer 2 counter high
$D815       VIA1 SR                     Shift register (speaker)
$D817       VIA1 ACR                    Auxiliary control
$D819       VIA1 PCR                    Peripheral control
$D81B       VIA1 IFR                    Interrupt flag
$D81D       VIA1 IER                    Interrupt enable
$D81F       VIA1 ORA-NH                 Port A (no handshake)
$DD81       VIA2 Port B                 COPS/keyboard interface
$DD83       VIA2 Port A                 COPS data
$DD85-$DD9F VIA2 registers              (same layout as VIA1)
$E010       Setup bit set               Enter setup mode (ROM at $0)
$E012       Setup bit reset             Exit setup mode
$E018       Vertical retrace            Clear VBL interrupt
$E800       Video page latch            Select primary/alternate screen
$F800       Hardware status register    System status bits
```

### VIA Register Spacing

VIA registers are at **odd byte addresses**, spaced 2 bytes apart. Register N is at base + (N * 2).

## Display

- **Resolution**: 720 x 364 pixels
- **Type**: Monochrome bitmap (1 bit per pixel)
- **Framebuffer size**: 32,760 bytes (720/8 × 364)
- **Bit order**: MSB = leftmost pixel; 1 = black, 0 = white
- **Dual page**: Primary screen + alternate screen (page-flippable via $FCE800)
- **Primary base**: $7A000 in RAM
- **Alternate base**: $7A000 + 32,760
- **Contrast**: Hardware-controlled via latch at $FCD01C (0-255)
- **Phosphor**: White P4 phosphor (paper-white display)
- **Refresh**: ~60 Hz with vertical retrace interrupt

## VIA 6522 Chips

### VIA1 — Parallel Port / ProFile Hard Disk

- **Base**: $FCD801
- **IRQ**: Level 1 autovector
- **Port A**: ProFile data bus (bidirectional 8-bit)
- **Port B**: ProFile control signals
  - PB0: OCD (device connected)
  - PB1: BSY (device busy)
  - PB2-PB7: Command/status lines
- **Timer 1**: System millisecond timer (free-running, 20ms resolution)
- **Shift Register**: Speaker waveform generation

### VIA2 — Keyboard / COPS Microcontroller

- **Base**: $FCDD81
- **IRQ**: Level 2 autovector
- **Port A**: COPS data (keyboard scancodes, mouse deltas, clock)
- **Port B**: COPS control signals
  - PB0: Data available from COPS
  - PB2: Mouse button state
- **CA1**: COPS data ready interrupt
- **Timer 1**: Keyboard repeat timer

## COPS Microcontroller

The COPS (Processor for Operations and Systems) is a dedicated microcontroller that manages:

- **Keyboard**: 128 key positions (0-127)
  - Key down: scancode with bit 7 clear
  - Key up: scancode with bit 7 set
  - Event queue: 32 entries (standard) + 16 (alternate)
- **Mouse**: Delta tracking
  - Sends 2-byte packets: signed dx, signed dy
  - Configurable scaling and thresholds
  - Odometer for total movement
- **Real-time clock**: Date/time with battery backup
- **Soft power**: Power on/off control
- **Speaker**: Waveform generation via VIA1 shift register

## Keyboard

- **128 key positions** (keycap codes 0-127)
- **Multiple layouts**: US QWERTY, Dvorak, international variants
- **ASCII translation**: 768-byte table (3 tables × 256 entries: normal, shift, option)
- **Modifier tracking**: Shift, Option, Apple (Command) keys tracked in bitmap
- **Auto-repeat**: Configurable initial delay and repeat rate

## Mouse

- **Interface**: Via COPS microcontroller
- **Tracking**: Relative deltas (signed 8-bit dx, dy per packet)
- **Button**: Single button, active low on VIA2 PB2
- **Cursor**: 16x16 pixel hardware cursor with mask and hotspot
- **Scaling**: Software-configurable movement thresholds

## Storage

### ProFile Hard Disk
- **Capacity**: 5 MB (9,728 blocks) or 10 MB (19,456 blocks)
- **Block size**: 532 bytes (512 data + 20 tag bytes)
- **Interface**: Parallel via VIA1
- **Protocol**: Command/status handshake through Port B, data through Port A
- **Commands**: Read, Write, Format, Verify

### Sony 3.5" Floppy (Lisa 2)
- **Capacity**: 400 KB (single-sided) or 800 KB (double-sided)
- **Variable speed**: Different RPM per track zone
- **Format**: GCR encoding, 80 tracks
- **Interface**: IWM (Integrated Woz Machine) chip

### Twiggy Drive (Lisa 1)
- **Capacity**: ~871 KB per disk
- **Type**: Proprietary Apple dual-sided 5.25" floppy
- **Format**: 46 cylinders, 2 surfaces, variable sectors/track (up to 23)
- **Interface**: 6504 microprocessor in shared memory
- **Commands**: Read, Write, Format, Clamp/Unclamp, Verify

## Interrupt Architecture

```
Level   Source              Vector
─────   ──────              ──────
  1     VIA1 (ProFile)      $64 (autovector 25)
  1     Vertical retrace    $64 (shared with VIA1)
  2     VIA2 (Keyboard)     $68 (autovector 26)
  7     NMI (power button)  $7C (autovector 31)
```

### Trap Vectors
- **TRAP #5**: Hardware interface dispatcher (90+ functions via LIBHW)
- **TRAP #7**: System call interface
- **Line-A ($Axxx)**: Lisa OS traps
- **Line-F ($Fxxx)**: Floating point coprocessor emulation

## Audio

- **Speaker**: Single piezo speaker
- **Generation**: Via VIA1 shift register (programmable waveform)
- **Volume**: Software-controlled (0-15 levels)
- **Capabilities**: Tone generation, beep, noise, silence

## MMU (Memory Management Unit)

- **Custom Lisa MMU** (not the 68451)
- **Segments**: 128 per context
- **Contexts**: 4 (domains)
- **Segment size**: 128 KB max
- **Registers per segment**: SLR (Segment Limit Register) + SOR (Segment Origin Register)
- **Setup mode**: When asserted, ROM appears at address $000000 for boot
- **Controlled via**: I/O space writes at $FCE010 (set) / $FCE012 (clear)

## Timing

- **CPU clock**: 5 MHz → 200 ns per cycle
- **VIA Timer 1**: Configurable, typically 20 ms period for system tick
- **Microsecond timer**: VIA Timer 2 or cycle counting
- **Alarm system**: Up to 20 independent software alarms
- **Vertical retrace**: ~60 Hz (16.67 ms per frame)
- **Frame cycles**: 5,000,000 / 60 ≈ 83,333 cycles per frame
