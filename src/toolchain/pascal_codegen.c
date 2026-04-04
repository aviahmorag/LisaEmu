/*
 * LisaEm Toolchain — Lisa Pascal Code Generator
 *
 * Generates Motorola 68000 machine code from a Lisa Pascal AST.
 *
 * Lisa Pascal calling convention:
 *   - A5 = global data base
 *   - A6 = frame pointer
 *   - A7 = stack pointer
 *   - Parameters pushed right-to-left
 *   - Function result space allocated by caller
 *   - LINK A6,#-localsize at entry
 *   - UNLK A6; RTS at exit
 */

#include "pascal_codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* ========================================================================
 * Helpers
 * ======================================================================== */

static void cg_error(codegen_t *cg, int line, const char *fmt, ...) {
    if (cg->num_errors >= 100) return;
    char *buf = cg->errors[cg->num_errors++];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 256, fmt, args);
    va_end(args);
    fprintf(stderr, "%s:%d: codegen error: %s\n", cg->current_file, line, buf);
}

static bool str_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

/* ========================================================================
 * Code emission (big-endian 68000)
 * ======================================================================== */

static void emit8(codegen_t *cg, uint8_t val) {
    if (cg->code_size < CODEGEN_MAX_OUTPUT)
        cg->code[cg->code_size] = val;
    cg->code_size++;
}

static void emit16(codegen_t *cg, uint16_t val) {
    emit8(cg, (val >> 8) & 0xFF);
    emit8(cg, val & 0xFF);
}

static void emit32(codegen_t *cg, uint32_t val) {
    emit16(cg, (val >> 16) & 0xFFFF);
    emit16(cg, val & 0xFFFF);
}

/* Patch a 16-bit value at a given offset */
static void patch16(codegen_t *cg, uint32_t offset, uint16_t val) {
    if (offset + 1 < CODEGEN_MAX_OUTPUT) {
        cg->code[offset] = (val >> 8) & 0xFF;
        cg->code[offset + 1] = val & 0xFF;
    }
}



/* Record current code position as a label target */
#define HERE(cg) ((cg)->code_size)

/* ========================================================================
 * Type system
 * ======================================================================== */

static type_desc_t *find_type(codegen_t *cg, const char *name) {
    for (int i = 0; i < cg->num_types; i++) {
        if (str_eq_nocase(cg->types[i].name, name))
            return &cg->types[i];
    }
    return NULL;
}

static type_desc_t *add_type(codegen_t *cg, const char *name, type_kind_t kind, int size) {
    if (cg->num_types >= CODEGEN_MAX_SYMBOLS) return NULL;
    type_desc_t *t = &cg->types[cg->num_types++];
    memset(t, 0, sizeof(type_desc_t));
    t->kind = kind;
    t->size = size;
    strncpy(t->name, name, sizeof(t->name) - 1);
    return t;
}

static void init_builtin_types(codegen_t *cg) {
    add_type(cg, "integer", TK_INTEGER, 2);
    add_type(cg, "int1", TK_BYTE, 1);
    add_type(cg, "int2", TK_INTEGER, 2);
    add_type(cg, "int4", TK_LONGINT, 4);
    add_type(cg, "longint", TK_LONGINT, 4);
    add_type(cg, "boolean", TK_BOOLEAN, 1);
    add_type(cg, "char", TK_CHAR, 1);
    add_type(cg, "real", TK_REAL, 4);
    add_type(cg, "byte", TK_BYTE, 1);
    add_type(cg, "absptr", TK_LONGINT, 4);   /* Lisa OS: absolute pointer */
    add_type(cg, "ptr", TK_POINTER, 4);
    add_type(cg, "text", TK_FILE, 0);
}

static int type_size(type_desc_t *t) {
    if (!t) return 2; /* default word */
    return t->size;
}

/* Resolve a type from an AST type node */
static type_desc_t *resolve_type(codegen_t *cg, ast_node_t *node) {
    if (!node) return find_type(cg, "integer");

    switch (node->type) {
        case AST_TYPE_IDENT:
            return find_type(cg, node->name);

        case AST_TYPE_POINTER: {
            type_desc_t *t = add_type(cg, "", TK_POINTER, 4);
            t->base_type = find_type(cg, node->name);
            return t;
        }

        case AST_TYPE_STRING: {
            type_desc_t *t = add_type(cg, "", TK_STRING, (int)(node->int_val + 1));
            t->max_length = (int)node->int_val;
            return t;
        }

        case AST_TYPE_SUBRANGE: {
            type_desc_t *t = add_type(cg, "", TK_SUBRANGE, 2);
            if (node->num_children >= 2) {
                t->range_low = (int)node->children[0]->int_val;
                t->range_high = (int)node->children[1]->int_val;
                int range = t->range_high - t->range_low;
                if (range <= 255) t->size = 1;
                else if (range <= 65535) t->size = 2;
                else t->size = 4;
            }
            return t;
        }

        case AST_TYPE_ARRAY: {
            type_desc_t *t = add_type(cg, "", TK_ARRAY, 0);
            /* children: low, high, ..., element_type */
            if (node->num_children >= 3) {
                t->array_low = (int)node->children[0]->int_val;
                t->array_high = (int)node->children[1]->int_val;
                t->element_type = resolve_type(cg, node->children[node->num_children - 1]);
                int count = t->array_high - t->array_low + 1;
                t->size = count * (t->element_type ? t->element_type->size : 2);
            }
            return t;
        }

        case AST_TYPE_RECORD: {
            type_desc_t *t = add_type(cg, "", TK_RECORD, 0);
            int offset = 0;
            for (int i = 0; i < node->num_children; i++) {
                ast_node_t *field = node->children[i];
                if (field->type == AST_FIELD && field->num_children > 0) {
                    type_desc_t *ft = resolve_type(cg, field->children[0]);
                    int fs = ft ? ft->size : 2;
                    /* Word-align fields */
                    if (fs >= 2 && (offset % 2)) offset++;
                    if (t->num_fields < 64) {
                        strncpy(t->fields[t->num_fields].name, field->name, 63);
                        t->fields[t->num_fields].offset = offset;
                        t->fields[t->num_fields].type = ft;
                        t->num_fields++;
                    }
                    offset += fs;
                }
            }
            if (offset % 2) offset++; /* Pad to word boundary */
            t->size = offset;
            return t;
        }

        case AST_TYPE_SET: {
            type_desc_t *t = add_type(cg, "", TK_SET, 32); /* 256-bit set */
            return t;
        }

        case AST_TYPE_PACKED:
            if (node->num_children > 0)
                return resolve_type(cg, node->children[0]);
            return find_type(cg, "integer");

        case AST_TYPE_ENUM: {
            type_desc_t *t = add_type(cg, "", TK_ENUM, 2);
            return t;
        }

        default:
            return find_type(cg, "integer");
    }
}

/* ========================================================================
 * Symbol management
 * ======================================================================== */

static cg_scope_t *current_scope(codegen_t *cg) {
    if (cg->scope_depth <= 0) return NULL;
    return &cg->scopes[cg->scope_depth - 1];
}

static void push_scope(codegen_t *cg) {
    if (cg->scope_depth < CODEGEN_MAX_SCOPE) {
        cg->scopes[cg->scope_depth].num_locals = 0;
        cg->scopes[cg->scope_depth].frame_size = 0;
        cg->scope_depth++;
    }
}

static void pop_scope(codegen_t *cg) {
    if (cg->scope_depth > 0) cg->scope_depth--;
}

static cg_symbol_t *find_local(codegen_t *cg, const char *name) {
    for (int d = cg->scope_depth - 1; d >= 0; d--) {
        cg_scope_t *sc = &cg->scopes[d];
        for (int i = 0; i < sc->num_locals; i++) {
            if (str_eq_nocase(sc->locals[i].name, name))
                return &sc->locals[i];
        }
    }
    return NULL;
}

static cg_symbol_t *find_global(codegen_t *cg, const char *name) {
    for (int i = 0; i < cg->num_globals; i++) {
        if (str_eq_nocase(cg->globals[i].name, name))
            return &cg->globals[i];
    }
    return NULL;
}

static cg_symbol_t *find_symbol_any(codegen_t *cg, const char *name) {
    cg_symbol_t *s = find_local(cg, name);
    if (s) return s;
    return find_global(cg, name);
}

static cg_symbol_t *add_local(codegen_t *cg, const char *name, type_desc_t *type, bool is_param, bool is_var) {
    cg_scope_t *sc = current_scope(cg);
    if (!sc || sc->num_locals >= CODEGEN_MAX_LOCALS) return NULL;
    cg_symbol_t *s = &sc->locals[sc->num_locals++];
    memset(s, 0, sizeof(cg_symbol_t));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->type = type;
    s->is_param = is_param;
    s->is_var_param = is_var;
    return s;
}

static cg_symbol_t *add_global_sym(codegen_t *cg, const char *name, type_desc_t *type) {
    if (cg->num_globals >= CODEGEN_MAX_SYMBOLS) return NULL;
    cg_symbol_t *s = &cg->globals[cg->num_globals++];
    memset(s, 0, sizeof(cg_symbol_t));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->type = type;
    s->is_global = true;
    return s;
}

/* ========================================================================
 * Expression code generation
 *
 * All expressions leave their result on the stack (or in D0 for simple types).
 * This is a stack-based evaluation model matching Lisa Pascal conventions.
 * ======================================================================== */

static void gen_expression(codegen_t *cg, ast_node_t *node);
static void gen_statement(codegen_t *cg, ast_node_t *node);

/* Load a variable's address into A0 */
static void gen_lvalue_addr(codegen_t *cg, ast_node_t *node) {
    if (node->type == AST_IDENT_EXPR) {
        cg_symbol_t *sym = find_symbol_any(cg, node->name);
        if (sym) {
            if (sym->is_param && sym->is_var_param) {
                /* VAR param: A6 + offset contains pointer */
                /* MOVEA.L offset(A6),A0 */
                emit16(cg, 0x206E);
                emit16(cg, (uint16_t)(int16_t)sym->offset);
            } else if (sym->is_param || !sym->is_global) {
                /* Local/param: LEA offset(A6),A0 */
                emit16(cg, 0x41EE);
                emit16(cg, (uint16_t)(int16_t)sym->offset);
            } else {
                /* Global: LEA offset(A5),A0 */
                emit16(cg, 0x41ED);
                emit16(cg, (uint16_t)(int16_t)sym->offset);
            }
        } else {
            /* Unknown symbol — emit placeholder with relocation */
            emit16(cg, 0x41F9);  /* LEA abs.L,A0 */
            emit32(cg, 0);       /* Will be relocated */
        }
    } else if (node->type == AST_ARRAY_ACCESS) {
        /* array[index]: compute base + (index - low) * element_size */
        gen_lvalue_addr(cg, node->children[0]); /* base in A0 */
        if (node->num_children > 1) {
            gen_expression(cg, node->children[1]); /* index in D0 */
            /* MOVE.W D0,D1; MULU #elemsize,D1; ADDA.L D1,A0 */
            emit16(cg, 0x3200);  /* MOVE.W D0,D1 */
            emit16(cg, 0xC2FC);  /* MULU #imm,D1 */
            emit16(cg, 2);       /* element size placeholder */
            emit16(cg, 0xD1C1);  /* ADDA.L D1,A0 */
        }
    } else if (node->type == AST_FIELD_ACCESS) {
        gen_lvalue_addr(cg, node->children[0]); /* record addr in A0 */
        /* ADDA.W #offset,A0 — field offset placeholder */
        emit16(cg, 0xD0FC);
        emit16(cg, 0);  /* field offset - would need type info */
    } else if (node->type == AST_DEREF) {
        gen_expression(cg, node->children[0]); /* pointer value in D0 */
        emit16(cg, 0x2040);  /* MOVEA.L D0,A0 */
    }
}

/* Generate code that leaves expression result in D0 */
static void gen_expression(codegen_t *cg, ast_node_t *node) {
    if (!node) return;

    switch (node->type) {
        case AST_INT_LITERAL:
            if (node->int_val >= -128 && node->int_val <= 127) {
                /* MOVEQ #imm,D0 */
                emit16(cg, 0x7000 | ((int8_t)node->int_val & 0xFF));
            } else if (node->int_val >= -32768 && node->int_val <= 32767) {
                /* MOVE.W #imm,D0 */
                emit16(cg, 0x303C);
                emit16(cg, (uint16_t)(int16_t)node->int_val);
            } else {
                /* MOVE.L #imm,D0 */
                emit16(cg, 0x203C);
                emit32(cg, (uint32_t)node->int_val);
            }
            break;

        case AST_STRING_LITERAL: {
            /* Store string in code, load address to A0 */
            /* LEA string_data(PC),A0 */
            emit16(cg, 0x41FA);       /* LEA d(PC),A0 */
            uint32_t disp_pos = cg->code_size;
            emit16(cg, 0);            /* displacement - patch later */
            uint32_t after = cg->code_size;
            /* BRA past string data */
            emit16(cg, 0x6000);
            uint32_t bra_pos = cg->code_size;
            emit16(cg, 0);
            /* String data: length byte + chars */
            uint32_t str_start = cg->code_size;
            int len = (int)strlen(node->str_val);
            emit8(cg, (uint8_t)len);
            for (int i = 0; i < len; i++) emit8(cg, node->str_val[i]);
            if (cg->code_size % 2) emit8(cg, 0); /* pad to word */
            /* Patch displacement and branch */
            patch16(cg, disp_pos, (uint16_t)(str_start - after + 2));
            patch16(cg, bra_pos, (uint16_t)(cg->code_size - after - 2));
            /* Move address to D0 for consistency */
            emit16(cg, 0x2008);  /* MOVE.L A0,D0 */
            break;
        }

        case AST_NIL_EXPR:
            emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
            break;

        case AST_IDENT_EXPR: {
            cg_symbol_t *sym = find_symbol_any(cg, node->name);
            if (sym) {
                if (sym->is_param && sym->is_var_param) {
                    /* VAR param: dereference pointer at offset(A6) */
                    emit16(cg, 0x206E);  /* MOVEA.L offset(A6),A0 */
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                    emit16(cg, 0x3010);  /* MOVE.W (A0),D0 */
                } else if (sym->is_param || !sym->is_global) {
                    /* Local/param: MOVE.W offset(A6),D0 */
                    int sz = sym->type ? sym->type->size : 2;
                    if (sz == 4) {
                        emit16(cg, 0x202E);  /* MOVE.L offset(A6),D0 */
                    } else if (sz == 1) {
                        emit16(cg, 0x102E);  /* MOVE.B offset(A6),D0 */
                    } else {
                        emit16(cg, 0x302E);  /* MOVE.W offset(A6),D0 */
                    }
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                } else {
                    /* Global: MOVE.W offset(A5),D0 */
                    int sz = sym->type ? sym->type->size : 2;
                    if (sz == 4) {
                        emit16(cg, 0x202D);
                    } else if (sz == 1) {
                        emit16(cg, 0x102D);
                    } else {
                        emit16(cg, 0x302D);
                    }
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                }
            } else {
                /* Unknown — might be a constant or external */
                emit16(cg, 0x303C);  /* MOVE.W #0,D0 placeholder */
                emit16(cg, 0);
            }
            break;
        }

        case AST_UNARY_OP:
            gen_expression(cg, node->children[0]);
            if (node->op == TOK_MINUS) {
                emit16(cg, 0x4440);  /* NEG.W D0 */
            } else if (node->op == TOK_NOT) {
                emit16(cg, 0x4640);  /* NOT.W D0 */
            }
            break;

        case AST_BINARY_OP: {
            gen_expression(cg, node->children[0]);
            /* Save left in D2 */
            emit16(cg, 0x3400);  /* MOVE.W D0,D2 */
            gen_expression(cg, node->children[1]);
            /* D2 = left, D0 = right */
            emit16(cg, 0x3200);  /* MOVE.W D0,D1 (right in D1) */
            emit16(cg, 0x3002);  /* MOVE.W D2,D0 (left in D0) */

            switch (node->op) {
                case TOK_PLUS:
                    emit16(cg, 0xD041);  /* ADD.W D1,D0 */
                    break;
                case TOK_MINUS:
                    emit16(cg, 0x9041);  /* SUB.W D1,D0 */
                    break;
                case TOK_STAR:
                    emit16(cg, 0xC1C1);  /* MULS D1,D0 */
                    break;
                case TOK_SLASH:
                case TOK_DIV:
                    emit16(cg, 0x48C0);  /* EXT.L D0 */
                    emit16(cg, 0x81C1);  /* DIVS D1,D0 */
                    break;
                case TOK_MOD:
                    emit16(cg, 0x48C0);  /* EXT.L D0 */
                    emit16(cg, 0x81C1);  /* DIVS D1,D0 */
                    emit16(cg, 0x4840);  /* SWAP D0 (remainder in low word) */
                    break;
                case TOK_AND:
                    emit16(cg, 0xC041);  /* AND.W D1,D0 */
                    break;
                case TOK_OR:
                    emit16(cg, 0x8041);  /* OR.W D1,D0 */
                    break;
                case TOK_XOR:
                    emit16(cg, 0xB341);  /* EOR.W D1,D1... actually EOR.W D0,D1 then move */
                    /* EOR.W D1,D0 */
                    emit16(cg, 0xB340);
                    break;
                case TOK_SHL:
                    /* ASL.W D1,D0 — shift D0 left by D1 */
                    emit16(cg, 0xE360);
                    break;
                case TOK_SHR:
                    /* LSR.W D1,D0 — shift D0 right by D1 */
                    emit16(cg, 0xE268);
                    break;
                case TOK_IN:
                    /* Set membership: D0 IN D1 (simplified — just emit BTST) */
                    emit16(cg, 0x0100);  /* BTST D0,D1 */
                    emit16(cg, 0x56C0);  /* SNE D0 */
                    emit16(cg, 0x4400);  /* NEG.B D0 */
                    emit16(cg, 0x0240); emit16(cg, 0x0001); /* ANDI.W #1,D0 */
                    break;

                /* Comparisons: CMP and set condition */
                case TOK_EQ:
                    emit16(cg, 0xB041);  /* CMP.W D1,D0 */
                    emit16(cg, 0x57C0);  /* SEQ D0 */
                    emit16(cg, 0x4400);  /* NEG.B D0 (0 or 1) */
                    emit16(cg, 0x0240); emit16(cg, 0x0001); /* ANDI.W #1,D0 */
                    break;
                case TOK_NE:
                    emit16(cg, 0xB041);
                    emit16(cg, 0x56C0);  /* SNE D0 */
                    emit16(cg, 0x4400);
                    emit16(cg, 0x0240); emit16(cg, 0x0001);
                    break;
                case TOK_LT:
                    emit16(cg, 0xB041);
                    emit16(cg, 0x5DC0);  /* SLT D0 */
                    emit16(cg, 0x4400);
                    emit16(cg, 0x0240); emit16(cg, 0x0001);
                    break;
                case TOK_LE:
                    emit16(cg, 0xB041);
                    emit16(cg, 0x5FC0);  /* SLE D0 */
                    emit16(cg, 0x4400);
                    emit16(cg, 0x0240); emit16(cg, 0x0001);
                    break;
                case TOK_GT:
                    emit16(cg, 0xB041);
                    emit16(cg, 0x5EC0);  /* SGT D0 */
                    emit16(cg, 0x4400);
                    emit16(cg, 0x0240); emit16(cg, 0x0001);
                    break;
                case TOK_GE:
                    emit16(cg, 0xB041);
                    emit16(cg, 0x5CC0);  /* SGE D0 */
                    emit16(cg, 0x4400);
                    emit16(cg, 0x0240); emit16(cg, 0x0001);
                    break;

                default:
                    cg_error(cg, node->line, "unsupported operator in expression");
                    break;
            }
            break;
        }

        case AST_FUNC_CALL: {
            /* Push arguments right-to-left.
             * Lisa Pascal convention: parameters are pushed as longwords (4 bytes)
             * for pointers/longints and words (2 bytes) for integers/booleans.
             * Without type info at the call site, push as LONGWORD (4 bytes)
             * to match assembly routines that expect 32-bit values. */
            for (int i = node->num_children - 1; i >= 0; i--) {
                gen_expression(cg, node->children[i]);
                emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
            }
            /* JSR to function */
            emit16(cg, 0x4EB9);  /* JSR abs.L */
            emit32(cg, 0);       /* Will be relocated */
            /* Add relocation */
            if (cg->num_relocs < CODEGEN_MAX_RELOCS) {
                cg_reloc_t *r = &cg->relocs[cg->num_relocs++];
                r->offset = cg->code_size - 4;
                strncpy(r->symbol, node->name, sizeof(r->symbol) - 1);
                r->size = 4;
                r->pc_relative = false;
            }
            /* Clean up args — 4 bytes per argument */
            int arg_bytes = node->num_children * 4;
            if (arg_bytes > 0 && arg_bytes <= 8) {
                emit16(cg, 0x508F | ((arg_bytes & 7) << 9));  /* ADDQ.L #n,SP */
            } else if (arg_bytes > 8) {
                emit16(cg, 0xDEFC);  /* ADDA.W #imm,SP */
                emit16(cg, (uint16_t)arg_bytes);
            }
            /* Result is in D0 */
            break;
        }

        case AST_ARRAY_ACCESS:
            gen_lvalue_addr(cg, node);
            emit16(cg, 0x3010);  /* MOVE.W (A0),D0 */
            break;

        case AST_FIELD_ACCESS:
            gen_lvalue_addr(cg, node);
            emit16(cg, 0x3010);  /* MOVE.W (A0),D0 */
            break;

        case AST_DEREF:
            gen_expression(cg, node->children[0]);
            emit16(cg, 0x2040);  /* MOVEA.L D0,A0 */
            emit16(cg, 0x3010);  /* MOVE.W (A0),D0 */
            break;

        case AST_ADDR_OF:
            gen_lvalue_addr(cg, node->children[0]);
            emit16(cg, 0x2008);  /* MOVE.L A0,D0 */
            break;

        case AST_SET_EXPR:
            /* Simplified: just push 0 for now */
            emit16(cg, 0x7000);
            break;

        default:
            /* Unknown expression type */
            emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
            break;
    }
}

/* ========================================================================
 * Statement code generation
 * ======================================================================== */

static void gen_statement(codegen_t *cg, ast_node_t *node) {
    if (!node) return;

    switch (node->type) {
        case AST_EMPTY:
            break;

        case AST_BLOCK:
            for (int i = 0; i < node->num_children; i++) {
                gen_statement(cg, node->children[i]);
            }
            break;

        case AST_ASSIGN: {
            /* Evaluate RHS into D0 */
            gen_expression(cg, node->children[1]);
            /* Store to LHS */
            ast_node_t *lhs = node->children[0];
            cg_symbol_t *sym = find_symbol_any(cg, lhs->name);
            if (sym) {
                if (sym->is_param && sym->is_var_param) {
                    emit16(cg, 0x206E);  /* MOVEA.L offset(A6),A0 */
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                    emit16(cg, 0x3080);  /* MOVE.W D0,(A0) */
                } else if (sym->is_param || !sym->is_global) {
                    int sz = sym->type ? sym->type->size : 2;
                    if (sz == 4) emit16(cg, 0x2D40);      /* MOVE.L D0,offset(A6) */
                    else if (sz == 1) emit16(cg, 0x1D40);  /* MOVE.B D0,offset(A6) */
                    else emit16(cg, 0x3D40);                /* MOVE.W D0,offset(A6) */
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                } else {
                    int sz = sym->type ? sym->type->size : 2;
                    if (sz == 4) emit16(cg, 0x2B40);
                    else if (sz == 1) emit16(cg, 0x1B40);
                    else emit16(cg, 0x3B40);
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                }
            } else if (lhs->type == AST_ARRAY_ACCESS || lhs->type == AST_FIELD_ACCESS || lhs->type == AST_DEREF) {
                /* Complex LHS */
                emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) save RHS */
                gen_lvalue_addr(cg, lhs);
                emit16(cg, 0x301F);  /* MOVE.W (SP)+,D0 restore RHS */
                emit16(cg, 0x3080);  /* MOVE.W D0,(A0) */
            }
            break;
        }

        case AST_CALL: {
            /* Procedure call with no args */
            emit16(cg, 0x4EB9);
            emit32(cg, 0);
            if (cg->num_relocs < CODEGEN_MAX_RELOCS) {
                cg_reloc_t *r = &cg->relocs[cg->num_relocs++];
                r->offset = cg->code_size - 4;
                strncpy(r->symbol, node->name, sizeof(r->symbol) - 1);
                r->size = 4;
            }
            break;
        }

        case AST_FUNC_CALL:
            gen_expression(cg, node);
            break;

        case AST_IF: {
            gen_expression(cg, node->children[0]);
            emit16(cg, 0x4A40);  /* TST.W D0 */
            emit16(cg, 0x6700);  /* BEQ.W else_branch */
            uint32_t else_pos = cg->code_size;
            emit16(cg, 0);       /* displacement - patch later */

            gen_statement(cg, node->children[1]);

            if (node->num_children > 2) {
                emit16(cg, 0x6000);  /* BRA.W end_if */
                uint32_t end_pos = cg->code_size;
                emit16(cg, 0);
                patch16(cg, else_pos, (uint16_t)(cg->code_size - else_pos));
                gen_statement(cg, node->children[2]);
                patch16(cg, end_pos, (uint16_t)(cg->code_size - end_pos));
            } else {
                patch16(cg, else_pos, (uint16_t)(cg->code_size - else_pos));
            }
            break;
        }

        case AST_WHILE: {
            uint32_t loop_start = cg->code_size;
            gen_expression(cg, node->children[0]);
            emit16(cg, 0x4A40);  /* TST.W D0 */
            emit16(cg, 0x6700);  /* BEQ.W end_while */
            uint32_t end_pos = cg->code_size;
            emit16(cg, 0);

            gen_statement(cg, node->children[1]);
            emit16(cg, 0x6000);  /* BRA.W loop_start */
            emit16(cg, (uint16_t)((int32_t)loop_start - (int32_t)cg->code_size));

            patch16(cg, end_pos, (uint16_t)(cg->code_size - end_pos));
            break;
        }

        case AST_REPEAT: {
            uint32_t loop_start = cg->code_size;
            /* Statements are children 0..n-2, condition is last child */
            for (int i = 0; i < node->num_children - 1; i++) {
                gen_statement(cg, node->children[i]);
            }
            gen_expression(cg, node->children[node->num_children - 1]);
            emit16(cg, 0x4A40);  /* TST.W D0 */
            emit16(cg, 0x6700);  /* BEQ.W loop_start */
            emit16(cg, (uint16_t)((int32_t)loop_start - (int32_t)cg->code_size));
            break;
        }

        case AST_FOR: {
            /* For var := start to/downto end do body */
            cg_symbol_t *var = find_symbol_any(cg, node->name);

            /* Initialize loop variable */
            gen_expression(cg, node->children[0]);
            if (var) {
                emit16(cg, 0x3D40);  /* MOVE.W D0,offset(A6) */
                emit16(cg, (uint16_t)(int16_t)(var ? var->offset : 0));
            }

            /* Evaluate end value, save in D3 */
            gen_expression(cg, node->children[1]);
            emit16(cg, 0x3600);  /* MOVE.W D0,D3 */

            /* Loop start */
            uint32_t loop_start = cg->code_size;

            /* Compare loop var to end */
            if (var) {
                emit16(cg, 0x302E);  /* MOVE.W offset(A6),D0 */
                emit16(cg, (uint16_t)(int16_t)var->offset);
            }
            emit16(cg, 0xB043);  /* CMP.W D3,D0 */
            if (node->int_val > 0) {
                emit16(cg, 0x6E00);  /* BGT.W end */
            } else {
                emit16(cg, 0x6D00);  /* BLT.W end */
            }
            uint32_t end_pos = cg->code_size;
            emit16(cg, 0);

            /* Body */
            gen_statement(cg, node->children[2]);

            /* Increment/decrement */
            if (var) {
                if (node->int_val > 0) {
                    emit16(cg, 0x526E);  /* ADDQ.W #1,offset(A6) */
                } else {
                    emit16(cg, 0x536E);  /* SUBQ.W #1,offset(A6) */
                }
                emit16(cg, (uint16_t)(int16_t)var->offset);
            }

            /* Branch back */
            emit16(cg, 0x6000);
            emit16(cg, (uint16_t)((int32_t)loop_start - (int32_t)cg->code_size));

            patch16(cg, end_pos, (uint16_t)(cg->code_size - end_pos));
            break;
        }

        case AST_WITH:
            /* Simplified: just execute the body */
            if (node->num_children > 0) {
                gen_statement(cg, node->children[node->num_children - 1]);
            }
            break;

        case AST_CASE: {
            /* Evaluate selector into D0, save in D3 */
            gen_expression(cg, node->children[0]);
            emit16(cg, 0x3600);  /* MOVE.W D0,D3 */

            uint32_t end_fixups[128];
            int num_fixups = 0;

            /* Case branches: pairs of (label, statement) starting at child[1] */
            int ci = 1;
            while (ci + 1 < node->num_children) {
                /* Evaluate label into D0 */
                gen_expression(cg, node->children[ci]);
                emit16(cg, 0xB043);  /* CMP.W D3,D0 */
                emit16(cg, 0x6600);  /* BNE.W next_case */
                uint32_t next_pos = cg->code_size;
                emit16(cg, 0);

                /* Case body */
                gen_statement(cg, node->children[ci + 1]);

                /* BRA.W end_case */
                emit16(cg, 0x6000);
                if (num_fixups < 128) end_fixups[num_fixups++] = cg->code_size;
                emit16(cg, 0);

                /* Patch BNE to skip to here */
                patch16(cg, next_pos, (uint16_t)(cg->code_size - next_pos));

                ci += 2;
            }

            /* OTHERWISE: any remaining odd children */
            while (ci < node->num_children) {
                gen_statement(cg, node->children[ci]);
                ci++;
            }

            /* Patch all end_case branches to here */
            for (int j = 0; j < num_fixups; j++) {
                patch16(cg, end_fixups[j], (uint16_t)(cg->code_size - end_fixups[j]));
            }
            break;
        }

        case AST_DIRECTIVE:
            /* Process segment directives */
            if (node->str_val[0] == '$' && (node->str_val[1] == 'S' || node->str_val[1] == 's')) {
                /* $S segmentname — change current segment */
                const char *seg = node->str_val + 2;
                while (*seg == ' ') seg++;
                strncpy(cg->segment, seg, sizeof(cg->segment) - 1);
            }
            break;

        default:
            break;
    }
}

/* ========================================================================
 * Declaration processing
 * ======================================================================== */

static void process_var_decl(codegen_t *cg, ast_node_t *node, bool is_global) {
    type_desc_t *type = (node->num_children > 0) ? resolve_type(cg, node->children[0]) : find_type(cg, "integer");

    /* Handle multiple names: "a,b,c" */
    char names[256];
    strncpy(names, node->name, sizeof(names) - 1);
    char *tok = strtok(names, ",");
    while (tok) {
        while (*tok == ' ') tok++;
        int sz = type_size(type);
        if (is_global) {
            cg_symbol_t *s = add_global_sym(cg, tok, type);
            if (s) {
                /* Assign global offset (grow from A5) */
                static int global_offset = 0;
                if (sz >= 2 && (global_offset % 2)) global_offset++;
                s->offset = global_offset;
                global_offset += sz;
            }
        } else {
            cg_scope_t *sc = current_scope(cg);
            if (sc) {
                if (sz >= 2 && (sc->frame_size % 2)) sc->frame_size++;
                sc->frame_size += sz;
                cg_symbol_t *s = add_local(cg, tok, type, false, false);
                if (s) s->offset = -(int)sc->frame_size;
            }
        }
        tok = strtok(NULL, ",");
    }
}

static void process_declarations(codegen_t *cg, ast_node_t *node, bool is_global) {
    for (int i = 0; i < node->num_children; i++) {
        ast_node_t *child = node->children[i];
        switch (child->type) {
            case AST_CONST_DECL: {
                /* Add as a type or symbol */
                type_desc_t *t = find_type(cg, "integer");
                cg_symbol_t *s = add_global_sym(cg, child->name, t);
                if (s && child->num_children > 0) {
                    s->offset = (int)child->children[0]->int_val; /* Store value in offset */
                }
                break;
            }
            case AST_TYPE_DECL: {
                if (child->num_children > 0) {
                    type_desc_t *t = resolve_type(cg, child->children[0]);
                    if (t && child->name[0]) {
                        strncpy(t->name, child->name, sizeof(t->name) - 1);
                    }
                }
                break;
            }
            case AST_VAR_DECL:
                process_var_decl(cg, child, is_global);
                break;
            case AST_DIRECTIVE:
                gen_statement(cg, child);
                break;
            default:
                break;
        }
    }
}

static void gen_proc_or_func(codegen_t *cg, ast_node_t *node) {
    if (!node->name[0]) return;

    /* Check for EXTERNAL/FORWARD */
    if (str_eq_nocase(node->str_val, "EXTERNAL") || str_eq_nocase(node->str_val, "FORWARD")) {
        cg_symbol_t *s = add_global_sym(cg, node->name, NULL);
        if (s) {
            s->is_external = str_eq_nocase(node->str_val, "EXTERNAL");
            s->is_forward = str_eq_nocase(node->str_val, "FORWARD");
        }
        return;
    }

    /* Record entry point */
    cg_symbol_t *entry = add_global_sym(cg, node->name, NULL);
    if (entry) entry->offset = (int)cg->code_size;

    push_scope(cg);

    /* Process parameters */
    int param_offset = 8; /* After saved A6 and return address */
    for (int i = 0; i < node->num_children; i++) {
        if (node->children[i]->type == AST_PARAM_LIST) {
            ast_node_t *params = node->children[i];
            for (int j = 0; j < params->num_children; j++) {
                ast_node_t *param = params->children[j];
                bool is_var = (param->str_val[0] != '\0');
                type_desc_t *ptype = (param->num_children > 0) ? resolve_type(cg, param->children[0]) : find_type(cg, "integer");
                cg_symbol_t *s = add_local(cg, param->name, ptype, true, is_var);
                if (s) {
                    s->offset = param_offset;
                    param_offset += is_var ? 4 : (ptype ? ptype->size : 2);
                    if (param_offset % 2) param_offset++;
                }
            }
        }
    }

    /* Process local declarations */
    for (int i = 0; i < node->num_children; i++) {
        ast_node_t *child = node->children[i];
        if (child->type == AST_VAR_DECL)
            process_var_decl(cg, child, false);
    }

    /* Generate code for nested procedures/functions FIRST.
     * In Pascal, nested procs are callable from the parent's body.
     * They get their own LINK/UNLK frames and are placed before
     * the parent's code in the output. */
    for (int i = 0; i < node->num_children; i++) {
        ast_node_t *child = node->children[i];
        if (child->type == AST_PROC_DECL || child->type == AST_FUNC_DECL) {
            gen_proc_or_func(cg, child);
        }
    }

    cg_scope_t *sc = current_scope(cg);
    int frame_size = sc ? sc->frame_size : 0;

    /* Record the actual entry point AFTER nested procs are emitted.
     * Update the global symbol offset to point here, not to the
     * first nested proc's code. */
    if (entry) entry->offset = (int)cg->code_size;

    /* LINK A6,#-frame_size */
    emit16(cg, 0x4E56);
    emit16(cg, (uint16_t)(int16_t)(-frame_size));

    /* Generate body */
    for (int i = 0; i < node->num_children; i++) {
        if (node->children[i]->type == AST_BLOCK) {
            gen_statement(cg, node->children[i]);
        }
    }

    /* UNLK A6; RTS */
    emit16(cg, 0x4E5E);
    emit16(cg, 0x4E75);

    pop_scope(cg);
}

/* ========================================================================
 * Top-level generation
 * ======================================================================== */

static void gen_unit(codegen_t *cg, ast_node_t *unit) {
    /* Process INTERFACE section — just collect declarations */
    for (int i = 0; i < unit->num_children; i++) {
        if (unit->children[i]->type == AST_INTERFACE) {
            process_declarations(cg, unit->children[i], true);
        }
    }

    /* Process IMPLEMENTATION section — generate code */
    for (int i = 0; i < unit->num_children; i++) {
        if (unit->children[i]->type == AST_IMPLEMENTATION) {
            ast_node_t *impl = unit->children[i];
            process_declarations(cg, impl, true);

            /* Count and log IMPLEMENTATION functions */
            int impl_funcs = 0;
            for (int j = 0; j < impl->num_children; j++) {
                if (impl->children[j]->type == AST_PROC_DECL ||
                    impl->children[j]->type == AST_FUNC_DECL)
                    impl_funcs++;
            }
            if (impl_funcs > 0) {
                const char *bn = strrchr(cg->current_file, '/');
                bn = bn ? bn + 1 : cg->current_file;
                fprintf(stderr, "  IMPL: %s — %d procs/funcs in IMPLEMENTATION\n",
                        bn, impl_funcs);
            }

            /* Generate code for procedures/functions and methods */
            for (int j = 0; j < impl->num_children; j++) {
                if (impl->children[j]->type == AST_PROC_DECL ||
                    impl->children[j]->type == AST_FUNC_DECL) {
                    gen_proc_or_func(cg, impl->children[j]);
                } else if (impl->children[j]->type == AST_METHODS) {
                    /* METHODS OF section */
                    ast_node_t *methods = impl->children[j];
                    for (int k = 0; k < methods->num_children; k++) {
                        if (methods->children[k]->type == AST_PROC_DECL ||
                            methods->children[k]->type == AST_FUNC_DECL) {
                            gen_proc_or_func(cg, methods->children[k]);
                        }
                    }
                }
            }
        }
    }
}

static void gen_program(codegen_t *cg, ast_node_t *prog) {
    process_declarations(cg, prog, true);

    /* Emit a BRA.W forward to the main body — nested procs come first
     * but the entry point must be at offset 0 for the boot ROM.
     * BRA.W uses PC-relative displacement, no relocation needed. */
    uint32_t bra_fixup = cg->code_size;
    emit16(cg, 0x6000);  /* BRA.W */
    emit16(cg, 0x0000);  /* Placeholder displacement — patched below */

    /* Generate code for all procedures/functions */
    for (int i = 0; i < prog->num_children; i++) {
        ast_node_t *child = prog->children[i];
        if (child->type == AST_PROC_DECL || child->type == AST_FUNC_DECL) {
            gen_proc_or_func(cg, child);
        } else if (child->type == AST_METHODS) {
            for (int j = 0; j < child->num_children; j++) {
                if (child->children[j]->type == AST_PROC_DECL ||
                    child->children[j]->type == AST_FUNC_DECL) {
                    gen_proc_or_func(cg, child->children[j]);
                }
            }
        }
    }

    /* Patch the BRA.W displacement to jump to the main body.
     * BRA.W: target = PC + displacement, where PC = BRA_addr + 2.
     * So displacement = body_offset - (bra_fixup + 2). */
    uint32_t body_offset = cg->code_size;
    int16_t displacement = (int16_t)(body_offset - (bra_fixup + 2));
    cg->code[bra_fixup + 2] = (displacement >> 8) & 0xFF;
    cg->code[bra_fixup + 3] = displacement & 0xFF;

    /* Generate the main body (BEGIN...END block) */
    for (int i = 0; i < prog->num_children; i++) {
        if (prog->children[i]->type == AST_BLOCK) {
            gen_statement(cg, prog->children[i]);
        }
    }

    /* RTS at end of program (if there was executable code) */
    if (cg->code_size > 4) {  /* More than just the BRA */
        emit16(cg, 0x4E75);
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void codegen_init(codegen_t *cg) {
    memset(cg, 0, sizeof(codegen_t));
    init_builtin_types(cg);
}

void codegen_free(codegen_t *cg) {
    memset(cg, 0, sizeof(codegen_t));
}

bool codegen_generate(codegen_t *cg, ast_node_t *ast) {
    if (!ast) return false;

    switch (ast->type) {
        case AST_UNIT:
            gen_unit(cg, ast);
            break;
        case AST_PROGRAM:
            gen_program(cg, ast);
            break;
        case AST_FRAGMENT:
            /* Code fragment: treat like a program body */
            gen_program(cg, ast);
            break;
        case AST_EMPTY:
            /* Empty file (e.g., directive-only) */
            break;
        default:
            cg_error(cg, 0, "unexpected AST type at top level: %s", ast_type_name(ast->type));
            return false;
    }

    return cg->num_errors == 0;
}

const uint8_t *codegen_get_code(codegen_t *cg, uint32_t *size) {
    if (size) *size = cg->code_size;
    return cg->code;
}

bool codegen_write_obj(codegen_t *cg, const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    /* Write header */
    fwrite("LOBJ", 1, 4, f);
    uint32_t version = 1;
    fwrite(&version, 4, 1, f);
    fwrite(&cg->code_size, 4, 1, f);

    uint32_t num_syms = 0;
    for (int i = 0; i < cg->num_globals; i++) {
        if (cg->globals[i].name[0]) num_syms++;
    }
    fwrite(&num_syms, 4, 1, f);

    /* Write code */
    fwrite(cg->code, 1, cg->code_size, f);

    /* Write symbol table */
    for (int i = 0; i < cg->num_globals; i++) {
        cg_symbol_t *s = &cg->globals[i];
        if (!s->name[0]) continue;
        uint8_t type = s->is_external ? 1 : 0;
        uint8_t flags = 4; /* defined */
        uint8_t namelen = (uint8_t)strlen(s->name);
        int32_t value = s->offset;
        fwrite(&type, 1, 1, f);
        fwrite(&flags, 1, 1, f);
        fwrite(&namelen, 1, 1, f);
        fwrite(s->name, 1, namelen, f);
        fwrite(&value, 4, 1, f);
    }

    /* Write relocations */
    uint32_t num_relocs = cg->num_relocs;
    fwrite(&num_relocs, 4, 1, f);
    for (int i = 0; i < cg->num_relocs; i++) {
        cg_reloc_t *r = &cg->relocs[i];
        uint8_t namelen = (uint8_t)strlen(r->symbol);
        fwrite(&r->offset, 4, 1, f);
        fwrite(&r->size, 4, 1, f);
        fwrite(&namelen, 1, 1, f);
        fwrite(r->symbol, 1, namelen, f);
    }

    fclose(f);
    printf("Wrote: %s (%u bytes code, %d symbols, %d relocs)\n",
           filename, cg->code_size, (int)num_syms, cg->num_relocs);
    return true;
}

int codegen_get_error_count(codegen_t *cg) {
    return cg->num_errors;
}
