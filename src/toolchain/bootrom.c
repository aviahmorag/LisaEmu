/*
 * LisaEm Toolchain — Boot ROM Synthesizer
 *
 * Generates a minimal boot ROM based on Lisa hardware specs.
 *
 * Boot sequence:
 *   1. CPU reads initial SSP from $FE0000 and initial PC from $FE0004
 *   2. Disable interrupts, set up supervisor stack
 *   3. Clear display to white, set contrast
 *   4. Switch MMU from setup mode to normal mode
 *   5. Jump to boot code pre-loaded at $20000
 *
 * The emulator pre-loads boot track (blocks 0-23) into RAM at $20000
 * during lisa_reset(), so no ProFile I/O is needed in the ROM.
 *
 * Hardware addresses:
 *   $FCD01C - Video contrast latch
 *   $FCE012 - MMU setup mode reset (write clears setup mode)
 *   $FE0000 - ROM (16KB)
 */

#include "bootrom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Big-endian helpers */
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

typedef struct {
    uint8_t *rom;
    uint32_t pc;
} rom_builder_t;

static void emit16(rom_builder_t *b, uint16_t v) {
    if (b->pc + 1 < ROM_SIZE) {
        put16(b->rom + b->pc, v);
    }
    b->pc += 2;
}

static void emit32(rom_builder_t *b, uint32_t v) {
    if (b->pc + 3 < ROM_SIZE) {
        put32(b->rom + b->pc, v);
    }
    b->pc += 4;
}

uint8_t *bootrom_generate(void) {
    uint8_t *rom = calloc(1, ROM_SIZE);
    if (!rom) return NULL;

    rom_builder_t b = { .rom = rom, .pc = 0 };

    /* ================================================================
     * Exception vector table (at ROM base = $FE0000)
     * In setup mode, ROM appears at $000000.
     * ================================================================ */

    /* Vector 0: Initial SSP */
    emit32(&b, 0x000FFFFE);

    /* Vector 1: Initial PC — ROM code at $FE0400 */
    emit32(&b, 0x00FE0400);

    /* Vectors 2-63: default → RTE handler at $FE0300,
     * with specific handlers for Line-A (10) and Line-F (11).
     *
     * IMPORTANT: Certain PROM entry points overlap with vector positions:
     *   $FE0084 (vector 33) = prom_monitor
     *   $FE0090 (vector 36) = prof_entry (ProFile read)
     *   $FE0094 (vector 37) = twig_entry (Twiggy read)
     *   $FE00BC (vector 47) = checksum routine
     * These positions contain JMP instructions to actual code, which
     * also serve as valid vector addresses during setup mode. */
    for (int i = 2; i < 64; i++) {
        if (i == 10) {
            emit32(&b, 0x00FE0320);  /* Line-A → skip handler */
        } else if (i == 11) {
            emit32(&b, 0x00FE0310);  /* Line-F → SANE skip handler */
        } else {
            emit32(&b, 0x00FE0300);  /* Default RTE */
        }
    }

    /* ================================================================
     * PROM entry points — placed at real PROM addresses.
     * These overlap with vector table entries but that's OK: after
     * startup, vectors come from RAM. The boot track (LDRLDR) calls
     * these as subroutines via JSR/JMP.
     * ================================================================ */

    /* $FE0084: prom_monitor — reenter PROM / halt */
    b.pc = 0x0084;
    emit16(&b, 0x4E71);          /* NOP */
    emit16(&b, 0x60FC);          /* BRA.S self (halt forever) */

    /* $FE0090: prof_entry — ProFile block read.
     * Just RTS — the LDRLDR will get empty/zero data but the boot
     * process won't crash. Eventually we need a real implementation. */
    b.pc = 0x0090;
    emit16(&b, 0x4E75);          /* RTS (2 bytes: $90-$91) */
    /* pad */
    emit16(&b, 0x4E71);          /* NOP (2 bytes: $92-$93) */

    /* $FE0094: twig_entry — Twiggy/Sony block read (stub) */
    b.pc = 0x0094;
    emit16(&b, 0x4E75);          /* RTS */

    /* ================================================================
     * PROM checksum routine at $FE00BC
     * Called by VERIFY_CKSUM (source-STARASM2.TEXT):
     *   A0 = address of parameter memory image
     *   D0 = word count - 1 (typically 31 for 32 words = 64 bytes)
     *   D1 = 0 (memory image) or 1 (shared memory)
     * Returns:
     *   D3 = computed checksum (0 if valid)
     *
     * Algorithm: XOR all 16-bit words. If the checksum field (last word)
     * is set correctly, XOR of all words including checksum = 0.
     * ================================================================ */
    b.pc = 0x00BC;
    /* CLR.L D3 */
    emit16(&b, 0x4283);
    /* loop: MOVE.W (A0)+,D2 */
    uint32_t cksum_loop = b.pc;
    emit16(&b, 0x3418);         /* MOVE.W (A0)+,D2 */
    /* EOR.W D2,D3 */
    emit16(&b, 0xB543);         /* EOR.W D2,D3 */
    /* DBRA D0,loop */
    emit16(&b, 0x51C8);
    emit16(&b, (uint16_t)((int16_t)(cksum_loop - b.pc)));
    /* RTS */
    emit16(&b, 0x4E75);

    /* ================================================================
     * Default exception handler at $FE0300: just RTE
     * ================================================================ */
    b.pc = 0x0300;
    emit16(&b, 0x4E73);  /* RTE */

    /* ================================================================
     * TRAP #5 handler (TRAPTOHW) at $FE0330:
     * Handles hardware interface calls during early boot.
     * Function number is in D7.
     * ScreenAddr ($18) / AltScreenAddr ($1A): return screen base in A0.
     * CursorDisplay ($14) / CursorHide ($10): no-op.
     * ================================================================ */
    b.pc = 0x0330;
    /* CMP.W #$18,D7 — is it ScreenAddr? */
    emit16(&b, 0x0C47);          /* CMPI.W #imm,D7 */
    emit16(&b, 0x0018);
    emit16(&b, 0x6706);          /* BEQ.S +6 → load screen addr */
    /* CMP.W #$1A,D7 — is it AltScreenAddr? */
    emit16(&b, 0x0C47);
    emit16(&b, 0x001A);
    emit16(&b, 0x6604);          /* BNE.S +4 → just RTE (cursor ops etc.) */
    /* Load screen address into A0.
     * Screen base is at low memory $1F8 (ALTMSBASE) or $1F4 (MSBASE).
     * For simplicity, load from $110 (prom_screen) which we initialize. */
    emit16(&b, 0x2079);          /* MOVEA.L ($110).L,A0 */
    emit32(&b, 0x00000110);
    emit16(&b, 0x4E73);          /* RTE */

    /* ================================================================
     * Line-F (SANE FP) handler at $FE0310:
     * The 68000 pushes PC of the Line-F opcode on the stack.
     * Skip past it (add 2 to stacked PC) and return.
     * Stack frame: (SP)=SR, 2(SP)=PC.L
     * ================================================================ */
    b.pc = 0x0310;
    /* ADDQ.L #2, 2(A7) — add 2 to the stacked PC
     * ADDQ: 0101 qqq 1 ss EEEEEE, qqq=010(#2), ss=10(long), EA=101 111(d16,A7)
     * = 0101 010 1 10 101111 = $55AF, followed by displacement word $0002 */
    emit16(&b, 0x54AF);
    emit16(&b, 0x0002);
    emit16(&b, 0x4E73);          /* RTE */

    /* ================================================================
     * Line-A handler at $FE0320: same as Line-F — skip the opcode
     * ================================================================ */
    b.pc = 0x0320;
    emit16(&b, 0x54AF);
    emit16(&b, 0x0002);
    emit16(&b, 0x4E73);          /* RTE */

    /* ================================================================
     * Main boot code at $FE0400
     * ================================================================ */
    b.pc = 0x0400;

    /* 1. Set supervisor stack */
    emit16(&b, 0x2E7C);          /* MOVEA.L #imm,SP */
    emit32(&b, 0x00079000);

    /* 2. Disable interrupts */
    emit16(&b, 0x46FC);          /* MOVE #$2700,SR */
    emit16(&b, 0x2700);

    /* 3. Clear display to white — screen at top of 2MB RAM ($1F8000) */
    emit16(&b, 0x207C);          /* MOVEA.L #$1F8000,A0 */
    emit32(&b, 0x001F8000);
    emit16(&b, 0x303C);          /* MOVE.W #8189,D0 */
    emit16(&b, 0x1FFD);
    uint32_t clear_loop = b.pc;
    emit16(&b, 0x20FC);          /* MOVE.L #0,(A0)+ */
    emit32(&b, 0x00000000);
    emit16(&b, 0x51C8);          /* DBRA D0,clear_loop */
    emit16(&b, (uint16_t)((int16_t)(clear_loop - b.pc)));

    /* 4. Set contrast to max: MOVE.B #$FF,$FCD01C */
    emit16(&b, 0x13FC);
    emit16(&b, 0x00FF);
    emit32(&b, 0x00FCD01C);

    /* 5. Program MMU for identity mapping before exiting setup mode.
     * The real Lisa boot loader does this; since we skip the loader,
     * we program a basic identity map here.
     *
     * MMU registers accessed during setup mode:
     *   SLIM (Segment Limit) at physical addr = $8000 + seg * $20000
     *   SORG (Segment Origin) at physical addr = $8008 + seg * $20000
     *
     * SLR value $0700 = RW memory, length covers full segment
     * SOR value = physical page origin (seg * 256 pages, each 512 bytes)
     *
     * We map segments 0-7 for the first 1MB of physical RAM. */

    /* Turn setup ON explicitly (should already be on, but be safe) */
    emit16(&b, 0x13FC);          /* MOVE.B #0,$FCE010 (setup on) */
    emit16(&b, 0x0000);
    emit32(&b, 0x00FCE010);

    /* Set context bits to context 1 (segment1=0, segment2=0 → ctx=1) */
    emit16(&b, 0x13FC);          /* MOVE.B #0,$FCE008 (seg1=0) */
    emit16(&b, 0x0000);
    emit32(&b, 0x00FCE008);
    emit16(&b, 0x13FC);          /* MOVE.B #0,$FCE00C (seg2=0) */
    emit16(&b, 0x0000);
    emit32(&b, 0x00FCE00C);

    /* Program 16 segments for identity mapping (2MB — full RAM) */
    /* Segment 0: SLIM=$0700 at $008000, SORG=$0000 at $008008 */
    for (int seg = 0; seg < 16; seg++) {
        uint32_t slim_addr = 0x008000 + (uint32_t)seg * 0x20000;
        uint32_t sorg_addr = 0x008008 + (uint32_t)seg * 0x20000;
        uint16_t sor_val = (uint16_t)(seg * 256);  /* seg * 128KB / 512 = seg * 256 pages */

        /* MOVE.W #$0700, (slim_addr).L — SLR = RW mem, full segment */
        emit16(&b, 0x33FC);      /* MOVE.W #imm,abs.L */
        emit16(&b, 0x0700);      /* SLR value: RW memory */
        emit32(&b, slim_addr);

        /* MOVE.W #sor_val, (sorg_addr).L — SOR = identity page origin */
        emit16(&b, 0x33FC);      /* MOVE.W #imm,abs.L */
        emit16(&b, sor_val);
        emit32(&b, sorg_addr);
    }

    /* Also map I/O segment: segment 126 ($FC0000 >> 17 = 126) */
    {
        uint32_t slim_addr = 0x008000 + 126U * 0x20000;
        uint32_t sorg_addr = 0x008008 + 126U * 0x20000;
        emit16(&b, 0x33FC);
        emit16(&b, 0x0900);      /* SLR = I/O space */
        emit32(&b, slim_addr);
        emit16(&b, 0x33FC);
        emit16(&b, (uint16_t)(126 * 256));  /* SOR for $FC0000 */
        emit32(&b, sorg_addr);
    }

    /* Map ROM segment: segment 127 ($FE0000 >> 17 = 127) */
    {
        uint32_t slim_addr = 0x008000 + 127U * 0x20000;
        uint32_t sorg_addr = 0x008008 + 127U * 0x20000;
        emit16(&b, 0x33FC);
        emit16(&b, 0x0F00);      /* SLR = SIO space (ROM) */
        emit32(&b, slim_addr);
        emit16(&b, 0x33FC);
        emit16(&b, (uint16_t)(127 * 256));  /* SOR for $FE0000 */
        emit32(&b, sorg_addr);
    }

    /* Now exit setup mode: MOVE.B #0,$FCE012 */
    emit16(&b, 0x13FC);
    emit16(&b, 0x0000);
    emit32(&b, 0x00FCE012);

    /* 6. Set A5 and A6 — PASCALINIT expects A5 in user area with
     * 32 bytes of Pascal runtime data below it (IORESULT, console ptrs).
     * These are all zeros for initial boot. A5 points to end of this area.
     * Use $70000 = top of user stack region (well above OS code). */
    emit16(&b, 0x2A7C);          /* MOVEA.L #imm,A5 */
    emit32(&b, 0x00070000);
    emit16(&b, 0x2C7C);          /* MOVEA.L #imm,A6 */
    emit32(&b, 0x00079000);      /* A6 = top of stack (valid frame ptr sentinel) */

    /* 7. Check boot code at $400 (linker starts code after vector table) */
    emit16(&b, 0x2039);          /* MOVE.L ($400).L,D0 */
    emit32(&b, 0x00000400);
    emit16(&b, 0x4A80);          /* TST.L D0 */
    emit16(&b, 0x670A);          /* BEQ.S try_boot_track (+10) */

    /* 8. Jump to OS code at $400 (cross-compiled image path) */
    emit16(&b, 0x4EF9);          /* JMP ($400).L */
    emit32(&b, 0x00000400);

    /* try_boot_track: $400 was zero, check if boot track is loaded at $20000 */
    emit16(&b, 0x2039);          /* MOVE.L ($20000).L,D0 */
    emit32(&b, 0x00020000);
    emit16(&b, 0x4A80);          /* TST.L D0 */
    emit16(&b, 0x6706);          /* BEQ.S no_boot (+6 = skip JMP) */

    /* Jump to boot track at $20000 (real Lisa OS image path) */
    emit16(&b, 0x4EF9);          /* JMP ($20000).L */
    emit32(&b, 0x00020000);

    /* no_boot: Draw diagonal error indicator and halt */
    emit16(&b, 0x207C);          /* MOVEA.L #$7A000,A0 */
    emit32(&b, 0x0007A000);
    emit16(&b, 0x303C);          /* MOVE.W #363,D0 */
    emit16(&b, 0x016B);
    uint32_t diag_loop = b.pc;
    emit16(&b, 0x10FC);          /* MOVE.B #$FF,(A0)+ */
    emit16(&b, 0x00FF);
    emit16(&b, 0xD0FC);          /* ADDA.W #89,A0 */
    emit16(&b, 0x0059);
    emit16(&b, 0x51C8);          /* DBRA D0,diag_loop */
    emit16(&b, (uint16_t)((int16_t)(diag_loop - b.pc)));

    emit16(&b, 0x4E71);          /* NOP */
    emit16(&b, 0x60FC);          /* BRA.S self (halt) */

    /* ================================================================
     * prof_entry at $FE0090 — ProFile block read for boot loader.
     * The LDRLDR boot track calls this to read sectors.
     * Interface (from LisaEm romless_proread):
     *   D1 = sector number (with 5:1 interleave encoding)
     *   A1 = tag destination (20 bytes)
     *   A2 = data destination (512 bytes)
     * Returns: carry clear = success
     *
     * We emit a NOP + RTS here. The emulator intercepts PC=$FE0090
     * and performs the read directly from the mounted disk image.
     * ================================================================ */
    b.pc = 0x0090;
    emit16(&b, 0x4E71);          /* NOP — intercepted by emulator */
    emit16(&b, 0x4E75);          /* RTS */

    /* twig_entry at $FE0094 — floppy read (stub) */
    b.pc = 0x0094;
    emit16(&b, 0x4E71);
    emit16(&b, 0x4E75);

    /* prom_monitor at $FE0084 — halt loop */
    b.pc = 0x0084;
    emit16(&b, 0x60FE);          /* BRA.S self (infinite loop) */

    /* ================================================================
     * Loader stub at $FE0600 — replacement for the boot loader's
     * LDRTRAP/DRIVER_CALL interface.
     *
     * ENTER_LOADER (source-STARASM2.TEXT) calls through loader_link ($204).
     * It passes D2 = pointer to fake_parms struct.
     * The original loader's LDRTRAP pushes D2 and calls DRIVER_CALL.
     *
     * Our stub writes D2 to the special I/O port $FCC100-$FCC103.
     * The emulator's io_write_cb intercepts this and processes the
     * loader call directly (reading from the ProFile disk image).
     * ================================================================ */
    b.pc = 0x0600;

    /* MOVE.L D2,$FCC100 — write fake_parms pointer to loader trap port.
     * This is a MOVE.L Dn,(xxx).L instruction.
     * Encoding: 0010 0011 1100 0010 = $23C2, followed by 32-bit address. */
    emit16(&b, 0x23C2);          /* MOVE.L D2,abs.L */
    emit32(&b, 0x00FCC100);      /* $FCC100 — loader trap port */

    emit16(&b, 0x4E75);          /* RTS — return to ENTER_LOADER */

    printf("Boot ROM generated: %u bytes used of %u\n", b.pc, ROM_SIZE);
    uint32_t ssp = ((uint32_t)rom[0]<<24)|((uint32_t)rom[1]<<16)|((uint32_t)rom[2]<<8)|rom[3];
    uint32_t pc = ((uint32_t)rom[4]<<24)|((uint32_t)rom[5]<<16)|((uint32_t)rom[6]<<8)|rom[7];
    printf("  Vector 0 (SSP): $%08X\n", ssp);
    printf("  Vector 1 (PC):  $%08X\n", pc);
    printf("  First code byte at $0400: $%02X\n", rom[0x400]);
    return rom;
}
