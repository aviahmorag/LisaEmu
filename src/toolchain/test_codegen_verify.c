#include "pascal_parser.h"
#include "pascal_codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int verify(const char *name, const char *src) {
    parser_t p;
    parser_init(&p, src, name);
    ast_node_t *ast = parser_parse(&p);
    if (!ast || p.num_errors > 0) { printf("FAIL %s: parse\n", name); parser_free(&p); return 1; }

    codegen_t *cg = calloc(1, sizeof(codegen_t));
    codegen_init(cg);
    strncpy(cg->current_file, name, sizeof(cg->current_file) - 1);
    codegen_generate(cg, ast);
    
    printf("\n--- %s (%d bytes, %d errors) ---\n", name, cg->code_size, cg->num_errors);
    if (cg->num_errors > 0) { 
        for (int i = 0; i < cg->num_errors; i++) printf("  ERR: %s\n", cg->errors[i]);
    }
    
    /* Disassemble and check */
    int errors = 0;
    for (int off = 0; off < (int)cg->code_size; ) {
        uint16_t op = (cg->code[off] << 8) | cg->code[off+1];
        int sz = 2;
        
        if ((op & 0xFFF8) == 0x4E50) { /* LINK */
            int16_t disp = (cg->code[off+2] << 8) | cg->code[off+3];
            printf("  %04X: LINK A%d,#%d\n", off, op&7, disp);
            sz = 4;
        } else if ((op & 0xFFF8) == 0x4E58) {
            printf("  %04X: UNLK A%d\n", off, op&7);
        } else if (op == 0x4E75) {
            printf("  %04X: RTS\n", off);
        } else if (op == 0xC2FC) { /* MULU #imm,D1 */
            uint16_t imm = (cg->code[off+2] << 8) | cg->code[off+3];
            printf("  %04X: MULU #%d,D1", off, imm);
            if (imm == 0) { printf(" *** ZERO ELEMENT SIZE ***"); errors++; }
            printf("\n");
            sz = 4;
        } else if (op == 0xD0FC) { /* ADDA.W #imm,A0 */
            int16_t imm = (cg->code[off+2] << 8) | cg->code[off+3];
            printf("  %04X: ADDA.W #%d,A0  (field offset)\n", off, imm);
            sz = 4;
        } else if (op == 0x2F00) {
            printf("  %04X: MOVE.L D0,-(SP)  push 4\n", off);
        } else if (op == 0x3F00) {
            printf("  %04X: MOVE.W D0,-(SP)  push 2\n", off);
        } else if (op == 0x2010) {
            printf("  %04X: MOVE.L (A0),D0\n", off);
        } else if (op == 0x3010) {
            printf("  %04X: MOVE.W (A0),D0\n", off);
        } else if (op == 0x2080) {
            printf("  %04X: MOVE.L D0,(A0)\n", off);
        } else if (op == 0x3080) {
            printf("  %04X: MOVE.W D0,(A0)\n", off);
        }
        /* Skip non-interesting instructions */
        else if (off + 3 < (int)cg->code_size && (op == 0x302E || op == 0x202E || op == 0x3D40 || op == 0x2D40 || op == 0x206E || op == 0x41EE)) {
            int16_t d = (cg->code[off+2] << 8) | cg->code[off+3];
            const char *mn = op==0x302E ? "MOVE.W" : op==0x202E ? "MOVE.L" : op==0x3D40 ? "MOVE.W" : op==0x2D40 ? "MOVE.L" : op==0x206E ? "MOVEA.L" : "LEA";
            const char *dir = (op==0x3D40||op==0x2D40) ? "D0," : "";
            printf("  %04X: %s %s%d(A6)%s\n", off, mn, dir, d, (op==0x3D40||op==0x2D40) ? "" : ",D0/A0");
            sz = 4;
        }
        else { sz = 2; } /* skip silently */
        
        off += sz;
    }
    
    printf("  Errors: %d\n", errors);
    codegen_free(cg);
    free(cg);
    parser_free(&p);
    return errors;
}

int main(void) {
    int total = 0;
    
    printf("=== Codegen Verification ===\n");
    
    total += verify("record_fields",
        "unit t1; interface\n"
        "type R = record a: integer; b: longint; c: ^integer; end;\n"
        "procedure F(VAR r: R);\n"
        "implementation\n"
        "procedure F; begin r.a := 1; r.b := 2; end;\n"
        "end.\n");

    total += verify("array_ptr_elem",
        "unit t2; interface\n"
        "type PA = array[0..9] of ^integer;\n"
        "procedure G(VAR a: PA; i: integer);\n"
        "implementation\n"
        "procedure G; var p: ^integer; begin p := a[i]; end;\n"
        "end.\n");

    total += verify("longint_ops",
        "unit t3; interface\n"
        "function H(a, b: longint): longint;\n"
        "implementation\n"
        "function H; begin H := a + b; end;\n"
        "end.\n");

    printf("\n=== TOTAL ERRORS: %d ===\n", total);
    return total;
}
