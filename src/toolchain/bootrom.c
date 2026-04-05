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
     * with specific handlers for Line-A (10) and Line-F (11) */
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
     * Default exception handler at $FE0300: just RTE
     * ================================================================ */
    b.pc = 0x0300;
    emit16(&b, 0x4E73);  /* RTE */

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

    /* 3. Clear display to white (bit 0 = white on Lisa) */
    emit16(&b, 0x207C);          /* MOVEA.L #$7A000,A0 */
    emit32(&b, 0x0007A000);
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

    /* 5. Switch MMU to normal mode: MOVE.B #0,$FCE012 */
    emit16(&b, 0x13FC);
    emit16(&b, 0x0000);
    emit32(&b, 0x00FCE012);

    /* 6. Set A5 and A6 — PASCALINIT expects A5 in user stack area.
     * A6 must be valid (frame pointer) before STARTUP's LINK A6. */
    emit16(&b, 0x2A7C);          /* MOVEA.L #imm,A5 */
    emit32(&b, 0x00014000);
    emit16(&b, 0x2C7C);          /* MOVEA.L #imm,A6 */
    emit32(&b, 0x00079000);      /* A6 = top of stack (valid frame ptr sentinel) */

    /* 7. Check boot code at $400 (linker starts code after vector table) */
    emit16(&b, 0x2039);          /* MOVE.L ($400).L,D0 */
    emit32(&b, 0x00000400);
    emit16(&b, 0x4A80);          /* TST.L D0 */
    emit16(&b, 0x6706);          /* BEQ.S no_boot (+6 = skip JMP) */

    /* 8. Jump to OS code at $400 */
    emit16(&b, 0x4EF9);          /* JMP ($400).L */
    emit32(&b, 0x00000400);

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

    printf("Boot ROM generated: %u bytes used of %u\n", b.pc, ROM_SIZE);
    uint32_t ssp = ((uint32_t)rom[0]<<24)|((uint32_t)rom[1]<<16)|((uint32_t)rom[2]<<8)|rom[3];
    uint32_t pc = ((uint32_t)rom[4]<<24)|((uint32_t)rom[5]<<16)|((uint32_t)rom[6]<<8)|rom[7];
    printf("  Vector 0 (SSP): $%08X\n", ssp);
    printf("  Vector 1 (PC):  $%08X\n", pc);
    printf("  First code byte at $0400: $%02X\n", rom[0x400]);
    return rom;
}
