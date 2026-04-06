/*
 * Test the 68000 assembler against actual Lisa source
 */

#include "asm68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple test: assemble a small 68000 program */
static const char *test_source =
    "; Test program\n"
    "        .PROC   TEST\n"
    "        .DEF    START\n"
    "        .DEF    LOOP\n"
    "        .REF    EXTERNAL\n"
    "\n"
    "SBIT    EQU     13\n"
    "BASE    EQU     $FC0000\n"
    "\n"
    "START   MOVE.L  #$100,A7\n"
    "        MOVEA.L #$200,A6\n"
    "        MOVEQ   #0,D0\n"
    "        CLR.L   D1\n"
    "\n"
    "LOOP    MOVE.W  (A6)+,D0\n"
    "        ADD.W   D0,D1\n"
    "        DBRA    D0,LOOP\n"
    "\n"
    "        LEA     BASE,A0\n"
    "        MOVE.B  #$FF,(A0)\n"
    "        TST.L   D0\n"
    "        BEQ.S   DONE\n"
    "        BSR     SUBROUT\n"
    "\n"
    "DONE    RTS\n"
    "\n"
    "SUBROUT LINK    A6,#-8\n"
    "        MOVEM.L D0-D3/A0-A2,-(SP)\n"
    "        MOVE.W  SR,-(SP)\n"
    "        MOVE.L  4(A6),D0\n"
    "        BTST    #SBIT,D0\n"
    "        BNE.S   RETSR\n"
    "        TRAP    #2\n"
    "RETSR   MOVEM.L (SP)+,D0-D3/A0-A2\n"
    "        UNLK    A6\n"
    "        RTS\n"
    "\n"
    "DATA    DC.W    $1234,$5678\n"
    "        DC.L    START\n"
    "        DC.B    'Hello',0\n"
    ;

int main(int argc, char *argv[]) {
    asm68k_t as;
    asm68k_init(&as);

    printf("=== LisaEm 68000 Assembler Test ===\n\n");

    if (argc > 1) {
        /* Assemble file from command line */
        printf("Assembling: %s\n", argv[1]);

        /* Set base dir for includes */
        char *slash = strrchr(argv[1], '/');
        if (slash) {
            char dir[256];
            size_t len = slash - argv[1];
            strncpy(dir, argv[1], len);
            dir[len] = '\0';
            asm68k_set_base_dir(&as, dir);
        }

        if (asm68k_assemble_file(&as, argv[1])) {
            printf("SUCCESS: %u bytes generated\n", as.output_size);
            asm68k_dump_symbols(&as);
        } else {
            printf("FAILED: %d errors\n", as.num_errors);
            for (int i = 0; i < as.num_errors; i++) {
                printf("  %s\n", asm68k_get_error(&as, i));
            }
        }
    } else {
        /* Run built-in test */
        printf("Assembling test program...\n\n");

        if (asm68k_assemble_string(&as, test_source, "test.asm")) {
            printf("SUCCESS: %u bytes generated\n\n", as.output_size);
            asm68k_dump_symbols(&as);

            /* Hex dump */
            printf("\n=== Code Output ===\n");
            const uint8_t *code = asm68k_get_output(&as, NULL);
            for (uint32_t i = 0; i < as.output_size; i++) {
                if (i % 16 == 0) printf("%04X: ", i);
                printf("%02X ", code[i]);
                if (i % 16 == 15 || i == as.output_size - 1) printf("\n");
            }
        } else {
            printf("FAILED: %d errors\n", as.num_errors);
            for (int i = 0; i < as.num_errors; i++) {
                printf("  %s\n", asm68k_get_error(&as, i));
            }
        }
    }

    asm68k_free(&as);

    /* Test MOVEM register list parsing — D0-D7/A0-A6 must not be
     * parsed as data register D0 (was a bug: '-' terminated the
     * register name check, so "D0-D7/A0-A6" matched as "D0"). */
    {
        printf("\n=== MOVEM Register List Test ===\n");
        asm68k_t as2;
        asm68k_init(&as2);

        const char *movem_test =
            "        .PROC   MOVEMTEST\n"
            "ENTRY   MOVEM.L D0-D7/A0-A6,-(SP)\n"
            "        NOP\n"
            "        MOVEM.L (SP)+,D0-D7/A0-A6\n"
            "        RTS\n";

        if (asm68k_assemble_string(&as2, movem_test, "movem_test.asm")) {
            const uint8_t *code = asm68k_get_output(&as2, NULL);
            /* MOVEM.L D0-D7/A0-A6,-(SP) should be $48E7 $FFFE */
            uint16_t op = (code[0] << 8) | code[1];
            uint16_t mask = (code[2] << 8) | code[3];
            if (op == 0x48E7 && mask == 0xFFFE) {
                printf("PASS: MOVEM.L D0-D7/A0-A6,-(SP) = $%04X $%04X\n", op, mask);
            } else {
                printf("FAIL: MOVEM.L D0-D7/A0-A6,-(SP) = $%04X $%04X (expected $48E7 $FFFE)\n", op, mask);
            }

            /* MOVEM.L (SP)+,D0-D7/A0-A6 should be $4CDF $7FFF */
            uint16_t op2 = (code[6] << 8) | code[7];  /* after NOP at +4 */
            uint16_t mask2 = (code[8] << 8) | code[9];
            if (op2 == 0x4CDF && mask2 == 0x7FFF) {
                printf("PASS: MOVEM.L (SP)+,D0-D7/A0-A6 = $%04X $%04X\n", op2, mask2);
            } else {
                printf("FAIL: MOVEM.L (SP)+,D0-D7/A0-A6 = $%04X $%04X (expected $4CDF $7FFF)\n", op2, mask2);
            }
        } else {
            printf("FAIL: assembly failed with %d errors\n", as2.num_errors);
        }
        asm68k_free(&as2);
    }

    /* Test @-label scoping — @3 in different scopes must resolve
     * to different addresses. */
    {
        printf("\n=== @-Label Scoping Test ===\n");
        asm68k_t as3;
        asm68k_init(&as3);

        const char *scope_test =
            "        .PROC   FUNC_A\n"
            "        BEQ.S   @3\n"      /* forward ref to @3 in FUNC_A scope */
            "        NOP\n"
            "@3      NOP\n"             /* @3 at offset ~4 */
            "        RTS\n"
            "\n"
            "        .PROC   FUNC_B\n"
            "        BEQ.S   @3\n"      /* forward ref to @3 in FUNC_B scope */
            "        NOP\n"
            "        NOP\n"
            "        NOP\n"
            "@3      NOP\n"             /* @3 at offset ~8 (different from FUNC_A's @3) */
            "        RTS\n";

        if (asm68k_assemble_string(&as3, scope_test, "scope_test.asm")) {
            const uint8_t *code = asm68k_get_output(&as3, NULL);
            /* FUNC_A: BEQ.S @3 at offset 0, displacement should be 2 (skip NOP) */
            uint8_t disp_a = code[1];
            /* FUNC_B starts after FUNC_A (2+2+2+2 = 8 bytes) */
            /* FUNC_B: BEQ.S @3 at offset 8, displacement should be 6 (skip 3 NOPs) */
            uint8_t disp_b = code[8+1];

            if (disp_a == 2 && disp_b == 6) {
                printf("PASS: @3 in FUNC_A: disp=%d, @3 in FUNC_B: disp=%d (different scopes)\n",
                       disp_a, disp_b);
            } else if (disp_a == disp_b) {
                printf("FAIL: @3 resolved to same displacement in both scopes (%d)\n", disp_a);
            } else {
                printf("INFO: @3 in FUNC_A: disp=%d, @3 in FUNC_B: disp=%d\n", disp_a, disp_b);
            }
        } else {
            printf("FAIL: assembly failed with %d errors\n", as3.num_errors);
            for (int i = 0; i < as3.num_errors; i++) {
                printf("  %s\n", asm68k_get_error(&as3, i));
            }
        }
        asm68k_free(&as3);
    }

    return 0;
}
