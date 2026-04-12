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

/* Pascal runtime-builtin procedures that should NEVER emit an external JSR.
 * The Lisa OS Pascal runtime provides these through compiler-generated
 * `%_NAME` symbols that we don't ship; emit an inline stub instead so the
 * linker doesn't report false unresolved references. The list is the same
 * set handled inline in gen_expression()'s AST_FUNC_CALL for intrinsics
 * that also appear as bare statement calls.
 *
 * Anything in this list is emitted as:
 *   for each arg: gen_expression (side effects)
 *   (no JSR, no relocation)
 *
 * Returning true means the caller should NOT emit a call. */
static bool is_pascal_runtime_stub_proc(const char *name);

static bool str_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

static bool is_pascal_runtime_stub_proc(const char *name) {
    if (!name || !*name) return false;
    /* Console I/O — we have no real console so these are no-ops. */
    if (str_eq_nocase(name, "WRITE")) return true;
    if (str_eq_nocase(name, "WRITELN")) return true;
    if (str_eq_nocase(name, "READ")) return true;
    if (str_eq_nocase(name, "READLN")) return true;
    if (str_eq_nocase(name, "PAGE")) return true;
    /* Termination. */
    if (str_eq_nocase(name, "HALT")) return true;
    /* Debug trace — never wired to anything real. */
    if (str_eq_nocase(name, "LogCall")) return true;
    if (str_eq_nocase(name, "LogSeg")) return true;
    return false;
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

/* Ensure code is word-aligned (68000 requires even PC for instructions) */
static void align_code(codegen_t *cg) {
    if (cg->code_size & 1) {
        emit8(cg, 0);  /* Single pad byte to make even */
    }
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
    /* Also search imported types from previously compiled units */
    if (cg->imported_types) {
        for (int i = 0; i < cg->imported_types_count; i++) {
            if (str_eq_nocase(cg->imported_types[i].name, name))
                return &cg->imported_types[i];
        }
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

/* Forward declarations for symbol lookup (used in resolve_type for CONST) */
static cg_symbol_t *find_global(codegen_t *cg, const char *name);
static cg_symbol_t *find_imported(codegen_t *cg, const char *name);
static void register_proc_sig(codegen_t *cg, const char *name, ast_node_t *params[], int num_params);
static cg_proc_sig_t *find_proc_sig(codegen_t *cg, const char *name);

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
            int str_len = (int)node->int_val;
            /* Resolve CONST identifier for string length: string[max_ename] */
            if (str_len == 0 && node->name[0]) {
                /* Look up the constant value by name */
                cg_symbol_t *csym = find_global(cg, node->name);
                if (!csym) csym = find_imported(cg, node->name);
                if (csym) str_len = csym->offset; /* CONST value stored in offset */
                if (str_len <= 0) str_len = 255;  /* Fallback to max string */
            }
            type_desc_t *t = add_type(cg, "", TK_STRING, str_len + 1);
            t->max_length = str_len;
            return t;
        }

        case AST_TYPE_SUBRANGE: {
            type_desc_t *t = add_type(cg, "", TK_SUBRANGE, 2);
            if (node->num_children >= 2) {
                /* Resolve CONST identifiers in subrange bounds */
                t->range_low = (int)node->children[0]->int_val;
                t->range_high = (int)node->children[1]->int_val;
                for (int bi = 0; bi < 2; bi++) {
                    ast_node_t *bound = node->children[bi];
                    if (bound->type == AST_IDENT_EXPR && bound->int_val == 0 && bound->name[0]) {
                        cg_symbol_t *cs = find_global(cg, bound->name);
                        if (!cs) cs = find_imported(cg, bound->name);
                        if (cs) { if (bi==0) t->range_low = cs->offset; else t->range_high = cs->offset; }
                    }
                }
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
            if (node->num_children < 3) {
                /* Handle ARRAY[TypeName] OF ElemType (2 children: index type + element type) */
                if (node->num_children == 2) {
                    t->element_type = resolve_type(cg, node->children[1]);
                    /* Try to resolve bounds from the index type name */
                    if (node->children[0]->type == AST_IDENT_EXPR) {
                        type_desc_t *idx_type = find_type(cg, node->children[0]->name);
                        if (idx_type && idx_type->kind == TK_SUBRANGE) {
                            t->array_low = idx_type->range_low;
                            t->array_high = idx_type->range_high;
                        } else if (idx_type && idx_type->kind == TK_ENUM) {
                            t->array_low = 0;
                            t->array_high = idx_type->size > 0 ? idx_type->size - 1 : 0;
                        }
                    }
                    int count = t->array_high - t->array_low + 1;
                    if (count <= 0) count = 64;
                    int elem_sz = t->element_type ? t->element_type->size : 2;
                    t->size = count * elem_sz;
                    if (t->size < 2) t->size = 2;
                }
            } else
            if (node->num_children >= 3) {
                /* Resolve array bounds — may be CONST identifiers */
                int lo = (int)node->children[0]->int_val;
                int hi = (int)node->children[1]->int_val;
                if (lo == 0 && node->children[0]->name[0]) {
                    cg_symbol_t *cs = find_global(cg, node->children[0]->name);
                    if (!cs) cs = find_imported(cg, node->children[0]->name);
                    if (cs && cs->is_const) lo = cs->offset;
                }
                if (hi == 0 && node->children[1]->name[0]) {
                    cg_symbol_t *cs = find_global(cg, node->children[1]->name);
                    if (!cs) cs = find_imported(cg, node->children[1]->name);
                    if (cs && cs->is_const) hi = cs->offset;
                }
                t->array_low = lo;
                t->array_high = hi;
                t->element_type = resolve_type(cg, node->children[node->num_children - 1]);
                int count = t->array_high - t->array_low + 1;
                if (count <= 0) {
                    count = 64;  /* Default for unresolved CONST bounds */
                    static int unresolved_warn = 0;
                    if (unresolved_warn++ < 5)
                        fprintf(stderr, "  WARN: array bounds unresolved (lo=%d hi=%d), defaulting to %d elements\n", lo, hi, count);
                }
                int elem_sz = t->element_type ? t->element_type->size : 2;
                t->size = count * elem_sz;
                if (t->size < 2) t->size = 2;  /* Minimum 2 bytes */
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

        case AST_TYPE_CLASS: {
            /* Clascal SUBCLASS OF — objects are pointer-sized (4 bytes).
             * Register as a pointer type so TYPE_NAME(expr) casts work. */
            type_desc_t *t = add_type(cg, "", TK_POINTER, 4);
            return t;
        }

        case AST_METHODS: {
            /* METHODS OF classname — method declarations, no type to create */
            return find_type(cg, "integer");
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

/* Find local variable and return its scope depth.
 * out_depth is set to the scope level where the variable was found
 * (0 = innermost/current, 1 = one level up, etc.) */
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

static int find_local_depth(codegen_t *cg, const char *name) {
    for (int d = cg->scope_depth - 1; d >= 0; d--) {
        cg_scope_t *sc = &cg->scopes[d];
        for (int i = 0; i < sc->num_locals; i++) {
            if (str_eq_nocase(sc->locals[i].name, name))
                return (cg->scope_depth - 1) - d;  /* 0=current, 1=parent, etc. */
        }
    }
    return -1;
}

/* Emit code to load the frame pointer for a variable at a given nesting depth.
 * depth=0: current scope → A6 is correct
 * depth=1: parent scope → A0 = (A6) (saved A6 from LINK)
 * depth=2: grandparent → A0 = ((A6))
 * Result in A6 (for depth 0) or A0 (for depth > 0). */
static void emit_frame_access(codegen_t *cg, int depth) {
    if (depth <= 0) return;  /* Current scope, A6 is fine */
    /* Follow static link chain through saved A6 values */
    emit16(cg, 0x2056);  /* MOVEA.L (A6),A0 — get parent's A6 */
    for (int i = 1; i < depth; i++) {
        emit16(cg, 0x2050);  /* MOVEA.L (A0),A0 — follow chain */
    }
}

static cg_symbol_t *find_global(codegen_t *cg, const char *name) {
    for (int i = 0; i < cg->num_globals; i++) {
        if (str_eq_nocase(cg->globals[i].name, name))
            return &cg->globals[i];
    }
    return NULL;
}

static cg_symbol_t *find_imported(codegen_t *cg, const char *name) {
    if (!cg->imported_globals) return NULL;
    for (int i = 0; i < cg->imported_globals_count; i++) {
        if (str_eq_nocase(cg->imported_globals[i].name, name))
            return &cg->imported_globals[i];
    }
    return NULL;
}

static cg_symbol_t *find_symbol_any(codegen_t *cg, const char *name) {
    cg_symbol_t *s = find_local(cg, name);
    if (s) {
        if (strcasecmp(name, "fp_ptr") == 0)
            fprintf(stderr, "  LOOKUP fp_ptr: FOUND LOCAL at offset=%d is_param=%d scope=%d in '%s'\n",
                    s->offset, s->is_param, cg->scope_depth, cg->current_file);
        return s;
    }
    if (strcasecmp(name, "fp_ptr") == 0) {
        fprintf(stderr, "  LOOKUP fp_ptr: NOT LOCAL (scope=%d), checking global/imported in '%s'\n",
                cg->scope_depth, cg->current_file);
    }
    s = find_global(cg, name);
    if (s) return s;
    return find_imported(cg, name);
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
    /* Check for existing symbol with same name — avoid duplicates that
     * confuse the linker. Interface + external declarations for the same
     * proc would otherwise create two entries, the first (ENTRY, val=0)
     * blocking the real implementation via "first ENTRY wins". */
    for (int i = 0; i < cg->num_globals; i++) {
        if (str_eq_nocase(cg->globals[i].name, name)) {
            if (type) cg->globals[i].type = type;
            return &cg->globals[i];
        }
    }
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
static void gen_lvalue_addr(codegen_t *cg, ast_node_t *node);

/* Generate an expression whose result will be used as a pointer (32-bit).
 * Calls gen_expression then retroactively patches any 16-bit MOVE to D0
 * into a 32-bit MOVE to D0, so that the subsequent MOVEA.L D0,A0 gets
 * the full 32-bit value instead of a sign-extended 16-bit value. */
static void gen_ptr_expression(codegen_t *cg, ast_node_t *node) {
    uint32_t before = cg->code_size;
    gen_expression(cg, node);
    uint32_t after = cg->code_size;
    /* Check if the emitted code ends with a 16-bit MOVE to D0 that we
     * can safely widen to 32-bit. Pattern: 0x30xx xxxx (4 bytes) where
     * the high byte 0x30 means MOVE.W <ea>,D0.
     * 0x302E = MOVE.W disp(A6),D0 → 0x202E = MOVE.L disp(A6),D0
     * 0x3028 = MOVE.W disp(A0),D0 → 0x2028 = MOVE.L disp(A0),D0
     * 0x302D = MOVE.W disp(A5),D0 → 0x202D = MOVE.L disp(A5),D0
     * 0x303C = MOVE.W #imm,D0     — need to widen to MOVE.L #imm,D0
     *   but this changes instruction size, so skip and use EXT.L instead.
     */
    if (after - before >= 4) {
        uint16_t opword = (cg->code[after - 4] << 8) | cg->code[after - 3];
        if (opword == 0x302E || opword == 0x3028 || opword == 0x302D) {
            /* Patch 0x30xx to 0x20xx (MOVE.W → MOVE.L) */
            cg->code[after - 4] = 0x20;
        }
    }
    /* If we couldn't patch (e.g. result came from MOVEQ, register op,
     * MOVE.W #imm, or a complex expression), the value in D0 may be
     * 16-bit. Sign-extend is wrong for pointers; we need the full value.
     * But if the emitted code already produced a MOVE.L or MOVEQ, this
     * is harmless — SWAP+CLR+SWAP on a full-width value would corrupt it.
     * So only emit an extend if we KNOW we have a 16-bit result. */
    /* For now, the opword patch handles the dominant case (frame/global
     * MOVE.W → MOVE.L). The type_load_size fix handles TK_POINTER types
     * at source. Together they should cover nearly all pointer loads. */
}

/* Check if an identifier is a field of an active WITH record.
 * Returns the field index and sets *out_type to the record type,
 * or returns -1 if not found. Searches from innermost WITH outward. */
static int with_lookup_field(codegen_t *cg, const char *name,
                             type_desc_t **out_type, int *out_with_idx) {
    for (int w = cg->with_depth - 1; w >= 0; w--) {
        type_desc_t *rt = cg->with_stack[w].record_type;
        if (!rt || rt->kind != TK_RECORD) continue;
        for (int fi = 0; fi < rt->num_fields; fi++) {
            if (str_eq_nocase(rt->fields[fi].name, name)) {
                if (out_type) *out_type = rt;
                if (out_with_idx) *out_with_idx = w;
                return fi;
            }
        }
    }
    return -1;
}

/* Generate code to load WITH record base address into A0.
 * This evaluates the record expression from the WITH context. */
static void gen_with_base(codegen_t *cg, int with_idx) {
    ast_node_t *expr = cg->with_stack[with_idx].record_expr;
    if (!expr) return;
    /* The record expression is typically a variable or pointer deref.
     * For pointer deref (ptr^): evaluate pointer, load into A0.
     * For variable: get its address into A0. */
    if (expr->type == AST_DEREF) {
        /* WITH ptr^ DO ... → evaluate ptr, move to A0 */
        gen_ptr_expression(cg, expr->children[0]);
        emit16(cg, 0x2040);  /* MOVEA.L D0,A0 */
    } else {
        /* WITH var DO ... → LEA var,A0 */
        gen_lvalue_addr(cg, expr);
    }
}

/* Return the correct load size for a type descriptor.
 * Pointers are ALWAYS 4 bytes regardless of what type->size says —
 * some types get created with size=2 before being resolved to pointers. */
static int type_load_size(type_desc_t *t) {
    if (!t) return 2;
    if (t->kind == TK_POINTER) return 4;
    if (t->kind == TK_LONGINT) return 4;
    if (t->kind == TK_PROC || t->kind == TK_FUNC) return 4; /* procedure/function pointers */
    return t->size > 0 ? t->size : 2;
}

/* Determine the byte size of an expression's result type.
 * Returns 1 (byte), 2 (word), or 4 (long). Used for size-aware MOVE,
 * arithmetic, and parameter passing throughout the codegen. */
static int expr_size(codegen_t *cg, ast_node_t *node) {
    if (!node) return 2;
    switch (node->type) {
        case AST_INT_LITERAL:
            return (node->int_val < -32768 || node->int_val > 32767) ? 4 : 2;
        case AST_STRING_LITERAL:
            return 4; /* pointer to string */
        case AST_ADDR_OF:
            return 4; /* pointer */
        case AST_IDENT_EXPR: {
            cg_symbol_t *sym = find_symbol_any(cg, node->name);
            if (sym && sym->type) return type_load_size(sym->type);
            if (sym && sym->is_const) return 2;
            /* Check built-in identifiers */
            if (str_eq_nocase(node->name, "nil")) return 4;
            if (str_eq_nocase(node->name, "true") || str_eq_nocase(node->name, "false")) return 2;
            /* Check WITH context for field size */
            if (cg->with_depth > 0) {
                type_desc_t *wrt = NULL;
                int fld = with_lookup_field(cg, node->name, &wrt, NULL);
                if (fld >= 0 && wrt && wrt->fields[fld].type)
                    return type_load_size(wrt->fields[fld].type);
            }
            return 2;
        }
        case AST_FIELD_ACCESS: {
            /* Look up the field type in the record */
            if (node->children[0] && node->children[0]->type == AST_IDENT_EXPR) {
                cg_symbol_t *rec = find_symbol_any(cg, node->children[0]->name);
                if (rec && rec->type) {
                    type_desc_t *rt = rec->type;
                    if (rt->kind == TK_POINTER && rt->base_type) rt = rt->base_type;
                    if (rt->kind == TK_RECORD) {
                        for (int i = 0; i < rt->num_fields; i++) {
                            if (str_eq_nocase(rt->fields[i].name, node->name)) {
                                if (rt->fields[i].type)
                                    return type_load_size(rt->fields[i].type);
                                break;
                            }
                        }
                    }
                }
            }
            return 2;
        }
        case AST_ARRAY_ACCESS: {
            if (node->children[0] && node->children[0]->type == AST_IDENT_EXPR) {
                cg_symbol_t *arr = find_symbol_any(cg, node->children[0]->name);
                if (arr && arr->type) {
                    type_desc_t *at = arr->type;
                    if (at->kind == TK_POINTER && at->base_type) at = at->base_type;
                    if (at->kind == TK_ARRAY && at->element_type)
                        return type_load_size(at->element_type);
                    if (at->kind == TK_STRING)
                        return 1;  /* string subscript returns a char */
                }
            }
            return 2;
        }
        case AST_DEREF: {
            if (node->children[0] && node->children[0]->type == AST_IDENT_EXPR) {
                cg_symbol_t *ptr = find_symbol_any(cg, node->children[0]->name);
                if (ptr && ptr->type && ptr->type->kind == TK_POINTER && ptr->type->base_type)
                    return type_load_size(ptr->type->base_type);
            }
            return 2;
        }
        case AST_FUNC_CALL: {
            /* Check known intrinsics for correct return size */
            if (node->name[0]) {
                if (str_eq_nocase(node->name, "ORD4")) return 4;
                if (str_eq_nocase(node->name, "POINTER")) return 4;
                if (str_eq_nocase(node->name, "ORD")) {
                    /* ORD on a pointer/longint argument returns longint (4 bytes) */
                    if (node->num_children > 0) {
                        int arg_sz = expr_size(cg, node->children[0]);
                        if (arg_sz >= 4) return 4;
                    }
                    return 2; /* ORD on integer/boolean returns integer */
                }
                if (str_eq_nocase(node->name, "SIZEOF")) return 2;
                if (str_eq_nocase(node->name, "CHR")) return 2;
                if (str_eq_nocase(node->name, "ABS")) {
                    if (node->num_children > 0) return expr_size(cg, node->children[0]);
                    return 2;
                }
                /* Look up function return type */
                cg_symbol_t *fsym = find_symbol_any(cg, node->name);
                if (fsym && fsym->type) return type_load_size(fsym->type);
            }
            return 2; /* Default to word */
        }
        case AST_BINARY_OP:
        case AST_UNARY_OP: {
            /* Propagate from operands */
            int s = 2;
            for (int i = 0; i < node->num_children; i++) {
                int cs = expr_size(cg, node->children[i]);
                if (cs > s) s = cs;
            }
            return s;
        }
        default:
            return 2;
    }
}

/* Emit size-appropriate MOVE (A0),D0 — reads value from address in A0 */
static void emit_read_a0_to_d0(codegen_t *cg, int sz) {
    if (sz == 4)      emit16(cg, 0x2010);  /* MOVE.L (A0),D0 */
    else if (sz == 1) emit16(cg, 0x1010);  /* MOVE.B (A0),D0 */
    else              emit16(cg, 0x3010);  /* MOVE.W (A0),D0 */
}

/* Emit size-appropriate MOVE D0,(A0) — stores value from D0 to address in A0 */
static void emit_write_d0_to_a0(codegen_t *cg, int sz) {
    if (sz == 4)      emit16(cg, 0x2080);  /* MOVE.L D0,(A0) */
    else if (sz == 1) emit16(cg, 0x1080);  /* MOVE.B D0,(A0) */
    else              emit16(cg, 0x3080);  /* MOVE.W D0,(A0) */
}

/* Emit size-appropriate MOVE D1,(A0) */
static void emit_write_d1_to_a0(codegen_t *cg, int sz) {
    if (sz == 4)      emit16(cg, 0x2081);  /* MOVE.L D1,(A0) */
    else if (sz == 1) emit16(cg, 0x1081);  /* MOVE.B D1,(A0) */
    else              emit16(cg, 0x3081);  /* MOVE.W D1,(A0) */
}

/* Load a variable's address into A0 */
static void gen_lvalue_addr(codegen_t *cg, ast_node_t *node) {
    if (node->type == AST_IDENT_EXPR) {
        cg_symbol_t *sym = find_symbol_any(cg, node->name);
        if (sym) {
            if (sym->is_param && sym->is_var_param) {
                /* VAR param: frame + offset contains pointer */
                int depth = find_local_depth(cg, node->name);
                if (depth > 0) {
                    emit_frame_access(cg, depth);  /* parent FP → A0 */
                    emit16(cg, 0x2068);  /* MOVEA.L offset(A0),A0 */
                } else {
                    emit16(cg, 0x206E);  /* MOVEA.L offset(A6),A0 */
                }
                emit16(cg, (uint16_t)(int16_t)sym->offset);
            } else if (sym->is_param || !sym->is_global) {
                /* Local/param: LEA offset(A6),A0 or offset(A0) for outer scope */
                int depth = find_local_depth(cg, node->name);
                if (depth > 0) {
                    emit_frame_access(cg, depth);
                    emit16(cg, 0x41E8);  /* LEA offset(A0),A0 */
                } else {
                    emit16(cg, 0x41EE);  /* LEA offset(A6),A0 */
                }
                emit16(cg, (uint16_t)(int16_t)sym->offset);
            } else {
                /* Global: LEA offset(A5),A0 */
                emit16(cg, 0x41ED);
                emit16(cg, (uint16_t)(int16_t)sym->offset);
            }
        } else if (cg->with_depth > 0) {
            /* Check WITH context for field name */
            type_desc_t *wrt = NULL;
            int widx = -1;
            int fld = with_lookup_field(cg, node->name, &wrt, &widx);
            if (fld >= 0 && wrt) {
                gen_with_base(cg, widx);
                int foff = wrt->fields[fld].offset;
                if (foff != 0) {
                    emit16(cg, 0xD0FC);  /* ADDA.W #offset,A0 */
                    emit16(cg, (uint16_t)(int16_t)foff);
                }
            } else {
                emit16(cg, 0x41F9);  /* LEA abs.L,A0 */
                emit32(cg, 0);
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
            /* Resolve element size from the array type */
            int elem_size = 2;  /* default word */
            int array_low = 0;
            if (node->children[0]->type == AST_IDENT_EXPR) {
                cg_symbol_t *arr_sym = find_symbol_any(cg, node->children[0]->name);
                if (arr_sym && arr_sym->type) {
                    type_desc_t *at = arr_sym->type;
                    /* Follow pointer to get the array type */
                    if (at->kind == TK_POINTER && at->base_type)
                        at = at->base_type;
                    if (at->kind == TK_ARRAY) {
                        array_low = at->array_low;
                        if (at->element_type)
                            elem_size = type_size(at->element_type);
                        else {
                            static int arr_warn = 0;
                            if (arr_warn++ < 20)
                                fprintf(stderr, "  ARRAY_ACCESS: '%s' kind=TK_ARRAY but element_type=NULL (elem_size defaults to 2) in %s\n",
                                        node->children[0]->name, cg->current_file);
                        }
                    } else if (at->kind == TK_STRING) {
                        /* String subscript: s[i] accesses a character (1 byte) */
                        elem_size = 1;
                    } else {
                        static int arr_warn2 = 0;
                        if (arr_warn2++ < 20)
                            fprintf(stderr, "  ARRAY_ACCESS: '%s' type->kind=%d (not TK_ARRAY=%d) (elem_size defaults to 2) in %s\n",
                                    node->children[0]->name, at->kind, TK_ARRAY, cg->current_file);
                    }
                } else {
                    static int arr_warn3 = 0;
                    if (arr_warn3++ < 20)
                        fprintf(stderr, "  ARRAY_ACCESS: '%s' sym=%p type=%p (elem_size defaults to 2) in %s\n",
                                node->children[0]->name, (void*)arr_sym, arr_sym ? (void*)arr_sym->type : NULL, cg->current_file);
                }
            }
            /* SUB.W #low,D0 (adjust for array base) */
            if (array_low != 0) {
                emit16(cg, 0x0440);  /* SUBI.W #imm,D0 */
                emit16(cg, (uint16_t)(int16_t)array_low);
            }
            /* MOVE.W D0,D1; MULU #elemsize,D1; ADDA.L D1,A0 */
            emit16(cg, 0x3200);  /* MOVE.W D0,D1 */
            emit16(cg, 0xC2FC);  /* MULU #imm,D1 */
            emit16(cg, (uint16_t)elem_size);
            emit16(cg, 0xD1C1);  /* ADDA.L D1,A0 */
        }
    } else if (node->type == AST_FIELD_ACCESS) {
        gen_lvalue_addr(cg, node->children[0]); /* record addr in A0 */
        /* Look up the field offset from the record type */
        int field_off = 0;
        if (node->children[0]->type == AST_IDENT_EXPR) {
            cg_symbol_t *rec_sym = find_symbol_any(cg, node->children[0]->name);
            if (rec_sym && rec_sym->type && rec_sym->type->kind == TK_RECORD) {
                for (int fi = 0; fi < rec_sym->type->num_fields; fi++) {
                    if (str_eq_nocase(rec_sym->type->fields[fi].name, node->name)) {
                        field_off = rec_sym->type->fields[fi].offset;
                        break;
                    }
                }
            } else if (rec_sym && rec_sym->type && rec_sym->type->kind == TK_POINTER &&
                       rec_sym->type->base_type && rec_sym->type->base_type->kind == TK_RECORD) {
                /* Pointer to record — look up field in the pointed-to record */
                type_desc_t *rt = rec_sym->type->base_type;
                for (int fi = 0; fi < rt->num_fields; fi++) {
                    if (str_eq_nocase(rt->fields[fi].name, node->name)) {
                        field_off = rt->fields[fi].offset;
                        break;
                    }
                }
            }
        } else if (node->children[0]->type == AST_DEREF) {
            /* p^.field — the deref child has the pointer variable */
            ast_node_t *ptr_node = node->children[0]->children[0];
            if (ptr_node && ptr_node->type == AST_IDENT_EXPR) {
                cg_symbol_t *ptr_sym = find_symbol_any(cg, ptr_node->name);
                if (ptr_sym && ptr_sym->type && ptr_sym->type->kind == TK_POINTER &&
                    ptr_sym->type->base_type && ptr_sym->type->base_type->kind == TK_RECORD) {
                    type_desc_t *rt = ptr_sym->type->base_type;
                    for (int fi = 0; fi < rt->num_fields; fi++) {
                        if (str_eq_nocase(rt->fields[fi].name, node->name)) {
                            field_off = rt->fields[fi].offset;
                            break;
                        }
                    }
                }
            }
        }
        /* ADDA.W #offset,A0 */
        if (field_off != 0) {
            emit16(cg, 0xD0FC);  /* ADDA.W #imm,A0 */
            emit16(cg, (uint16_t)(int16_t)field_off);
        }
        /* If offset is 0, no ADDA needed */
    } else if (node->type == AST_DEREF) {
        gen_ptr_expression(cg, node->children[0]); /* pointer value in D0 */
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
            /* Check if this identifier is a callable proc/func */
            cg_proc_sig_t *ident_sig = find_proc_sig(cg, node->name);
            if (ident_sig && ident_sig->num_params == 0) {
                if (!ident_sig->is_function) {
                    /* Procedure name in expression context → push its ADDRESS.
                     * In Lisa Pascal, INITSYS(PASCALINIT) passes PASCALINIT's
                     * address as a ptr, not calling it. Procedures don't return
                     * values, so using one in an expression means "its address". */
                    emit16(cg, 0x203C);  /* MOVE.L #imm,D0 */
                    emit32(cg, 0);       /* Placeholder — will be relocated */
                    if (cg->num_relocs < CODEGEN_MAX_RELOCS) {
                        cg_reloc_t *r = &cg->relocs[cg->num_relocs++];
                        r->offset = cg->code_size - 4;
                        strncpy(r->symbol, node->name, sizeof(r->symbol) - 1);
                        r->size = 4;
                        r->pc_relative = false;
                    }
                    break;
                }
                /* Zero-arg function call.
                 * For external (assembly) functions: push space for return value
                 * on stack before JSR. Assembly function puts result there.
                 * For Pascal functions: result comes back in D0. */
                if (ident_sig->is_external) {
                    emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) — reserve space for result */
                }
                emit16(cg, 0x4EB9);  /* JSR abs.L */
                emit32(cg, 0);       /* Placeholder — will be relocated */
                if (cg->num_relocs < CODEGEN_MAX_RELOCS) {
                    cg_reloc_t *r = &cg->relocs[cg->num_relocs++];
                    r->offset = cg->code_size - 4;
                    strncpy(r->symbol, node->name, sizeof(r->symbol) - 1);
                    r->size = 4;
                    r->pc_relative = false;
                }
                if (ident_sig->is_external) {
                    emit16(cg, 0x201F);  /* MOVE.L (SP)+,D0 — pop result */
                }
                break;
            }
            cg_symbol_t *sym = find_symbol_any(cg, node->name);
            if (sym) {
                if (sym->is_const) {
                    /* CONST: emit immediate value */
                    int val = sym->offset;
                    if (val >= -128 && val <= 127) {
                        emit16(cg, 0x7000 | (val & 0xFF));  /* MOVEQ #val,D0 */
                    } else {
                        emit16(cg, 0x203C);  /* MOVE.L #imm,D0 */
                        emit32(cg, (uint32_t)val);
                    }
                } else if (sym->is_param && sym->is_var_param) {
                    /* VAR param: dereference pointer at frame + offset */
                    int depth = find_local_depth(cg, node->name);
                    if (depth > 0) {
                        emit_frame_access(cg, depth);  /* parent FP → A0 */
                        emit16(cg, 0x2068);  /* MOVEA.L offset(A0),A0 */
                    } else {
                        emit16(cg, 0x206E);  /* MOVEA.L offset(A6),A0 */
                    }
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                    /* Size-aware dereference of VAR parameter */
                    emit_read_a0_to_d0(cg, type_load_size(sym->type));
                } else if (sym->is_param || !sym->is_global) {
                    /* Local/param: size-aware load from stack frame.
                     * For outer-scope variables, follow frame chain first. */
                    if (sym->is_param && !sym->is_var_param) {
                        static int vp_warn = 0;
                        if (vp_warn++ < 30)
                            fprintf(stderr, "  [VARDIAG] PARAM '%s' at +%d type=%s sz=%d is_var=0 in %s (code_off=$%X)\n",
                                    node->name, sym->offset,
                                    sym->type ? sym->type->name : "NULL",
                                    sym->type ? sym->type->size : -1,
                                    cg->current_file, cg->code_size);
                    }
                    int depth = find_local_depth(cg, node->name);
                    int sz = type_load_size(sym->type);
                    if (depth > 0) {
                        /* Outer scope: follow static link chain into A0, then use A0 */
                        emit_frame_access(cg, depth);
                        if (sz == 4)      emit16(cg, 0x2028);  /* MOVE.L offset(A0),D0 */
                        else if (sz == 1) emit16(cg, 0x1028);  /* MOVE.B offset(A0),D0 */
                        else              emit16(cg, 0x3028);  /* MOVE.W offset(A0),D0 */
                    } else {
                        if (sz == 4)      emit16(cg, 0x202E);  /* MOVE.L offset(A6),D0 */
                        else if (sz == 1) emit16(cg, 0x102E);  /* MOVE.B offset(A6),D0 */
                        else              emit16(cg, 0x302E);  /* MOVE.W offset(A6),D0 */
                    }
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                } else {
                    /* Global: size-aware load from A5 */
                    int sz = type_load_size(sym->type);
                    if (sz == 4) {
                        emit16(cg, 0x202D);
                    } else if (sz == 1) {
                        emit16(cg, 0x102D);
                    } else {
                        emit16(cg, 0x302D);  /* MOVE.W offset(A5),D0 */
                    }
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                }
            } else if (cg->with_depth > 0) {
                /* Check WITH context: is this identifier a field of an active WITH record? */
                type_desc_t *wrt = NULL;
                int widx = -1;
                int fld = with_lookup_field(cg, node->name, &wrt, &widx);
                if (fld >= 0 && wrt) {
                    /* Generate: load WITH record base into A0, add field offset, read */
                    gen_with_base(cg, widx);
                    int foff = wrt->fields[fld].offset;
                    if (foff != 0) {
                        emit16(cg, 0xD0FC);  /* ADDA.W #offset,A0 */
                        emit16(cg, (uint16_t)(int16_t)foff);
                    }
                    int fsz = type_load_size(wrt->fields[fld].type);
                    emit_read_a0_to_d0(cg, fsz);
                } else {
                    /* Not a WITH field — fall through to unknown/constant */
                    emit16(cg, 0x303C);  /* MOVE.W #0,D0 placeholder */
                    emit16(cg, 0);
                }
            } else {
                /* Check for built-in constants */
                if (str_eq_nocase(node->name, "true") || str_eq_nocase(node->name, "TRUE")) {
                    emit16(cg, 0x7001);  /* MOVEQ #1,D0 */
                } else if (str_eq_nocase(node->name, "false") || str_eq_nocase(node->name, "FALSE")) {
                    emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
                } else if (str_eq_nocase(node->name, "nil") || str_eq_nocase(node->name, "NIL")) {
                    emit16(cg, 0x7000);  /* MOVEQ #0,D0 (nil = 0) */
                } else if (str_eq_nocase(node->name, "maxint") || str_eq_nocase(node->name, "MAXINT")) {
                    emit16(cg, 0x303C);  /* MOVE.W #32767,D0 */
                    emit16(cg, 0x7FFF);
                } else {
                    /* Unknown — treat as external, emit 0 */
                    emit16(cg, 0x303C);  /* MOVE.W #0,D0 placeholder */
                    emit16(cg, 0);
                }
            }
            break;
        }

        case AST_UNARY_OP:
            gen_expression(cg, node->children[0]);
            if (node->op == TOK_MINUS) {
                int sz = expr_size(cg, node->children[0]);
                emit16(cg, (sz == 4) ? 0x4480 : 0x4440);  /* NEG.L/W D0 */
            } else if (node->op == TOK_NOT) {
                int sz = expr_size(cg, node->children[0]);
                emit16(cg, (sz == 4) ? 0x4680 : 0x4640);  /* NOT.L/W D0 */
            }
            break;

        case AST_BINARY_OP: {
            /* Determine if 32-bit operation needed */
            int opsz = expr_size(cg, node);
            bool use_long = (opsz == 4);

            gen_expression(cg, node->children[0]);
            /* Save left in D2 */
            emit16(cg, use_long ? 0x2400 : 0x3400);  /* MOVE.L/W D0,D2 */
            gen_expression(cg, node->children[1]);
            /* D2 = left, D0 = right */
            emit16(cg, use_long ? 0x2200 : 0x3200);  /* MOVE.L/W D0,D1 */
            emit16(cg, use_long ? 0x2002 : 0x3002);  /* MOVE.L/W D2,D0 */

            switch (node->op) {
                case TOK_PLUS:
                    emit16(cg, use_long ? 0xD081 : 0xD041);  /* ADD.L/W D1,D0 */
                    break;
                case TOK_MINUS:
                    emit16(cg, use_long ? 0x9081 : 0x9041);  /* SUB.L/W D1,D0 */
                    break;
                case TOK_STAR:
                    if (use_long) {
                        /* 32-bit multiply: D0.L * D1.L → D0.L
                         * M68000 only has 16×16→32 MULU/MULS.
                         * Algorithm: result = A_lo*B_lo + (A_hi*B_lo + A_lo*B_hi)<<16
                         * (A_hi*B_hi<<32 overflows and is discarded) */
                        emit16(cg, 0x2400);  /* MOVE.L D0,D2     ; D2 = A */
                        emit16(cg, 0x2F01);  /* MOVE.L D1,-(SP)  ; save B */
                        emit16(cg, 0x4840);  /* SWAP D0           ; D0.w = A_hi */
                        emit16(cg, 0xC0C1);  /* MULU.W D1,D0     ; D0 = A_hi * B_lo */
                        emit16(cg, 0x4841);  /* SWAP D1           ; D1.w = B_hi */
                        emit16(cg, 0xC2C2);  /* MULU.W D2,D1     ; D1 = A_lo * B_hi */
                        emit16(cg, 0xD041);  /* ADD.W D1,D0      ; sum cross products */
                        emit16(cg, 0x4840);  /* SWAP D0           ; cross sum → high word */
                        emit16(cg, 0x4240);  /* CLR.W D0          ; clear low word */
                        emit16(cg, 0x221F);  /* MOVE.L (SP)+,D1  ; restore B */
                        emit16(cg, 0xC4C1);  /* MULU.W D1,D2     ; D2 = A_lo * B_lo */
                        emit16(cg, 0xD082);  /* ADD.L D2,D0      ; final result */
                    } else {
                        emit16(cg, 0xC1C1);  /* MULS.W D1,D0 */
                    }
                    break;
                case TOK_SLASH:
                case TOK_DIV:
                    if (use_long) {
                        /* 32-bit unsigned division: D0.L / D1.L → D0.L
                         * Uses binary long division (shift-and-subtract).
                         * D0=dividend → quotient, D2=remainder, D3=counter */
                        emit16(cg, 0x4282);  /* CLR.L D2           ; remainder=0 */
                        emit16(cg, 0x761F);  /* MOVEQ #31,D3       ; 32 iterations */
                        /* loop: */
                        emit16(cg, 0xD080);  /* ADD.L D0,D0        ; dividend <<= 1 */
                        emit16(cg, 0xD582);  /* ADDX.L D2,D2       ; remainder <<= 1 + carry */
                        emit16(cg, 0xB481);  /* CMP.L D1,D2        ; remainder >= divisor? */
                        emit16(cg, 0x6504);  /* BCS.S +4           ; skip if remainder < divisor */
                        emit16(cg, 0x9481);  /* SUB.L D1,D2        ; remainder -= divisor */
                        emit16(cg, 0x5280);  /* ADDQ.L #1,D0       ; set quotient bit */
                        /* .skip: */
                        emit16(cg, 0x51CB);  /* DBRA D3,offset     */
                        emit16(cg, 0xFFF2);  /* displacement = -14  (back to ADD.L D0,D0) */
                    } else {
                        emit16(cg, 0x48C0);  /* EXT.L D0 */
                        emit16(cg, 0x81C1);  /* DIVS D1,D0 */
                    }
                    break;
                case TOK_MOD:
                    if (use_long) {
                        /* 32-bit unsigned modulo: D0.L mod D1.L → D0.L
                         * Same algorithm as division, result is remainder in D2 */
                        emit16(cg, 0x4282);  /* CLR.L D2           ; remainder=0 */
                        emit16(cg, 0x761F);  /* MOVEQ #31,D3       ; 32 iterations */
                        emit16(cg, 0xD080);  /* ADD.L D0,D0        ; dividend <<= 1 */
                        emit16(cg, 0xD582);  /* ADDX.L D2,D2       ; remainder <<= 1 + carry */
                        emit16(cg, 0xB481);  /* CMP.L D1,D2        ; remainder >= divisor? */
                        emit16(cg, 0x6504);  /* BCS.S +4           ; skip if remainder < divisor */
                        emit16(cg, 0x9481);  /* SUB.L D1,D2        ; remainder -= divisor */
                        emit16(cg, 0x5280);  /* ADDQ.L #1,D0       ; set quotient bit */
                        emit16(cg, 0x51CB);  /* DBRA D3,offset     */
                        emit16(cg, 0xFFF2);  /* displacement = -14  */
                        emit16(cg, 0x2002);  /* MOVE.L D2,D0       ; result = remainder */
                    } else {
                        emit16(cg, 0x48C0);  /* EXT.L D0 */
                        emit16(cg, 0x81C1);  /* DIVS D1,D0 */
                        emit16(cg, 0x4840);  /* SWAP D0 (remainder in low word) */
                    }
                    break;
                case TOK_AND:
                    emit16(cg, use_long ? 0xC081 : 0xC041);
                    break;
                case TOK_OR:
                    emit16(cg, use_long ? 0x8081 : 0x8041);
                    break;
                case TOK_XOR:
                    emit16(cg, use_long ? 0xB380 : 0xB340);  /* EOR.L/W D1,D0 */
                    break;
                case TOK_SHL:
                    emit16(cg, use_long ? 0xE3A0 : 0xE360);  /* ASL.L/W D1,D0 */
                    break;
                case TOK_SHR:
                    emit16(cg, use_long ? 0xE2A8 : 0xE268);  /* LSR.L/W D1,D0 */
                    break;
                case TOK_IN:
                    /* Set membership: D0 IN D1 (simplified — just emit BTST) */
                    emit16(cg, 0x0100);  /* BTST D0,D1 */
                    emit16(cg, 0x56C0);  /* SNE D0 */
                    emit16(cg, 0x4400);  /* NEG.B D0 */
                    emit16(cg, 0x0240); emit16(cg, 0x0001); /* ANDI.W #1,D0 */
                    break;

                /* Comparisons: CMP.W/L and set condition */
                #define EMIT_CMP_SCC(scc) \
                    emit16(cg, use_long ? 0xB081 : 0xB041); \
                    emit16(cg, scc); \
                    emit16(cg, 0x4400); \
                    emit16(cg, 0x0240); emit16(cg, 0x0001)

                case TOK_EQ: EMIT_CMP_SCC(0x57C0); break;  /* SEQ */
                case TOK_NE: EMIT_CMP_SCC(0x56C0); break;  /* SNE */
                case TOK_LT: EMIT_CMP_SCC(0x5DC0); break;  /* SLT */
                case TOK_LE: EMIT_CMP_SCC(0x5FC0); break;  /* SLE */
                case TOK_GT: EMIT_CMP_SCC(0x5EC0); break;  /* SGT */
                case TOK_GE: EMIT_CMP_SCC(0x5CC0); break;  /* SGE */

                #undef EMIT_CMP_SCC

                default:
                    cg_error(cg, node->line, "unsupported operator in expression");
                    break;
            }
            break;
        }

        case AST_FUNC_CALL: {
            /* Check for Pascal intrinsics — inline instead of JSR */
            const char *fn = node->name;

            /* ORD(x), ORD4(x): convert to integer/longint — identity operation */
            if (str_eq_nocase(fn, "ORD") || str_eq_nocase(fn, "ORD4")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                /* D0 already has the value */
                break;
            }

            /* POINTER(x): cast integer to pointer — identity */
            if (str_eq_nocase(fn, "POINTER")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                break;
            }

            /* CHR(x): integer to char — mask to byte */
            if (str_eq_nocase(fn, "CHR")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                emit16(cg, 0x0240);  /* ANDI.W #$FF,D0 */
                emit16(cg, 0x00FF);
                break;
            }

            /* ABS(x): absolute value */
            if (str_eq_nocase(fn, "ABS")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                if (node->num_children > 0 && expr_size(cg, node->children[0]) == 4) {
                    emit16(cg, 0x4A80);  /* TST.L D0 */
                    emit16(cg, 0x6A02);  /* BPL.S +2 (skip NEG) */
                    emit16(cg, 0x4480);  /* NEG.L D0 */
                } else {
                    emit16(cg, 0x4A40);  /* TST.W D0 */
                    emit16(cg, 0x6A02);  /* BPL.S +2 (skip NEG) */
                    emit16(cg, 0x4440);  /* NEG.W D0 */
                }
                break;
            }

            /* ODD(x): true if odd */
            if (str_eq_nocase(fn, "ODD")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                emit16(cg, 0x0240);  /* ANDI.W #1,D0 */
                emit16(cg, 0x0001);
                break;
            }

            /* SIZEOF(type): emit type size as immediate */
            if (str_eq_nocase(fn, "SIZEOF")) {
                int sz = 2; /* default */
                if (node->num_children > 0) {
                    /* Try to resolve the type name */
                    ast_node_t *arg = node->children[0];
                    if (arg->type == AST_IDENT_EXPR) {
                        type_desc_t *t = find_type(cg, arg->name);
                        if (t && t->size > 0) sz = t->size;
                        else {
                            /* Check if it's a variable and get its type size */
                            cg_symbol_t *s = find_symbol_any(cg, arg->name);
                            if (s && s->type && s->type->size > 0) sz = s->type->size;
                        }
                    }
                }
                emit16(cg, 0x303C);  /* MOVE.W #sz,D0 */
                emit16(cg, (uint16_t)sz);
                break;
            }

            /* SUCC(x): x + 1 */
            if (str_eq_nocase(fn, "SUCC")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                if (node->num_children > 0 && expr_size(cg, node->children[0]) == 4) {
                    emit16(cg, 0x5280);  /* ADDQ.L #1,D0 */
                } else {
                    emit16(cg, 0x5240);  /* ADDQ.W #1,D0 */
                }
                break;
            }

            /* PRED(x): x - 1 */
            if (str_eq_nocase(fn, "PRED")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                if (node->num_children > 0 && expr_size(cg, node->children[0]) == 4) {
                    emit16(cg, 0x5380);  /* SUBQ.L #1,D0 */
                } else {
                    emit16(cg, 0x5340);  /* SUBQ.W #1,D0 */
                }
                break;
            }

            /* LENGTH(s): first byte of string = length */
            if (str_eq_nocase(fn, "LENGTH")) {
                if (node->num_children > 0) gen_ptr_expression(cg, node->children[0]);
                emit16(cg, 0x2040);  /* MOVEA.L D0,A0 */
                emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
                emit16(cg, 0x1010);  /* MOVE.B (A0),D0 */
                break;
            }

            /* WRITE/WRITELN: no-op (no console output in emulator) */
            if (str_eq_nocase(fn, "WRITE") || str_eq_nocase(fn, "WRITELN") ||
                str_eq_nocase(fn, "READ") || str_eq_nocase(fn, "READLN")) {
                /* Evaluate all args (for side effects) but discard */
                for (int i = 0; i < node->num_children; i++)
                    gen_expression(cg, node->children[i]);
                emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
                break;
            }

            /* EXIT: return from current procedure */
            if (str_eq_nocase(fn, "EXIT")) {
                /* If EXIT has an argument (procedure name), ignore it */
                emit16(cg, 0x4E5E);  /* UNLK A6 */
                emit16(cg, 0x4E75);  /* RTS */
                break;
            }

            /* HALT: infinite loop */
            if (str_eq_nocase(fn, "HALT")) {
                emit16(cg, 0x4E71);  /* NOP */
                emit16(cg, 0x60FC);  /* BRA.S self */
                break;
            }

            /* ROUND(x), TRUNC(x): identity for integer args */
            if (str_eq_nocase(fn, "ROUND") || str_eq_nocase(fn, "TRUNC")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                break;
            }

            /* SQR(x): x * x */
            if (str_eq_nocase(fn, "SQR")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                emit16(cg, 0xC1C0);  /* MULS D0,D0 */
                break;
            }

            /* MOVELEFT/MOVERIGHT/FILLCHAR: memory operations */
            if (str_eq_nocase(fn, "MOVELEFT") || str_eq_nocase(fn, "MOVERIGHT") ||
                str_eq_nocase(fn, "FILLCHAR") || str_eq_nocase(fn, "SCANEQ") ||
                str_eq_nocase(fn, "SCANNE")) {
                /* These need proper implementation but for now evaluate args and no-op */
                for (int i = 0; i < node->num_children; i++)
                    gen_expression(cg, node->children[i]);
                emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
                break;
            }

            /* COPY(s, start, len): substring — simplified to return original string */
            if (str_eq_nocase(fn, "COPY")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                break;
            }

            /* CONCAT: string concatenation — simplified to return first arg */
            if (str_eq_nocase(fn, "CONCAT")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                break;
            }

            /* POS: find substring — return 0 (not found) */
            if (str_eq_nocase(fn, "POS")) {
                for (int i = 0; i < node->num_children; i++)
                    gen_expression(cg, node->children[i]);
                emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
                break;
            }

            /* DELETE/INSERT: string operations — no-op */
            if (str_eq_nocase(fn, "DELETE") || str_eq_nocase(fn, "INSERT")) {
                for (int i = 0; i < node->num_children; i++)
                    gen_expression(cg, node->children[i]);
                break;
            }

            /* LogCall: debug trace — always no-op */
            if (str_eq_nocase(fn, "LogCall") || str_eq_nocase(fn, "LOGCALL")) {
                break;
            }

            /* WAnd/WOr/WXor: word-level bitwise ops (Lisa Pascal extensions) */
            if (str_eq_nocase(fn, "WAnd")) {
                if (node->num_children >= 2) {
                    gen_expression(cg, node->children[0]);
                    emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) save first arg */
                    gen_expression(cg, node->children[1]);
                    emit16(cg, 0x301F);  /* MOVE.W (SP)+,D0 restore → but need in D1 */
                    /* Actually: eval arg0→D0, push, eval arg1→D0, pop→D1, AND */
                    /* Let me redo: */
                }
                /* Simpler: eval both, AND them */
                if (node->num_children >= 2) {
                    gen_expression(cg, node->children[1]);
                    emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */
                    gen_expression(cg, node->children[0]);
                    emit16(cg, 0x3200);  /* MOVE.W D0,D1 */
                    emit16(cg, 0x301F);  /* MOVE.W (SP)+,D0 */
                    emit16(cg, 0xC041);  /* AND.W D1,D0 */
                } else if (node->num_children == 1) {
                    gen_expression(cg, node->children[0]);
                }
                break;
            }
            if (str_eq_nocase(fn, "WOr")) {
                if (node->num_children >= 2) {
                    gen_expression(cg, node->children[1]);
                    emit16(cg, 0x3F00);
                    gen_expression(cg, node->children[0]);
                    emit16(cg, 0x3200);
                    emit16(cg, 0x301F);
                    emit16(cg, 0x8041);  /* OR.W D1,D0 */
                }
                break;
            }
            if (str_eq_nocase(fn, "WXor")) {
                if (node->num_children >= 2) {
                    gen_expression(cg, node->children[1]);
                    emit16(cg, 0x3F00);
                    gen_expression(cg, node->children[0]);
                    emit16(cg, 0x3200);
                    emit16(cg, 0x301F);
                    emit16(cg, 0xB141);  /* EOR.W D1,D0 */
                }
                break;
            }
            if (str_eq_nocase(fn, "WNot")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                emit16(cg, 0x4640);  /* NOT.W D0 */
                break;
            }

            /* IORESULT: return 0 (no error) */
            if (str_eq_nocase(fn, "IORESULT")) {
                emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
                break;
            }

            /* EOF/EOLN: standard Pascal file functions — return FALSE */
            if (str_eq_nocase(fn, "EOF") || str_eq_nocase(fn, "EOLN")) {
                if (node->num_children > 0) gen_expression(cg, node->children[0]);
                emit16(cg, 0x7000);  /* MOVEQ #0,D0 (FALSE) */
                break;
            }

            /* BLOCKREAD/BLOCKWRITE/UNITREAD/UNITWRITE: I/O — no-op, return 0 */
            if (str_eq_nocase(fn, "BLOCKREAD") || str_eq_nocase(fn, "BLOCKWRITE") ||
                str_eq_nocase(fn, "UNITREAD") || str_eq_nocase(fn, "UNITWRITE") ||
                str_eq_nocase(fn, "UNITSTATUS") || str_eq_nocase(fn, "UNITCLEAR") ||
                str_eq_nocase(fn, "UNITBUSY")) {
                for (int i = 0; i < node->num_children; i++)
                    gen_expression(cg, node->children[i]);
                emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
                break;
            }

            /* InClass(obj, TFoo): Clascal class membership test.
             * Original compiler emits JSR %_InObCP or %_InObCN.
             * These runtime functions use % prefix which our lexer can't parse yet.
             * TODO: Add % identifier support to lexer, then this resolves to %_InObCN.
             * For now, falls through to generic call → linker stub. */

            /* Type cast: TYPE_NAME(expr) — identity operation in Lisa Pascal.
             * Also handle common built-in type names that may not be in our type table. */
            /* Type cast: TYPE_NAME(expr) — identity operation in Lisa Pascal.
             * Check type table first, then explicit built-in names. */
            if (find_type(cg, fn) != NULL ||
                str_eq_nocase(fn, "LONGINT") || str_eq_nocase(fn, "INTEGER") ||
                str_eq_nocase(fn, "BOOLEAN") || str_eq_nocase(fn, "CHAR") ||
                str_eq_nocase(fn, "BYTE") || str_eq_nocase(fn, "REAL") ||
                str_eq_nocase(fn, "Ptr") || str_eq_nocase(fn, "Handle") ||
                str_eq_nocase(fn, "WordPtr") || str_eq_nocase(fn, "WindowPtr") ||
                str_eq_nocase(fn, "PicHandle") || str_eq_nocase(fn, "FuncHandle") ||
                str_eq_nocase(fn, "TpInteger") || str_eq_nocase(fn, "TpLONGINT") ||
                str_eq_nocase(fn, "QDPtr") || str_eq_nocase(fn, "GrafPtr") ||
                str_eq_nocase(fn, "ThByte") || str_eq_nocase(fn, "TXLRect") ||
                str_eq_nocase(fn, "TOffsets") || str_eq_nocase(fn, "TPMouseEvent") ||
                str_eq_nocase(fn, "TGraphView") ||
                str_eq_nocase(fn, "TUTCollection") || str_eq_nocase(fn, "TUTArray") ||
                str_eq_nocase(fn, "TUTString") || str_eq_nocase(fn, "TUnivText") ||
                str_eq_nocase(fn, "TReadUnivText") || str_eq_nocase(fn, "TWriteUnivText")) {
                /* Type cast = evaluate the argument, result stays in D0 */
                if (node->num_children > 0)
                    gen_expression(cg, node->children[0]);
                break;
            }

            /* Check if calling a procedure/function parameter (indirect call) */
            {
                cg_symbol_t *psym = find_local(cg, fn);
                if (psym && psym->is_param) {
                    /* Push arguments */
                    for (int i = node->num_children - 1; i >= 0; i--) {
                        gen_expression(cg, node->children[i]);
                        emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
                    }
                    /* Load address from stack frame, call indirect */
                    emit16(cg, 0x206E);  /* MOVEA.L offset(A6),A0 */
                    emit16(cg, (uint16_t)(int16_t)psym->offset);
                    emit16(cg, 0x4E90);  /* JSR (A0) */
                    /* Caller cleans up */
                    int ab = node->num_children * 4;
                    if (ab > 0 && ab <= 8) emit16(cg, 0x508F | ((ab & 7) << 9));
                    else if (ab > 8) { emit16(cg, 0xDEFC); emit16(cg, (uint16_t)ab); }
                    break;
                }
            }

            /* Generic function call — not an intrinsic.
             * Pascal calling convention: parameters pushed left-to-right
             * (first param pushed first = deepest on stack).
             * Callee-clean (external/assembly) routines pop params themselves.
             * Caller-clean (Pascal) routines need caller to adjust SP after. */
            cg_proc_sig_t *sig = find_proc_sig(cg, fn);
            cg_symbol_t *callee_sym = find_global(cg, fn);
            if (!callee_sym) callee_sym = find_imported(cg, fn);
            bool is_callee_clean = (callee_sym && callee_sym->is_external) ||
                                   (sig && sig->is_external);
            {

            /* Push order: left-to-right for callee-clean (assembly),
             * right-to-left for caller-clean (Pascal codegen).
             * Use correct push size (MOVE.W vs MOVE.L) based on param type. */
            if (is_callee_clean) {
                for (int i = 0; i < node->num_children; i++) {
                    bool is_var_arg = (sig && i < sig->num_params && sig->param_is_var[i]);
                    int psize = (sig && i < sig->num_params) ? sig->param_size[i] : 4;
                    if (is_var_arg) {
                        gen_lvalue_addr(cg, node->children[i]);
                        emit16(cg, 0x2F08);  /* MOVE.L A0,-(SP) */
                    } else if (psize <= 2) {
                        gen_expression(cg, node->children[i]);
                        emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */
                    } else {
                        gen_expression(cg, node->children[i]);
                        emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
                    }
                }
            } else {
                for (int i = node->num_children - 1; i >= 0; i--) {
                    bool is_var_arg = (sig && i < sig->num_params && sig->param_is_var[i]);
                    int psize = (sig && i < sig->num_params) ? sig->param_size[i] : 4;
                    if (is_var_arg) {
                        gen_lvalue_addr(cg, node->children[i]);
                        emit16(cg, 0x2F08);  /* MOVE.L A0,-(SP) */
                    } else if (psize <= 2) {
                        gen_expression(cg, node->children[i]);
                        emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */
                    } else {
                        gen_expression(cg, node->children[i]);
                        emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
                    }
                }
            }
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
            /* Clean up args — but NOT for EXTERNAL (assembly) routines,
             * which are callee-clean (they pop their own parameters). */
            if (!is_callee_clean) {
                int arg_bytes = 0;
                for (int i = 0; i < node->num_children; i++) {
                    bool is_var_arg = (sig && i < sig->num_params && sig->param_is_var[i]);
                    int psize = (sig && i < sig->num_params) ? sig->param_size[i] : 4;
                    arg_bytes += is_var_arg ? 4 : (psize <= 2 ? 2 : 4);
                }
                if (arg_bytes > 0 && arg_bytes <= 8) {
                    emit16(cg, 0x508F | ((arg_bytes & 7) << 9));  /* ADDQ.L #n,SP */
                } else if (arg_bytes > 8) {
                    emit16(cg, 0xDEFC);  /* ADDA.W #imm,SP */
                    emit16(cg, (uint16_t)arg_bytes);
                }
            }
            /* Result is in D0 */
            break;
        }

        case AST_ARRAY_ACCESS:
            gen_lvalue_addr(cg, node);
            emit_read_a0_to_d0(cg, expr_size(cg, node));
            break;

        case AST_FIELD_ACCESS:
            gen_lvalue_addr(cg, node);
            emit_read_a0_to_d0(cg, expr_size(cg, node));
            break;

        case AST_DEREF:
            gen_ptr_expression(cg, node->children[0]);
            emit16(cg, 0x2040);  /* MOVEA.L D0,A0 */
            emit_read_a0_to_d0(cg, expr_size(cg, node));
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
                align_code(cg);  /* Ensure even PC after every statement */
            }
            break;

        case AST_ASSIGN: {
            /* Evaluate RHS into D0 */
            int rhs_sz = expr_size(cg, node->children[1]);
            gen_expression(cg, node->children[1]);
            /* Post-hoc: if the generated code ends with a 32-bit producer
             * (MOVE.L A0,D0, MOVE.L #imm,D0, MOVE.L disp(An),D0, etc.),
             * ensure the store uses 4 bytes even if expr_size missed it. */
            if (rhs_sz < 4 && cg->code_size >= 2) {
                uint16_t last_op = (cg->code[cg->code_size - 2] << 8) | cg->code[cg->code_size - 1];
                /* Check for common MOVE.L ?,D0 patterns (opword 0x2xxx) */
                if ((last_op & 0xF1C0) == 0x2000 ||   /* MOVE.L <ea>,D0 */
                    last_op == 0x2008 ||                /* MOVE.L A0,D0 */
                    last_op == 0x201F)                  /* MOVE.L (SP)+,D0 */
                    rhs_sz = 4;
                /* Also check 4 bytes back for MOVE.L with displacement */
                if (rhs_sz < 4 && cg->code_size >= 4) {
                    uint16_t prev_op = (cg->code[cg->code_size - 4] << 8) | cg->code[cg->code_size - 3];
                    if (prev_op == 0x202E || /* MOVE.L disp(A6),D0 */
                        prev_op == 0x202D || /* MOVE.L disp(A5),D0 */
                        prev_op == 0x2028 || /* MOVE.L disp(A0),D0 */
                        prev_op == 0x2010 || /* MOVE.L (A0),D0 */
                        prev_op == 0x203C)   /* MOVE.L #imm,D0 */
                        rhs_sz = 4;
                }
            }
            /* Store to LHS — use the larger of target type size and RHS
             * expression size to avoid truncating pointer/longint values
             * when the target type descriptor has incorrect size info. */
            ast_node_t *lhs = node->children[0];
            cg_symbol_t *sym = NULL;
            /* Check if LHS is a WITH field before normal lookup */
            if (lhs->type == AST_IDENT_EXPR && cg->with_depth > 0) {
                type_desc_t *wrt = NULL;
                int widx = -1;
                int fld = with_lookup_field(cg, lhs->name, &wrt, &widx);
                if (fld >= 0 && wrt) {
                    /* WITH field assignment: save RHS, load base, add offset, store */
                    int fsz = type_load_size(wrt->fields[fld].type);
                    if (fsz > 4) fsz = 4;
                    if (rhs_sz > fsz) fsz = rhs_sz; /* don't truncate pointer RHS */
                    if (fsz == 4) {
                        emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) save RHS */
                    } else {
                        emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) save RHS */
                    }
                    gen_with_base(cg, widx);
                    int foff = wrt->fields[fld].offset;
                    if (foff != 0) {
                        emit16(cg, 0xD0FC);  /* ADDA.W #offset,A0 */
                        emit16(cg, (uint16_t)(int16_t)foff);
                    }
                    if (fsz == 4) {
                        emit16(cg, 0x201F);  /* MOVE.L (SP)+,D0 */
                    } else {
                        emit16(cg, 0x301F);  /* MOVE.W (SP)+,D0 */
                    }
                    emit_write_d0_to_a0(cg, fsz);
                    break;  /* Done with this assignment */
                }
            }
            if (lhs->type == AST_IDENT_EXPR)
                sym = find_symbol_any(cg, lhs->name);
            if (sym) {
                if (sym->is_param && sym->is_var_param) {
                    int sz = type_load_size(sym->type);
                    if (sz > 4) sz = 4;
                    if (rhs_sz > sz) sz = rhs_sz; /* don't truncate pointer RHS */
                    int depth = find_local_depth(cg, lhs->name);
                    if (depth > 0) {
                        emit16(cg, 0x2200);  /* MOVE.L D0,D1 — save value */
                        emit_frame_access(cg, depth);  /* parent FP → A0 */
                        emit16(cg, 0x2068);  /* MOVEA.L offset(A0),A0 */
                        emit16(cg, (uint16_t)(int16_t)sym->offset);
                        emit_write_d1_to_a0(cg, sz);
                    } else {
                        emit16(cg, 0x206E);  /* MOVEA.L offset(A6),A0 */
                        emit16(cg, (uint16_t)(int16_t)sym->offset);
                        emit_write_d0_to_a0(cg, sz);
                    }
                } else if (sym->is_param || !sym->is_global) {
                    int depth = find_local_depth(cg, lhs->name);
                    int sz = type_load_size(sym->type);
                    /* Clamp to a valid MOVE size (1, 2, or 4). Large types
                     * (strings, records) would need block copy; for now use 4
                     * which is the widest single MOVE can transfer. */
                    if (sz > 4) sz = 4;
                    if (rhs_sz > sz) sz = rhs_sz; /* don't truncate pointer RHS */
                    if (depth > 0) {
                        /* Outer scope: save D0, get frame, write via A0 */
                        emit16(cg, 0x2200);  /* MOVE.L D0,D1 — save value */
                        emit_frame_access(cg, depth);
                        if (sz == 4)      emit16(cg, 0x2141);  /* MOVE.L D1,offset(A0) */
                        else if (sz == 1) emit16(cg, 0x1141);  /* MOVE.B D1,offset(A0) */
                        else              emit16(cg, 0x3141);  /* MOVE.W D1,offset(A0) */
                    } else {
                        if (sz == 4)      emit16(cg, 0x2D40);  /* MOVE.L D0,offset(A6) */
                        else if (sz == 1) emit16(cg, 0x1D40);  /* MOVE.B D0,offset(A6) */
                        else              emit16(cg, 0x3D40);  /* MOVE.W D0,offset(A6) */
                    }
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                } else {
                    int sz = type_load_size(sym->type);
                    if (sz > 4) sz = 4;
                    if (rhs_sz > sz) sz = rhs_sz; /* don't truncate pointer RHS */
                    if (sz == 4) emit16(cg, 0x2B40);
                    else if (sz == 1) emit16(cg, 0x1B40);
                    else emit16(cg, 0x3B40);  /* MOVE.W D0,offset(A5) */
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                }
            } else if (lhs->type == AST_ARRAY_ACCESS || lhs->type == AST_FIELD_ACCESS || lhs->type == AST_DEREF) {
                /* Complex LHS: size-aware save/restore/store */
                int sz = expr_size(cg, lhs);
                if (sz > 4) sz = 4;
                if (rhs_sz > sz) sz = rhs_sz; /* don't truncate pointer RHS */
                if (sz == 4) {
                    emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
                    gen_lvalue_addr(cg, lhs);
                    emit16(cg, 0x201F);  /* MOVE.L (SP)+,D0 */
                } else {
                    emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */
                    gen_lvalue_addr(cg, lhs);
                    emit16(cg, 0x301F);  /* MOVE.W (SP)+,D0 */
                }
                emit_write_d0_to_a0(cg, sz);
            }
            break;
        }

        case AST_CALL: {
            /* Pascal runtime builtins invoked as statements — no external
             * JSR, just evaluate args for side effects. Without this check
             * every `writeln(x)` call became an unresolved relocation to
             * the (non-existent) symbol "WRITELN". */
            if (is_pascal_runtime_stub_proc(node->name)) {
                /* Emit nothing for arg evaluation — these procs are no-ops
                 * and args are almost always pure literals or simple reads
                 * with no real side effects. Emitting arg-eval bytes leaves
                 * dead code in the output that corrupts execution if PC
                 * ever lands there (e.g. via a mangled exception return). */
                if (str_eq_nocase(node->name, "HALT")) {
                    emit16(cg, 0x4E71);  /* NOP */
                    emit16(cg, 0x60FC);  /* BRA.S self */
                }
                break;
            }
            /* Check if calling a procedure parameter (indirect call).
             * A procedure parameter is a local with is_param=true and
             * no type child in the AST (type defaults to "integer"). */
            {
                cg_symbol_t *sym = find_local(cg, node->name);
                if (sym && sym->is_param) {
                    /* Push any arguments */
                    for (int a = node->num_children - 1; a >= 0; a--) {
                        gen_expression(cg, node->children[a]);
                        emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
                    }
                    /* Load procedure address from stack frame and call indirect */
                    emit16(cg, 0x206E);  /* MOVEA.L offset(A6),A0 */
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                    emit16(cg, 0x4E90);  /* JSR (A0) */
                    /* Caller cleans up args */
                    int arg_bytes = node->num_children * 4;
                    if (arg_bytes > 0 && arg_bytes <= 8)
                        emit16(cg, 0x508F | ((arg_bytes & 7) << 9));
                    else if (arg_bytes > 8) {
                        emit16(cg, 0xDEFC);
                        emit16(cg, (uint16_t)arg_bytes);
                    }
                    break;
                }
            }
            /* Direct procedure call */
            /* Look up procedure signature for param sizes */
            {
                cg_proc_sig_t *call_sig = find_proc_sig(cg, node->name);
                cg_symbol_t *call_sym = find_global(cg, node->name);
                if (!call_sym) call_sym = find_imported(cg, node->name);
                bool call_callee_clean = (call_sym && call_sym->is_external) ||
                                         (call_sig && call_sig->is_external);

                /* Push arguments with correct sizes */
                if (call_callee_clean) {
                    /* Left-to-right for callee-clean (assembly) */
                    for (int a = 0; a < node->num_children; a++) {
                        bool is_var_arg = (call_sig && a < call_sig->num_params && call_sig->param_is_var[a]);
                        int psize = (call_sig && a < call_sig->num_params) ? call_sig->param_size[a] : 4;
                        if (is_var_arg) {
                            gen_lvalue_addr(cg, node->children[a]);
                            emit16(cg, 0x2F08);  /* MOVE.L A0,-(SP) */
                        } else if (psize <= 2) {
                            gen_expression(cg, node->children[a]);
                            emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */
                        } else {
                            gen_expression(cg, node->children[a]);
                            emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
                        }
                    }
                } else {
                    /* Right-to-left for caller-clean (Pascal) */
                    for (int a = node->num_children - 1; a >= 0; a--) {
                        bool is_var_arg = (call_sig && a < call_sig->num_params && call_sig->param_is_var[a]);
                        int psize = (call_sig && a < call_sig->num_params) ? call_sig->param_size[a] : 4;
                        if (is_var_arg) {
                            gen_lvalue_addr(cg, node->children[a]);
                            emit16(cg, 0x2F08);  /* MOVE.L A0,-(SP) */
                        } else if (psize <= 2) {
                            gen_expression(cg, node->children[a]);
                            emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */
                        } else {
                            gen_expression(cg, node->children[a]);
                            emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
                        }
                    }
                }
                emit16(cg, 0x4EB9);
                emit32(cg, 0);
                if (cg->num_relocs < CODEGEN_MAX_RELOCS) {
                    cg_reloc_t *r = &cg->relocs[cg->num_relocs++];
                    r->offset = cg->code_size - 4;
                    strncpy(r->symbol, node->name, sizeof(r->symbol) - 1);
                    r->size = 4;
                }
                /* Clean up args -- only for caller-clean routines */
                if (!call_callee_clean) {
                    int arg_bytes = 0;
                    for (int a = 0; a < node->num_children; a++) {
                        bool is_var_arg = (call_sig && a < call_sig->num_params && call_sig->param_is_var[a]);
                        int psize = (call_sig && a < call_sig->num_params) ? call_sig->param_size[a] : 4;
                        arg_bytes += is_var_arg ? 4 : (psize <= 2 ? 2 : 4);
                    }
                    if (arg_bytes > 0 && arg_bytes <= 8)
                        emit16(cg, 0x508F | ((arg_bytes & 7) << 9));
                    else if (arg_bytes > 8) {
                        emit16(cg, 0xDEFC);
                        emit16(cg, (uint16_t)arg_bytes);
                    }
                }
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
                align_code(cg);
                patch16(cg, else_pos, (uint16_t)(cg->code_size - else_pos));
                gen_statement(cg, node->children[2]);
                align_code(cg);
                patch16(cg, end_pos, (uint16_t)(cg->code_size - end_pos));
            } else {
                align_code(cg);
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
            align_code(cg);
            emit16(cg, 0x6000);  /* BRA.W loop_start */
            emit16(cg, (uint16_t)((int32_t)loop_start - (int32_t)cg->code_size));

            align_code(cg);
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

            /* Initialize loop variable (size-aware store) */
            int for_rhs_sz = expr_size(cg, node->children[0]);
            gen_expression(cg, node->children[0]);
            if (var) {
                int sz = type_load_size(var->type);
                if (for_rhs_sz > sz) sz = for_rhs_sz;
                if (sz == 4)
                    emit16(cg, 0x2D40);  /* MOVE.L D0,offset(A6) */
                else
                    emit16(cg, 0x3D40);  /* MOVE.W D0,offset(A6) */
                emit16(cg, (uint16_t)(int16_t)(var ? var->offset : 0));
            }

            /* Evaluate end value, save in D3 (size-aware) */
            int vsz = type_load_size(var ? var->type : NULL);
            gen_expression(cg, node->children[1]);
            if (vsz == 4)
                emit16(cg, 0x2600);  /* MOVE.L D0,D3 */
            else
                emit16(cg, 0x3600);  /* MOVE.W D0,D3 */

            /* Loop start */
            uint32_t loop_start = cg->code_size;

            /* Compare loop var to end (size-aware) */
            if (var) {
                if (vsz == 4)
                    emit16(cg, 0x202E);  /* MOVE.L offset(A6),D0 */
                else
                    emit16(cg, 0x302E);  /* MOVE.W offset(A6),D0 */
                emit16(cg, (uint16_t)(int16_t)var->offset);
            }
            if (vsz == 4)
                emit16(cg, 0xB083);  /* CMP.L D3,D0 */
            else
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

            /* Increment/decrement (size-aware) */
            if (var) {
                if (node->int_val > 0) {
                    if (vsz == 4)
                        emit16(cg, 0x52AE);  /* ADDQ.L #1,offset(A6) */
                    else
                        emit16(cg, 0x526E);  /* ADDQ.W #1,offset(A6) */
                } else {
                    if (vsz == 4)
                        emit16(cg, 0x53AE);  /* SUBQ.L #1,offset(A6) */
                    else
                        emit16(cg, 0x536E);  /* SUBQ.W #1,offset(A6) */
                }
                emit16(cg, (uint16_t)(int16_t)var->offset);
            }

            /* Branch back */
            align_code(cg);
            emit16(cg, 0x6000);
            emit16(cg, (uint16_t)((int32_t)loop_start - (int32_t)cg->code_size));

            align_code(cg);
            patch16(cg, end_pos, (uint16_t)(cg->code_size - end_pos));
            break;
        }

        case AST_WITH: {
            /* WITH record1, record2, ... DO body
             * Push each record expression onto the WITH context stack
             * so field names are resolved implicitly. */
            int body_idx = node->num_children - 1;
            int num_withs = body_idx;  /* All children except last are records */
            int saved_depth = cg->with_depth;

            for (int wi = 0; wi < num_withs && cg->with_depth < 16; wi++) {
                ast_node_t *rec_expr = node->children[wi];
                type_desc_t *rt = NULL;

                /* Determine the record type from the expression */
                if (rec_expr->type == AST_DEREF && rec_expr->children[0]) {
                    /* WITH ptr^ DO ... — get pointed-to type */
                    ast_node_t *ptr_node = rec_expr->children[0];
                    if (ptr_node->type == AST_IDENT_EXPR) {
                        cg_symbol_t *sym = find_symbol_any(cg, ptr_node->name);
                        if (sym && sym->type && sym->type->kind == TK_POINTER && sym->type->base_type)
                            rt = sym->type->base_type;
                    }
                } else if (rec_expr->type == AST_IDENT_EXPR) {
                    /* WITH var DO ... — get variable's type */
                    cg_symbol_t *sym = find_symbol_any(cg, rec_expr->name);
                    if (sym && sym->type) {
                        rt = sym->type;
                        if (rt->kind == TK_POINTER && rt->base_type)
                            rt = rt->base_type;
                    }
                } else if (rec_expr->type == AST_ARRAY_ACCESS && rec_expr->children[0]) {
                    /* WITH arr[i] DO ... — get element type */
                    if (rec_expr->children[0]->type == AST_IDENT_EXPR) {
                        cg_symbol_t *sym = find_symbol_any(cg, rec_expr->children[0]->name);
                        if (sym && sym->type && sym->type->kind == TK_ARRAY && sym->type->element_type)
                            rt = sym->type->element_type;
                    }
                } else if (rec_expr->type == AST_FIELD_ACCESS) {
                    /* WITH rec.subfield DO ... — resolve nested field type */
                    int es = expr_size(cg, rec_expr);
                    (void)es; /* type resolution is complex; push NULL and hope for the best */
                }

                cg->with_stack[cg->with_depth].record_type = (rt && rt->kind == TK_RECORD) ? rt : NULL;
                cg->with_stack[cg->with_depth].record_expr = rec_expr;
                cg->with_depth++;
            }

            /* Generate body */
            if (body_idx >= 0) {
                gen_statement(cg, node->children[body_idx]);
            }

            /* Pop WITH context */
            cg->with_depth = saved_depth;
            break;
        }

        case AST_CASE: {
            /* Evaluate selector into D0, save in D3 (size-aware) */
            int csz = expr_size(cg, node->children[0]);
            gen_expression(cg, node->children[0]);
            if (csz == 4)
                emit16(cg, 0x2600);  /* MOVE.L D0,D3 */
            else
                emit16(cg, 0x3600);  /* MOVE.W D0,D3 */

            uint32_t end_fixups[128];
            int num_fixups = 0;

            /* Case branches: pairs of (label, statement) starting at child[1] */
            int ci = 1;
            while (ci + 1 < node->num_children) {
                /* Evaluate label into D0 */
                gen_expression(cg, node->children[ci]);
                if (csz == 4)
                    emit16(cg, 0xB083);  /* CMP.L D3,D0 */
                else
                    emit16(cg, 0xB043);  /* CMP.W D3,D0 */
                emit16(cg, 0x6600);  /* BNE.W next_case */
                uint32_t next_pos = cg->code_size;
                emit16(cg, 0);

                /* Case body */
                gen_statement(cg, node->children[ci + 1]);

                /* BRA.W end_case */
                align_code(cg);
                emit16(cg, 0x6000);
                if (num_fixups < 128) end_fixups[num_fixups++] = cg->code_size;
                emit16(cg, 0);

                /* Patch BNE to skip to here */
                align_code(cg);
                patch16(cg, next_pos, (uint16_t)(cg->code_size - next_pos));

                ci += 2;
            }

            /* OTHERWISE: any remaining odd children */
            while (ci < node->num_children) {
                gen_statement(cg, node->children[ci]);
                ci++;
            }

            /* Patch all end_case branches to here */
            align_code(cg);
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
    /* If type wasn't found, default to 4 bytes (longint/pointer).
     * In Lisa Pascal, most non-trivial types are at least 4 bytes: pointers,
     * longints, absptr, records. Defaulting to 2 (integer) causes pointer
     * truncation and stack frame underallocation throughout the OS code.
     * The 4-byte default is safer: worst case wastes 2 bytes per variable;
     * the 2-byte default silently corrupts pointers and addresses. */
    if (!type && node->num_children > 0) {
        type = find_type(cg, "longint");  /* Default unresolved types to 4 bytes */
    }

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
                /* Assign global offset (grow DOWNWARD from A5).
                 * Lisa Pascal convention: A5 points to the top of the global
                 * data area. Globals use negative offsets: A5-2, A5-4, etc.
                 * Uses a process-wide counter because all kernel units
                 * share the same A5-relative global data area. */
                static int global_offset = 0;
                if (sz >= 2 && (global_offset % 2)) global_offset++;
                global_offset += sz;
                s->offset = -global_offset;
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
                    s->is_const = true;
                }
                break;
            }
            case AST_TYPE_DECL: {
                if (child->num_children > 0) {
                    type_desc_t *t = resolve_type(cg, child->children[0]);
                    if (t && child->name[0]) {
                        /* If this type already has a different name, create a new
                         * alias entry instead of overwriting the existing name.
                         * Otherwise types like tenbite→fp_extended→extended
                         * would lose intermediate names. */
                        if (t->name[0] && !str_eq_nocase(t->name, child->name)) {
                            type_desc_t *alias = add_type(cg, child->name, t->kind, t->size);
                            if (alias) {
                                /* Copy all type info from original */
                                char saved_name[64];
                                strncpy(saved_name, child->name, sizeof(saved_name) - 1);
                                saved_name[63] = '\0';
                                *alias = *t;
                                strncpy(alias->name, saved_name, sizeof(alias->name) - 1);
                            }
                        } else {
                            strncpy(t->name, child->name, sizeof(t->name) - 1);
                        }
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
            case AST_PROC_DECL:
            case AST_FUNC_DECL: {
                bool is_ext = str_eq_nocase(child->str_val, "EXTERNAL");
                bool is_func = (child->type == AST_FUNC_DECL);
                /* Register signature for VAR parameter tracking */
                bool has_params = false;
                for (int j = 0; j < child->num_children; j++) {
                    if (child->children[j]->type == AST_PARAM_LIST) {
                        register_proc_sig(cg, child->name,
                                          child->children[j]->children,
                                          child->children[j]->num_children);
                        has_params = true;
                        break;
                    }
                }
                /* Set is_function flag on the registered signature */
                {
                    cg_proc_sig_t *sig = find_proc_sig(cg, child->name);
                    if (sig) sig->is_function = is_func;
                }
                /* If external declaration without params, mark existing signature */
                if (is_ext) {
                    cg_proc_sig_t *existing = find_proc_sig(cg, child->name);
                    if (existing) { existing->is_external = true; existing->is_function = is_func; }
                    /* Also register empty sig if no params and no existing sig */
                    if (!has_params && !existing && cg->proc_sigs &&
                        cg->num_proc_sigs < CODEGEN_MAX_PROC_SIGS) {
                        cg_proc_sig_t *sig = &cg->proc_sigs[cg->num_proc_sigs++];
                        memset(sig, 0, sizeof(*sig));
                        strncpy(sig->name, child->name, 63);
                        sig->is_external = true;
                        sig->is_function = is_func;
                    }
                }
                /* Register as external symbol */
                cg_symbol_t *psym = add_global_sym(cg, child->name, NULL);
                if (psym && is_ext) psym->is_external = true;
                break;
            }
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

    /* Ensure word alignment before entry point */
    align_code(cg);

    /* Record entry point */
    cg_symbol_t *entry = add_global_sym(cg, node->name, NULL);
    if (entry) entry->offset = (int)cg->code_size;

    push_scope(cg);
    if (strcasestr(cg->current_file, "STARTUP"))
        fprintf(stderr, "  SCOPE PUSH: %s depth=%d frame_size=%d\n",
                node->name, cg->scope_depth,
                cg->scope_depth > 0 ? cg->scopes[cg->scope_depth-1].frame_size : -1);

    /* Process parameters */
    int param_offset = 8; /* After saved A6 and return address */
    bool has_param_list = false;
    for (int i = 0; i < node->num_children; i++) {
        if (node->children[i]->type == AST_PARAM_LIST) {
            has_param_list = true;
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
    /* If IMPLEMENTATION body has no param list, reconstruct params from
     * the INTERFACE/FORWARD declaration's stored signature */
    if (!has_param_list) {
        cg_proc_sig_t *sig = find_proc_sig(cg, node->name);
        if (sig && sig->num_params > 0) {
            fprintf(stderr, "  INHERITED PARAMS for %s: %d params from signature\n", node->name, sig->num_params);
            for (int j = 0; j < sig->num_params; j++) {
                fprintf(stderr, "    param[%d]: '%s' size=%d var=%d type=%s\n",
                        j, sig->param_name[j],
                        sig->param_type[j] ? sig->param_type[j]->size : -1,
                        sig->param_is_var[j],
                        sig->param_type[j] ? sig->param_type[j]->name : "NULL");
            }
            for (int j = 0; j < sig->num_params; j++) {
                type_desc_t *ptype = sig->param_type[j];
                if (!ptype) ptype = find_type(cg, "integer");
                cg_symbol_t *s = add_local(cg, sig->param_name[j], ptype, true, sig->param_is_var[j]);
                if (s) {
                    s->offset = param_offset;
                    fprintf(stderr, "      → registered '%s' at A6+%d (size=%d)\n",
                            sig->param_name[j], param_offset, ptype ? ptype->size : 2);
                    param_offset += sig->param_is_var[j] ? 4 : (ptype ? ptype->size : 2);
                    if (param_offset % 2) param_offset++;
                }
            }
        }
    }

    /* Register procedure signature for VAR parameter tracking at call sites */
    for (int i = 0; i < node->num_children; i++) {
        if (node->children[i]->type == AST_PARAM_LIST) {
            ast_node_t *params = node->children[i];
            register_proc_sig(cg, node->name, params->children, params->num_children);
            break;
        }
    }

    /* Process local declarations */
    for (int i = 0; i < node->num_children; i++) {
        ast_node_t *child = node->children[i];
        if (child->type == AST_VAR_DECL) {
            if (strcasestr(cg->current_file, "STARTUP") &&
                strcasestr(node->name, "INITSYS"))
                fprintf(stderr, "  LOCAL VAR: %s in %s\n", child->name, node->name);
            process_var_decl(cg, child, false);
        }
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
    align_code(cg);
    if (entry) entry->offset = (int)cg->code_size;

    /* LINK A6,#-frame_size */
    if (strcasestr(cg->current_file, "STARTUP") && strcasestr(node->name, "INITSYS"))
        fprintf(stderr, "  INITSYS LINK: frame_size=%d at offset %u\n", frame_size, cg->code_size);
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

    /* Ensure word alignment before main body — 68000 requires even PC */
    align_code(cg);
    /* Patch the BRA.W displacement to jump to the main body.
     * BRA.W: target = PC + displacement, where PC = BRA_addr + 2.
     * So displacement = body_offset - (bra_fixup + 2). */
    uint32_t body_offset = cg->code_size;
    int16_t displacement = (int16_t)(body_offset - (bra_fixup + 2));
    if (strcasestr(cg->current_file, "STARTUP")) {
        fprintf(stderr, "STARTUP BRA.W: bra_fixup=%u body_offset=%u displacement=%d (0x%04X) target_odd=%d\n",
                bra_fixup, body_offset, displacement, (uint16_t)displacement, body_offset & 1);
    }
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
    cg->proc_sigs = calloc(CODEGEN_MAX_PROC_SIGS, sizeof(cg_proc_sig_t));
    init_builtin_types(cg);
}

void codegen_free(codegen_t *cg) {
    if (cg->proc_sigs) free(cg->proc_sigs);
    memset(cg, 0, sizeof(codegen_t));
}

/* Register a procedure signature for VAR parameter tracking */
static void register_proc_sig(codegen_t *cg, const char *name, ast_node_t *params[], int num_params) {
    if (!cg->proc_sigs || cg->num_proc_sigs >= CODEGEN_MAX_PROC_SIGS) return;
    /* Log signature registrations for key boot procedures */
    if (strcasecmp(name, "INTSOFF") == 0 || strcasecmp(name, "INTSON") == 0 ||
        strcasecmp(name, "POOL_INIT") == 0 || strcasecmp(name, "INIT_TRAPV") == 0) {
        fprintf(stderr, "  REGISTER SIG: %s (%d params) in %s\n", name, num_params, cg->current_file);
    }
    cg_proc_sig_t *sig = &cg->proc_sigs[cg->num_proc_sigs++];
    strncpy(sig->name, name, 63);
    sig->num_params = num_params < CODEGEN_MAX_PARAMS ? num_params : CODEGEN_MAX_PARAMS;
    for (int i = 0; i < sig->num_params; i++) {
        /* str_val[0] != '\0' means VAR parameter (set by parser) */
        sig->param_is_var[i] = (params[i]->str_val[0] != '\0');
        strncpy(sig->param_name[i], params[i]->name, 63);
        /* Resolve and store the parameter type */
        type_desc_t *ptype = NULL;
        if (params[i]->num_children > 0)
            ptype = resolve_type(cg, params[i]->children[0]);
        sig->param_type[i] = ptype;
        if (sig->param_is_var[i]) {
            sig->param_size[i] = 4;  /* VAR params are always pointers */
        } else {
            if (ptype && ptype->size == 4)
                sig->param_size[i] = 4;  /* longint, pointer, etc. */
            else
                sig->param_size[i] = 2;  /* integer, enum, boolean, etc. */
        }
    }
}

/* Look up a procedure signature by name */
static cg_proc_sig_t *find_proc_sig(codegen_t *cg, const char *name) {
    /* Search local signatures */
    if (cg->proc_sigs) {
        for (int i = 0; i < cg->num_proc_sigs; i++) {
            if (strcasecmp(cg->proc_sigs[i].name, name) == 0)
                return &cg->proc_sigs[i];
        }
    }
    /* Search imported signatures */
    if (cg->imported_proc_sigs) {
        for (int i = 0; i < cg->imported_proc_sigs_count; i++) {
            if (strcasecmp(cg->imported_proc_sigs[i].name, name) == 0)
                return &cg->imported_proc_sigs[i];
        }
    }
    return NULL;
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
