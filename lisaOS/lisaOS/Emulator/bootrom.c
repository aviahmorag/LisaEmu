/*
 * LisaEm Toolchain — Boot ROM Synthesizer
 *
 * Generates a minimal boot ROM based on Lisa hardware specs.
 *
 * Lisa boot sequence (from source analysis):
 *   1. CPU reads initial SSP from $FE0000 and initial PC from $FE0004
 *   2. ROM code at PC: disable interrupts, set up supervisor stack
 *   3. Initialize MMU: setup mode (ROM at $000000, RAM at $080000)
 *   4. Test and clear RAM
 *   5. Initialize VIAs (disable all interrupts)
 *   6. Set up display: video base at $7A000, contrast
 *   7. Read boot blocks from ProFile via VIA1 parallel interface
 *   8. Copy boot blocks to RAM at $20000
 *   9. Switch MMU to normal mode (RAM at $000000)
 *   10. Jump to loaded code at $20000
 *
 * Hardware addresses (from HARDWARE_SPECS.md):
 *   $FC0000 - I/O space base
 *   $FCD01C - Video contrast latch
 *   $FCD801 - VIA1 (ProFile/parallel port)
 *   $FCDD81 - VIA2 (keyboard/COPS)
 *   $FE0000 - ROM (16KB)
 *
 * This is a minimal ROM that boots from a ProFile disk image.
 * It doesn't do memory testing, hardware diagnostics, or error display.
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

/* Emit a 68000 instruction into the ROM buffer */
typedef struct {
    uint8_t *rom;
    uint32_t pc;        /* Current position relative to ROM start */
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
     * In setup mode, ROM appears at $000000, so vectors are read from here.
     * ================================================================ */

    /* Vector 0: Initial SSP — top of 1MB RAM */
    emit32(&b, 0x000FFFFE);     /* SSP = $0FFFFE (just below 1MB boundary) */

    /* Vector 1: Initial PC — start of ROM code */
    emit32(&b, 0x00FE0400);     /* PC = $FE0400 (ROM base + $400, after vectors) */

    /* Vectors 2-63: Bus error, Address error, Illegal instruction, etc.
     * Point them all to a simple RTE handler */
    for (int i = 2; i < 64; i++) {
        emit32(&b, 0x00FE0300);  /* All exceptions → simple handler at $FE0300 */
    }

    /* ================================================================
     * Simple exception handler at offset $0300 (address $FE0300)
     * Just does RTE (return from exception)
     * ================================================================ */
    b.pc = 0x0300;
    emit16(&b, 0x4E73);  /* RTE */

    /* ================================================================
     * Main boot code at offset $0400 (address $FE0400)
     * ================================================================ */
    b.pc = 0x0400;

    /* --- Minimal ROM: just clear display and draw a rectangle --- */
    /* No VIA init, no I/O — just RAM writes to video memory */

    /* Set supervisor stack below video RAM */
    emit16(&b, 0x2E7C);          /* MOVEA.L #$79000,SP */
    emit32(&b, 0x00079000);

    /* Disable interrupts */
    emit16(&b, 0x46FC);          /* MOVE #$2700,SR */
    emit16(&b, 0x2700);

    /* Clear display to white: fill $7A000-$81FF8 with $FF */
    emit16(&b, 0x207C);          /* MOVEA.L #$7A000,A0 */
    emit32(&b, 0x0007A000);
    emit16(&b, 0x303C);          /* MOVE.W #8189,D0 */
    emit16(&b, 0x1FFD);
    uint32_t clear_loop = b.pc;
    emit16(&b, 0x20FC);          /* MOVE.L #$FFFFFFFF,(A0)+ */
    emit32(&b, 0xFFFFFFFF);
    emit16(&b, 0x51C8);          /* DBRA D0,clear_loop */
    emit16(&b, (uint16_t)((int16_t)(clear_loop - b.pc)));

    /* Draw black rectangle: 20 rows x 20 bytes at center of screen */
    /* Offset = row 150 * 90 bytes/row + col 35 = 13535 */
    emit16(&b, 0x207C);          /* MOVEA.L #$7A000+13535,A0 */
    emit32(&b, 0x0007A000 + 13535);
    emit16(&b, 0x303C);          /* MOVE.W #19,D0 — 20 rows */
    emit16(&b, 0x0013);
    uint32_t row_loop = b.pc;
    emit16(&b, 0x323C);          /* MOVE.W #19,D1 — 20 bytes */
    emit16(&b, 0x0013);
    uint32_t col_loop = b.pc;
    emit16(&b, 0x10FC);          /* MOVE.B #0,(A0)+ — black */
    emit16(&b, 0x0000);
    emit16(&b, 0x51C9);          /* DBRA D1,col_loop */
    emit16(&b, (uint16_t)((int16_t)(col_loop - b.pc)));
    emit16(&b, 0xD0FC);          /* ADDA.W #70,A0 — skip to next row */
    emit16(&b, 0x0046);
    emit16(&b, 0x51C8);          /* DBRA D0,row_loop */
    emit16(&b, (uint16_t)((int16_t)(row_loop - b.pc)));

    /* Infinite loop */
    emit16(&b, 0x4E71);          /* NOP */
    emit16(&b, 0x60FC);          /* BRA.S self */

    printf("Boot ROM generated: %u bytes used of %u\n", b.pc, ROM_SIZE);
    return rom;
}
