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
    return 0;
}
