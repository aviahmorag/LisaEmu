#include "pascal_parser.h"
#include "pascal_codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Decode a single instruction starting at off; return its size in bytes.
   Print a human-readable line and update *errors for obvious problems. */
static int decode_one(const uint8_t *code, int code_size, int off, int *errors,
                      int *push_total, int *pop_total) {
    if (off + 1 >= code_size) return 2;
    uint16_t op = (code[off] << 8) | code[off+1];
    int sz = 2;

    if ((op & 0xFFF8) == 0x4E50) { /* LINK */
        int16_t disp = (code[off+2] << 8) | code[off+3];
        printf("  %04X: LINK A%d,#%d\n", off, op&7, disp);
        sz = 4;
    } else if ((op & 0xFFF8) == 0x4E58) {
        printf("  %04X: UNLK A%d\n", off, op&7);
    } else if (op == 0x4E75) {
        printf("  %04X: RTS\n", off);
    } else if (op == 0x4EB9) { /* JSR abs.L */
        if (off + 5 < code_size) {
            uint32_t addr = ((uint32_t)code[off+2]<<24)|((uint32_t)code[off+3]<<16)|
                            ((uint32_t)code[off+4]<<8)|code[off+5];
            printf("  %04X: JSR $%08X\n", off, addr);
            sz = 6;
        }
    } else if (op == 0x4EB8) { /* JSR abs.W */
        uint16_t addr = (code[off+2] << 8) | code[off+3];
        printf("  %04X: JSR $%04X\n", off, addr);
        sz = 4;
    } else if (op == 0x4EBA) { /* JSR d(PC) */
        int16_t disp = (code[off+2] << 8) | code[off+3];
        printf("  %04X: JSR %d(PC)  [-> $%04X]\n", off, disp, (uint16_t)(off + 2 + disp));
        sz = 4;
    } else if (op == 0xC2FC) { /* MULU #imm,D1 */
        uint16_t imm = (code[off+2] << 8) | code[off+3];
        printf("  %04X: MULU #%d,D1", off, imm);
        if (imm == 0) { printf(" *** ZERO ELEMENT SIZE ***"); (*errors)++; }
        printf("\n");
        sz = 4;
    } else if (op == 0xD0FC) { /* ADDA.W #imm,A0 */
        int16_t imm = (code[off+2] << 8) | code[off+3];
        printf("  %04X: ADDA.W #%d,A0  (field offset)\n", off, imm);
        sz = 4;
    } else if (op == 0xD1FC) { /* ADDA.L #imm,A0 */
        if (off + 5 < code_size) {
            int32_t imm = ((int32_t)code[off+2]<<24)|((int32_t)code[off+3]<<16)|
                          ((int32_t)code[off+4]<<8)|code[off+5];
            printf("  %04X: ADDA.L #%d,A0  (field offset long)\n", off, imm);
            sz = 6;
        }
    } else if (op == 0xDEFC) { /* ADDA.W #imm,SP */
        int16_t imm = (code[off+2] << 8) | code[off+3];
        printf("  %04X: ADDA.W #%d,SP  (stack cleanup)\n", off, imm);
        if (pop_total) *pop_total += imm;
        sz = 4;
    } else if (op == 0xDFFC) { /* ADDA.L #imm,SP */
        if (off + 5 < code_size) {
            int32_t imm = ((int32_t)code[off+2]<<24)|((int32_t)code[off+3]<<16)|
                          ((int32_t)code[off+4]<<8)|code[off+5];
            printf("  %04X: ADDA.L #%d,SP  (stack cleanup long)\n", off, imm);
            if (pop_total) *pop_total += imm;
            sz = 6;
        }
    } else if (op == 0x2F00) {
        printf("  %04X: MOVE.L D0,-(SP)  push 4\n", off);
        if (push_total) *push_total += 4;
    } else if (op == 0x3F00) {
        printf("  %04X: MOVE.W D0,-(SP)  push 2\n", off);
        if (push_total) *push_total += 2;
    } else if ((op & 0xFFF8) == 0x2F08) { /* MOVE.L An,-(SP) */
        printf("  %04X: MOVE.L A%d,-(SP)  push 4 (addr reg)\n", off, op & 7);
        if (push_total) *push_total += 4;
    } else if ((op & 0xFFF0) == 0x2F00) { /* MOVE.L Dn,-(SP) other than D0 */
        printf("  %04X: MOVE.L D%d,-(SP)  push 4\n", off, op & 7);
        if (push_total) *push_total += 4;
    } else if ((op & 0xFFF0) == 0x3F00) { /* MOVE.W Dn,-(SP) other than D0 */
        printf("  %04X: MOVE.W D%d,-(SP)  push 2\n", off, op & 7);
        if (push_total) *push_total += 2;
    } else if (op == 0x4878) { /* PEA abs.W */
        int16_t imm = (code[off+2] << 8) | code[off+3];
        printf("  %04X: PEA $%04X  push 4\n", off, (uint16_t)imm);
        if (push_total) *push_total += 4;
        sz = 4;
    } else if (op == 0x4879) { /* PEA abs.L */
        if (off + 5 < code_size) {
            uint32_t addr = ((uint32_t)code[off+2]<<24)|((uint32_t)code[off+3]<<16)|
                            ((uint32_t)code[off+4]<<8)|code[off+5];
            printf("  %04X: PEA $%08X  push 4\n", off, addr);
            if (push_total) *push_total += 4;
            sz = 6;
        }
    } else if ((op & 0xFFF8) == 0x4868) { /* PEA d(An) */
        int16_t disp = (code[off+2] << 8) | code[off+3];
        printf("  %04X: PEA %d(A%d)  push 4\n", off, disp, op & 7);
        if (push_total) *push_total += 4;
        sz = 4;
    } else if (op == 0x487A) { /* PEA d(PC) */
        int16_t disp = (code[off+2] << 8) | code[off+3];
        printf("  %04X: PEA %d(PC)  push 4\n", off, disp);
        if (push_total) *push_total += 4;
        sz = 4;
    } else if (op == 0x2010) {
        printf("  %04X: MOVE.L (A0),D0\n", off);
    } else if (op == 0x3010) {
        printf("  %04X: MOVE.W (A0),D0\n", off);
    } else if (op == 0x2080) {
        printf("  %04X: MOVE.L D0,(A0)\n", off);
    } else if (op == 0x3080) {
        printf("  %04X: MOVE.W D0,(A0)\n", off);
    } else if (op == 0x2050) { /* MOVEA.L (A0),A0 */
        printf("  %04X: MOVEA.L (A0),A0  (deref pointer)\n", off);
    } else if (op == 0x0C40) { /* CMPI.W #imm,D0 */
        int16_t imm = (code[off+2] << 8) | code[off+3];
        printf("  %04X: CMPI.W #%d,D0\n", off, imm);
        sz = 4;
    } else if (op == 0x0C80) { /* CMPI.L #imm,D0 */
        if (off + 5 < code_size) {
            int32_t imm = ((int32_t)code[off+2]<<24)|((int32_t)code[off+3]<<16)|
                          ((int32_t)code[off+4]<<8)|code[off+5];
            printf("  %04X: CMPI.L #%d,D0\n", off, imm);
            sz = 6;
        }
    } else if (op == 0x5240) { /* ADDQ.W #1,D0 */
        printf("  %04X: ADDQ.W #1,D0\n", off);
    } else if (op == 0x5280) { /* ADDQ.L #1,D0 */
        printf("  %04X: ADDQ.L #1,D0\n", off);
    } else if (op == 0x5340) { /* SUBQ.W #1,D0 */
        printf("  %04X: SUBQ.W #1,D0\n", off);
    } else if (op == 0x5380) { /* SUBQ.L #1,D0 */
        printf("  %04X: SUBQ.L #1,D0\n", off);
    } else if ((op & 0xFF00) == 0x6600) { /* BNE */
        int8_t disp8 = (int8_t)(op & 0xFF);
        if (disp8 == 0) {
            int16_t disp16 = (code[off+2] << 8) | code[off+3];
            printf("  %04X: BNE $%04X\n", off, (uint16_t)(off + 2 + disp16));
            sz = 4;
        } else {
            printf("  %04X: BNE.S $%04X\n", off, (uint16_t)(off + 2 + disp8));
        }
    } else if ((op & 0xFF00) == 0x6700) { /* BEQ */
        int8_t disp8 = (int8_t)(op & 0xFF);
        if (disp8 == 0) {
            int16_t disp16 = (code[off+2] << 8) | code[off+3];
            printf("  %04X: BEQ $%04X\n", off, (uint16_t)(off + 2 + disp16));
            sz = 4;
        } else {
            printf("  %04X: BEQ.S $%04X\n", off, (uint16_t)(off + 2 + disp8));
        }
    } else if ((op & 0xFF00) == 0x6D00) { /* BLT */
        int8_t disp8 = (int8_t)(op & 0xFF);
        if (disp8 == 0) {
            int16_t disp16 = (code[off+2] << 8) | code[off+3];
            printf("  %04X: BLT $%04X\n", off, (uint16_t)(off + 2 + disp16));
            sz = 4;
        } else {
            printf("  %04X: BLT.S $%04X\n", off, (uint16_t)(off + 2 + disp8));
        }
    } else if ((op & 0xFF00) == 0x6E00) { /* BGT */
        int8_t disp8 = (int8_t)(op & 0xFF);
        if (disp8 == 0) {
            int16_t disp16 = (code[off+2] << 8) | code[off+3];
            printf("  %04X: BGT $%04X\n", off, (uint16_t)(off + 2 + disp16));
            sz = 4;
        } else {
            printf("  %04X: BGT.S $%04X\n", off, (uint16_t)(off + 2 + disp8));
        }
    } else if ((op & 0xFF00) == 0x6F00) { /* BLE */
        int8_t disp8 = (int8_t)(op & 0xFF);
        if (disp8 == 0) {
            int16_t disp16 = (code[off+2] << 8) | code[off+3];
            printf("  %04X: BLE $%04X\n", off, (uint16_t)(off + 2 + disp16));
            sz = 4;
        } else {
            printf("  %04X: BLE.S $%04X\n", off, (uint16_t)(off + 2 + disp8));
        }
    } else if ((op & 0xFF00) == 0x6C00) { /* BGE */
        int8_t disp8 = (int8_t)(op & 0xFF);
        if (disp8 == 0) {
            int16_t disp16 = (code[off+2] << 8) | code[off+3];
            printf("  %04X: BGE $%04X\n", off, (uint16_t)(off + 2 + disp16));
            sz = 4;
        } else {
            printf("  %04X: BGE.S $%04X\n", off, (uint16_t)(off + 2 + disp8));
        }
    } else if ((op & 0xFF00) == 0x6000) { /* BRA */
        int8_t disp8 = (int8_t)(op & 0xFF);
        if (disp8 == 0) {
            int16_t disp16 = (code[off+2] << 8) | code[off+3];
            printf("  %04X: BRA $%04X\n", off, (uint16_t)(off + 2 + disp16));
            sz = 4;
        } else {
            printf("  %04X: BRA.S $%04X\n", off, (uint16_t)(off + 2 + disp8));
        }
    } else if (op == 0x3F3C) { /* MOVE.W #imm,-(SP) */
        int16_t imm = (code[off+2] << 8) | code[off+3];
        printf("  %04X: MOVE.W #%d,-(SP)  push 2 (imm)\n", off, imm);
        if (push_total) *push_total += 2;
        sz = 4;
    } else if (op == 0x2F3C) { /* MOVE.L #imm,-(SP) */
        if (off + 5 < code_size) {
            int32_t imm = ((int32_t)code[off+2]<<24)|((int32_t)code[off+3]<<16)|
                          ((int32_t)code[off+4]<<8)|code[off+5];
            printf("  %04X: MOVE.L #%d,-(SP)  push 4 (imm)\n", off, imm);
            if (push_total) *push_total += 4;
            sz = 6;
        }
    }
    /* Frame-relative ops */
    else if (off + 3 < code_size && (op == 0x302E || op == 0x202E || op == 0x3D40 ||
             op == 0x2D40 || op == 0x206E || op == 0x41EE)) {
        int16_t d = (code[off+2] << 8) | code[off+3];
        const char *mn = op==0x302E ? "MOVE.W" : op==0x202E ? "MOVE.L" :
                         op==0x3D40 ? "MOVE.W" : op==0x2D40 ? "MOVE.L" :
                         op==0x206E ? "MOVEA.L" : "LEA";
        const char *dir = (op==0x3D40||op==0x2D40) ? "D0," : "";
        printf("  %04X: %s %s%d(A6)%s\n", off, mn, dir, d,
               (op==0x3D40||op==0x2D40) ? "" : ",D0/A0");
        sz = 4;
    }
    /* Push from frame: MOVE.W d(A6),-(SP) = 0x3F2E, MOVE.L d(A6),-(SP) = 0x2F2E */
    else if (off + 3 < code_size && (op == 0x3F2E || op == 0x2F2E)) {
        int16_t d = (code[off+2] << 8) | code[off+3];
        int psz = (op == 0x2F2E) ? 4 : 2;
        printf("  %04X: MOVE.%c %d(A6),-(SP)  push %d (from frame)\n",
               off, psz==4?'L':'W', d, psz);
        if (push_total) *push_total += psz;
        sz = 4;
    }
    /* PEA d(A6) = 0x486E */
    else if (off + 3 < code_size && op == 0x486E) {
        int16_t d = (code[off+2] << 8) | code[off+3];
        printf("  %04X: PEA %d(A6)  push 4 (addr of local)\n", off, d);
        if (push_total) *push_total += 4;
        sz = 4;
    }
    else { sz = 2; } /* skip silently */

    return sz;
}

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
    int push_total = 0, pop_total = 0;
    for (int off = 0; off < (int)cg->code_size; ) {
        int sz = decode_one(cg->code, (int)cg->code_size, off, &errors,
                            &push_total, &pop_total);
        off += sz;
    }

    /* Check for push/pop imbalances visible in the stream */
    if (push_total > 0 && pop_total > 0 && push_total != pop_total) {
        printf("  NOTE: pushed %d bytes total, cleaned %d bytes total (review for mismatch)\n",
               push_total, pop_total);
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

    /* ----------------------------------------------------------------
     * Original tests
     * ---------------------------------------------------------------- */

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

    /* ----------------------------------------------------------------
     * NEW: Stack corruption hunting tests
     * ---------------------------------------------------------------- */

    /* 1. Nested calls with mixed param sizes
     *    Procedure A calls B(integer, longint, pointer).
     *    The push sizes must be 2, 4, 4 and the caller must clean 10 bytes. */
    total += verify("nested_call_mixed_params",
        "unit tc1; interface\n"
        "type P = ^integer;\n"
        "procedure B(x: integer; y: longint; z: P);\n"
        "procedure A;\n"
        "implementation\n"
        "procedure B; begin end;\n"
        "procedure A; var i: integer; l: longint; p: P;\n"
        "begin B(i, l, p); end;\n"
        "end.\n");

    /* 2. VAR params with record types
     *    Procedure takes VAR r: SomeRecord, accesses r.field.
     *    Must dereference the VAR pointer, then apply field offset with correct size. */
    total += verify("var_param_record_access",
        "unit tc2; interface\n"
        "type Rec = record x: integer; y: longint; z: integer; end;\n"
        "procedure SetFields(VAR r: Rec);\n"
        "implementation\n"
        "procedure SetFields; begin r.x := 10; r.y := 100000; r.z := 20; end;\n"
        "end.\n");

    /* 3. Function returning longint
     *    Function F: longint called from another procedure.
     *    The return value must be handled as 4 bytes, not 2. */
    total += verify("func_return_longint",
        "unit tc3; interface\n"
        "function GetVal(a: longint): longint;\n"
        "procedure UseVal;\n"
        "implementation\n"
        "function GetVal; begin GetVal := a + 1; end;\n"
        "procedure UseVal; var v: longint;\n"
        "begin v := GetVal(42); end;\n"
        "end.\n");

    /* 4. Multiple VAR params
     *    procedure P(VAR a: integer; VAR b: longint)
     *    Both derefs must use correct sizes (MOVE.W for integer, MOVE.L for longint). */
    total += verify("multi_var_params",
        "unit tc4; interface\n"
        "procedure Swap(VAR a: integer; VAR b: longint);\n"
        "implementation\n"
        "procedure Swap; begin a := 1; b := 2; end;\n"
        "end.\n");

    /* 5. Calling an external (callee-clean) procedure
     *    When Pascal code calls an EXTERNAL procedure, the caller must NOT
     *    clean the stack — the callee does it. Mismatch = stack corruption. */
    total += verify("call_external_proc",
        "unit tc5; interface\n"
        "procedure ExtProc(x: integer; y: longint); external;\n"
        "procedure Caller;\n"
        "implementation\n"
        "procedure Caller; begin ExtProc(5, 100); end;\n"
        "end.\n");

    /* 6. FOR loop with different variable types
     *    FOR i (integer) and FOR j (longint) — the increment and compare
     *    must use the correct operand size (.W vs .L). */
    total += verify("for_loop_integer",
        "unit tc6; interface\n"
        "procedure LoopI;\n"
        "implementation\n"
        "procedure LoopI; var i: integer; s: integer;\n"
        "begin s := 0; for i := 1 to 10 do s := s + i; end;\n"
        "end.\n");

    total += verify("for_loop_longint",
        "unit tc7; interface\n"
        "procedure LoopL;\n"
        "implementation\n"
        "procedure LoopL; var j: longint; s: longint;\n"
        "begin s := 0; for j := 1 to 1000 do s := s + j; end;\n"
        "end.\n");

    /* 7. Array of records
     *    Access arr[i].field — must compute array stride (record size * index)
     *    THEN apply field offset. A zero stride or wrong field offset = corruption. */
    total += verify("array_of_records",
        "unit tc8; interface\n"
        "type Item = record id: integer; val: longint; end;\n"
        "type Items = array[0..9] of Item;\n"
        "procedure GetField(VAR a: Items; i: integer; VAR out: longint);\n"
        "implementation\n"
        "procedure GetField; begin out := a[i].val; end;\n"
        "end.\n");

    /* 8. Nested record fields
     *    r.inner.field — must chain field accesses correctly:
     *    base + outer offset, then + inner offset. */
    total += verify("nested_record_fields",
        "unit tc9; interface\n"
        "type Inner = record a: integer; b: longint; end;\n"
        "type Outer = record tag: integer; sub: Inner; extra: integer; end;\n"
        "procedure Access(VAR r: Outer; VAR out: longint);\n"
        "implementation\n"
        "procedure Access; begin out := r.sub.b; end;\n"
        "end.\n");

    printf("\n=== TOTAL ERRORS: %d ===\n", total);
    return total;
}
