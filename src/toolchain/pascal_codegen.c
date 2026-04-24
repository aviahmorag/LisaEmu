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

/* Lisa Pascal: identifiers are significant to 8 characters only.
 * "ordrefncbptr" and "ordrefncb" both resolve to the same variable
 * because their first 8 characters ("ordrefnc") match.
 * Compare case-insensitively, treating identifiers as equal if their
 * first 8 characters match (shorter strings must match exactly). */
static bool str_eq_nocase(const char *a, const char *b) {
    int n = 0;
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++; b++;
        if (++n >= 8) return true;  /* 8-char significant match */
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
    type_desc_t *local = NULL;
    /* P101: prefer an EXACT case-insensitive full-string match over the
     * 8-char-significant fallback. Apple Pascal's 8-char significance
     * rule is for identifiers at the source-language level, but our
     * implementation of str_eq_nocase treats `LogicalAddress` and
     * `logicaladr` as equal — and when a local type inside one proc
     * (e.g. `logicaladr` inside SOURCE-EXCEPRES's `showregs`) pollutes
     * the global type table, it can mask an unrelated global type of
     * longer name (e.g. `LogicalAddress = LongInt` in HWINT). That
     * collision caused `ord(@PARAMEM_WRITE)`'s param type to resolve
     * as TK_RECORD → ARG_BY_REF true → AlarmAssign's routine arg
     * got pushed as @routine instead of routine-value. Search for the
     * exact full-string match first, then fall back to significant-
     * match for the legitimate 8-char truncation case. */
    for (int i = 0; i < cg->num_types; i++) {
        if (strcasecmp(cg->types[i].name, name) == 0) {
            local = &cg->types[i];
            break;
        }
    }
    if (!local) {
        for (int i = 0; i < cg->num_types; i++) {
            if (str_eq_nocase(cg->types[i].name, name)) {
                local = &cg->types[i];
                break;
            }
        }
    }
    /* P80g: for RECORD types, prefer imported version if the local has
     * corrupt field offsets (all zeros). The local type gets its offsets
     * zeroed by dangling pointer reads after *existing = *t copies.
     * The imported version (from the pre-pass fixup) has correct offsets. */
    if (local && local->kind == TK_RECORD && local->num_fields > 1 &&
        local->fields[1].offset == 0 && cg->imported_types) {
        for (int i = 0; i < cg->imported_types_count; i++) {
            if (str_eq_nocase(cg->imported_types[i].name, name) &&
                cg->imported_types[i].kind == TK_RECORD &&
                cg->imported_types[i].num_fields > 1 &&
                cg->imported_types[i].fields[1].offset > 0) {
                return &cg->imported_types[i];
            }
        }
    }
    if (local) return local;
    /* Also search imported types — exact match first, then 8-char significant. */
    if (cg->imported_types) {
        for (int i = 0; i < cg->imported_types_count; i++) {
            if (strcasecmp(cg->imported_types[i].name, name) == 0)
                return &cg->imported_types[i];
        }
        for (int i = 0; i < cg->imported_types_count; i++) {
            if (str_eq_nocase(cg->imported_types[i].name, name))
                return &cg->imported_types[i];
        }
    }
    return NULL;
}

static type_desc_t *add_type(codegen_t *cg, const char *name, type_kind_t kind, int size) {
    if (cg->num_types >= CODEGEN_MAX_SYMBOLS) {
        static int overflow_warn = 0;
        if (overflow_warn++ < 3)
            fprintf(stderr, "WARNING: type table overflow at %d types (max %d)\n",
                    cg->num_types, CODEGEN_MAX_SYMBOLS);
        return NULL;
    }
    type_desc_t *t = &cg->types[cg->num_types++];
    memset(t, 0, sizeof(type_desc_t));
    t->kind = kind;
    t->size = size;
    strncpy(t->name, name, sizeof(t->name) - 1);
    return t;
}

static void init_builtin_types(codegen_t *cg) {
    add_type(cg, "integer", TK_INTEGER, 2);
    add_type(cg, "int1", TK_BYTE, 1);     /* P79f: int1 is naturally 1 byte.
                                           * Widening to 2 bytes for unpacked record
                                           * fields is done in the record resolver. The
                                           * P79 fix of TK_INTEGER/2 was too broad —
                                           * it doubled array[1..150] of int1 elements,
                                           * causing sc_par_no in sc_table to overflow
                                           * into b_syslocal_ptr (SCTAB2 corruption). */
    add_type(cg, "int2", TK_INTEGER, 2);
    add_type(cg, "int4", TK_LONGINT, 4);
    add_type(cg, "longint", TK_LONGINT, 4);
    add_type(cg, "boolean", TK_BOOLEAN, 2);  /* Lisa Pascal: word-sized on 68000 */
    add_type(cg, "char", TK_CHAR, 1);
    add_type(cg, "real", TK_REAL, 4);
    /* P95: Apple's Pascal source declares `byte = -128..127` (signed
     * subrange) in SOURCE-VMSTUFF, source-LOADER, source-TWIG, etc.
     * If we register `byte` as TK_BYTE (unsigned), byte reads zero-
     * extend and comparisons against signed int constants like
     * `codeblock = -123` fail — $0085 != $FF85. Apple's compiler
     * sign-extended `byte` reads because the declared range includes
     * negative values. Register as a signed subrange so our
     * type_is_signed_byte() catches it and emits EXT.W/EXT.L on load. */
    {
        type_desc_t *bt = add_type(cg, "byte", TK_SUBRANGE, 1);
        if (bt) { bt->range_low = -128; bt->range_high = 127; }
    }
    add_type(cg, "absptr", TK_LONGINT, 4);   /* Lisa OS: absolute pointer */
    add_type(cg, "ptr", TK_POINTER, 4);
    add_type(cg, "text", TK_FILE, 0);
}

static cg_symbol_t *add_global_sym(codegen_t *cg, const char *name, type_desc_t *type);

static int type_size(type_desc_t *t) {
    if (!t) return 2; /* default word */
    return t->size;
}

/* Forward declarations for symbol lookup (used in resolve_type for CONST) */
static cg_symbol_t *find_global(codegen_t *cg, const char *name);
static cg_symbol_t *find_imported(codegen_t *cg, const char *name);
static cg_symbol_t *find_symbol_any(codegen_t *cg, const char *name);
static void register_proc_sig(codegen_t *cg, const char *name, ast_node_t *params[], int num_params, int nest_depth);
static cg_proc_sig_t *find_proc_sig(codegen_t *cg, const char *name);

/* Non-primitive value param (record/string/array): P16 passes by reference
 * as a 4-byte pointer. Caller must push @arg via LEA, not the arg value.
 * P102: if the cached param_type pointer was recorded in a unit whose
 * local type table has since been overwritten, the recorded kind can
 * lie. Re-resolve by recorded type name against the current type tables
 * when possible. */
static type_desc_t *find_type(codegen_t *cg, const char *name);
static inline bool ARG_BY_REF(codegen_t *cg, cg_proc_sig_t *sig, int a) {
    if (!sig || a >= sig->num_params) return false;
    if (sig->param_is_var[a]) return false;
    /* P102: prefer the registration-time snapshot of the kind. Across
     * compilation units the cached pointer can be mutated by unrelated
     * code; the snapshot captured at register_proc_sig is authoritative. */
    int kind = sig->param_type_kind[a];
    if (kind == 0) {
        /* Fallback: re-resolve by name, then by cached pointer. */
        type_desc_t *t = NULL;
        if (cg && sig->param_type_name[a][0])
            t = find_type(cg, sig->param_type_name[a]);
        if (!t) t = sig->param_type[a];
        if (!t) return false;
        kind = (int)t->kind;
    }
    return kind == TK_RECORD || kind == TK_STRING || kind == TK_ARRAY;
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
            /* Store base type name for forward-reference resolution */
            strncpy(t->base_name, node->name, sizeof(t->base_name) - 1);
            t->base_name[63] = '\0';
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
                /* P72: same fix as P71 for subrange bounds — handle
                 * unary-minus (e.g. `-128..127`) and CONST identifier
                 * references (e.g. `min_ldsn..max_ldsn`). Without this,
                 * negative bounds collapse to 0 and arrays/subranges
                 * are sized smaller than Apple's source intends. */
                for (int bi = 0; bi < 2; bi++) {
                    ast_node_t *bound = node->children[bi];
                    int val = 0;
                    if (bound->type == AST_UNARY_OP &&
                        bound->num_children > 0 &&
                        bound->children[0]->type == AST_INT_LITERAL) {
                        int inner = (int)bound->children[0]->int_val;
                        val = (bound->op == TOK_MINUS) ? -inner : inner;
                    } else if (bound->type == AST_IDENT_EXPR && bound->name[0]) {
                        cg_symbol_t *cs = find_global(cg, bound->name);
                        if (!cs) cs = find_imported(cg, bound->name);
                        if (cs) val = cs->offset;
                    } else {
                        val = (int)bound->int_val;
                    }
                    if (bi == 0) t->range_low = val;
                    else t->range_high = val;
                }
                int range = t->range_high - t->range_low;
                /* P79f: byte-sized subranges (range<=255) get natural size=1.
                 * In unpacked record fields, the record resolver widens to 2.
                 * In arrays, elements stay at 1 byte (matching Apple's layout:
                 * sc_par_no: array[1..150] of int1 = 150 bytes, not 300).
                 * Previously only packed context gave size=1; unpacked always
                 * gave 2, making array elements double-sized. */
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
                /* Resolve array bounds — may be CONST identifiers. Use
                 * find_symbol_any so that local CONSTs (declared inside
                 * the enclosing proc's CONST section) are consulted in
                 * addition to globals/imports. Without this, local
                 * CONST bounds like `maxdtable = 35` inside
                 * INIT_JTDRIVER fall back to 0, which collapses an
                 * `array[0..maxdtable] of record` to a 1-element array
                 * and makes sizeof(enclosing_record) = first-field-size
                 * instead of count*record-size. Downstream effect: the
                 * array-element stride in lvalue code is wrong AND the
                 * GETSPACE sizeof arg is wrong. */
                int lo = (int)node->children[0]->int_val;
                int hi = (int)node->children[1]->int_val;
                if (lo == 0 && node->children[0]->name[0]) {
                    cg_symbol_t *cs = find_symbol_any(cg, node->children[0]->name);
                    if (cs && cs->is_const) lo = cs->offset;
                }
                if (hi == 0 && node->children[1]->name[0]) {
                    cg_symbol_t *cs = find_symbol_any(cg, node->children[1]->name);
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
            if (cg->in_packed) t->is_packed = true;
            int offset = 0;
            int variant_start = -1;   /* offset where variants begin; -1 = not yet */
            int variant_max_end = 0;  /* max end offset across all variant arms */
            int current_arm = 0;      /* 0 = fixed part, 1..N = variant arm index */
            /* P87d: bit-level cursor for packed records. Tracks total bits
             * consumed from the start of the record (or current variant arm).
             * bit_cursor % 8 gives the free-bit position within the byte at
             * bit_cursor / 8. For whole-byte fields we byte-align this up
             * before advancing, which keeps the unpacked semantics intact.
             * For bit-packable fields (Tnibble 4 bits, boolean 1 bit) in
             * packed context we subdivide the byte. First-declared field
             * gets the HIGH bits, matching Apple's pmem comment layout. */
            int bit_cursor = 0;
            /* P87d: only enable boolean/Tnibble bit-packing when the record
             * actually contains a Tnibble-sized field. That narrows the
             * change to records (like pmem) whose layout is unreachable
             * without nibble splitting, while leaving boolean-only packed
             * records (e.g. segstates: nine booleans) at their existing
             * 1-byte-per-field layout — the asm pin tables and embedded-
             * record offset chains still expect those sizes. */
            bool record_has_nibble = false;
            /* P90: packed record with a variant part (case..of) must overlap
             * its arms cleanly. If an arm widens (e.g. boolean to 2 bytes),
             * its layout can extend past another arm's `longint` storage and
             * reading a field through one arm picks up stale bytes after the
             * other arm's assignment. c_ne in PMEM is the smoking gun:
             *   true:  (PutLastOp:boolean; lastsize:pmbyte; offset:integer)
             *   false: (l: longint)
             * Expected 4/4 bytes; default boolean=2 makes true=5, leaving
             * byte 4 unset after `l := NextEntry`. `offset` at byte 3 then
             * reads stale data and GetNxtConfig always takes the
             * errnum:=e_badnext path. Fix: in packed variant records, pack
             * booleans to 1 byte so arms align. */
            bool record_has_variant = false;
            if (cg->in_packed) {
                for (int pi = 0; pi < node->num_children; pi++) {
                    ast_node_t *pf = node->children[pi];
                    if (pf->type != AST_FIELD || pf->num_children == 0) continue;
                    type_desc_t *pft = resolve_type(cg, pf->children[0]);
                    if (pft && pft->kind == TK_SUBRANGE &&
                        pft->range_low == 0 && pft->range_high == 15) {
                        record_has_nibble = true;
                    }
                }
                for (int pi = 0; pi < node->num_children; pi++) {
                    ast_node_t *pf = node->children[pi];
                    if (pf->type == AST_FIELD && pf->num_children == 0 &&
                        strcmp(pf->name, "__variant_begin__") == 0) {
                        record_has_variant = true;
                        break;
                    }
                }
            }
            for (int i = 0; i < node->num_children; i++) {
                ast_node_t *field = node->children[i];
                if (field->type != AST_FIELD) continue;
                /* Variant-region sentinels inserted by the parser.
                 * NOTE: str_eq_nocase does 8-char significant-prefix matching,
                 * so "__variant_begin__", "__variant_arm__", "__variant_end__"
                 * all match each other via it. Use strcmp for exact match. */
                if (field->num_children == 0) {
                    if (strcmp(field->name, "__variant_begin__") == 0) {
                        if (offset % 2) offset++;
                        variant_start = offset;
                        variant_max_end = offset;
                        t->variant_start = offset;
                        current_arm = 1;  /* fields that follow belong to arm 1 */
                        bit_cursor = offset * 8;
                    } else if (strcmp(field->name, "__variant_arm__") == 0) {
                        /* Track the end of the previous arm, reset to variant start */
                        if (offset > variant_max_end) variant_max_end = offset;
                        offset = variant_start;
                        bit_cursor = offset * 8;
                        current_arm++;
                    } else if (strcmp(field->name, "__variant_end__") == 0) {
                        if (offset > variant_max_end) variant_max_end = offset;
                        offset = variant_max_end;
                        bit_cursor = offset * 8;
                        variant_start = -1;
                        current_arm = 0;
                    }
                    continue;
                }
                type_desc_t *ft = resolve_type(cg, field->children[0]);
                /* P80c: if resolve_type failed (field type not yet defined),
                 * try imported_types as fallback. Without this, record fields
                 * with types defined in later units get size 2 (default), and
                 * the record layout is wrong — e.g., hdr_freepool's firstfree
                 * (int4) gets size 2 instead of 4 when int4/longint isn't yet
                 * in the local type table. */
                if (!ft && field->children[0] && field->children[0]->name[0] &&
                    cg->imported_types) {
                    const char *tname = field->children[0]->name;
                    for (int it = 0; it < cg->imported_types_count; it++) {
                        if (str_eq_nocase(cg->imported_types[it].name, tname)) {
                            ft = &cg->imported_types[it];
                            break;
                        }
                    }
                }
                int fs = ft ? ft->size : 2;
                /* P87c: in packed records with nibble-packed fields (e.g.
                 * pmem), booleans collapse to 1 byte (and further to
                 * individual bits in the bit-packing path below). Other
                 * packed records (boolean-only like segstates) keep the
                 * unpacked default of 2 bytes so their layouts match
                 * hand-coded asm pin tables that have been validated over
                 * the existing boot sequence. */
                if (cg->in_packed && (record_has_nibble || record_has_variant) && ft &&
                    ft->kind == TK_BOOLEAN && fs == 2)
                    fs = 1;
                /* P87d: bit-packed field decision. Only in packed records
                 * that contain at least one Tnibble (range 0..15, 4 bits);
                 * such records can't be correctly laid out without bit
                 * splitting. For TK_BOOLEAN → 1 bit, for that Tnibble
                 * subrange → 4 bits. Gating on record_has_nibble leaves
                 * boolean-only packed records (segstates, flags bitfields
                 * elsewhere) at their existing byte-sized layout so the
                 * handwritten asm-pin tables stay consistent. */
                int packed_bits = 0;
                if (record_has_nibble && ft) {
                    if (ft->kind == TK_BOOLEAN) {
                        packed_bits = 1;
                    } else if (ft->kind == TK_SUBRANGE &&
                               ft->range_low == 0 && ft->range_high == 15) {
                        packed_bits = 4;
                    }
                }
                /* P79: In Lisa Pascal unpacked records, string fields are
                 * padded to even length. string[32] = 33 bytes → 34. */
                if (ft && ft->kind == TK_STRING && (fs % 2)) fs++;
                /* P79f: In Lisa Pascal unpacked records, byte-sized scalar
                 * fields (int1, char, byte) are widened to word size (2 bytes).
                 * Asm code reads these with MOVE.W (e.g., PRIORITY(A1) in PCB).
                 * Only widen scalars, not records/arrays/strings.
                 *
                 * P82c: only widen ISOLATED byte fields — pairs of consecutive
                 * byte fields stay tight so codesdb's lockcount(1)+sdbtype(1)
                 * end at offset 14 (= Apple's oset_freechain). Forward-peek
                 * by looking at the next AST child.
                 *
                 * P85c: byte-SUBRANGEs (e.g. `0..255` for priority/norm_pri)
                 * ALWAYS widen when at an even offset — Apple's ASM reads
                 * PCB.priority with MOVE.W PRIORITY(A1) expecting a word. At
                 * odd offsets they stay 1 byte (e.g. PCB.domain @17 packs
                 * tight with blk_state). TK_BYTE/TK_CHAR/TK_ENUM keep the
                 * pair-pack optimization so codesdb's lockcount(int1)+
                 * sdbtype(enum) pair remains tight at 12/13. */
                int widen_byte_subrange = 0;
                /* P85c: only INLINE subranges qualify for byte-subrange
                 * widening. Named type aliases that happen to resolve to a
                 * byte-sized subrange (e.g. `int1 = -128..127`,
                 * `domainRange = 0..maxDomain`) must not widen — Apple
                 * treats `int1` as an explicit 1-byte type. Inline subrange
                 * fields are AST_TYPE_SUBRANGE nodes; named references are
                 * AST_TYPE_IDENT.
                 *
                 * Further narrowed to PCB-only pending diagnosis of cascading
                 * failures in other records (hour/minute/second date fields,
                 * pm_* parallel-port fields etc.). PCB widening unblocks the
                 * QUEUE_PR RQSCAN spin; other records stay tight-packed so
                 * their asm/Pascal offsets match what they were with P82c. */
                int is_inline_subrange = (field->children[0] &&
                    field->children[0]->type == AST_TYPE_SUBRANGE) &&
                    (str_eq_nocase(t->name, "PCB") ||
                     str_eq_nocase(t->name, "pcb"));
                if (fs == 1 && ft && !cg->in_packed &&
                    (ft->kind == TK_BYTE || ft->kind == TK_CHAR ||
                     (ft->kind == TK_SUBRANGE && is_inline_subrange))) {
                    int should_widen = 1;
                    if (!is_inline_subrange) {
                        int next_is_byte = 0;
                        for (int j = i + 1; j < node->num_children; j++) {
                            ast_node_t *nxt = node->children[j];
                            if (nxt->type != AST_FIELD) continue;
                            if (nxt->num_children == 0) continue;  /* sentinel */
                            type_desc_t *nft = resolve_type(cg, nxt->children[0]);
                            if (nft && nft->size == 1 &&
                                (nft->kind == TK_BYTE || nft->kind == TK_CHAR ||
                                 nft->kind == TK_SUBRANGE || nft->kind == TK_ENUM))
                                next_is_byte = 1;
                            break;
                        }
                        if (next_is_byte) should_widen = 0;
                    }
                    /* Only widen when current offset is even; at odd offsets
                     * widening would pad and break alignment (see domain@17). */
                    if (should_widen && (offset % 2) == 0) {
                        fs = 2;
                        if (is_inline_subrange) widen_byte_subrange = 1;
                    }
                }
                /* Word-align fields — skipped in packed records so byte
                 * fields don't force a padding hole between odd-offset
                 * neighbors. */
                if (fs >= 2 && (offset % 2) && !cg->in_packed) offset++;
                /* P87d: packed bit-field placement. If this field is
                 * bit-packable (boolean or Tnibble in packed context),
                 * place it at the current bit_cursor; otherwise byte-align
                 * bit_cursor and advance by fs bytes. We keep `offset`
                 * synced with bit_cursor / 8 so downstream consumers
                 * (variant bookkeeping, t->size) still see byte-aligned
                 * numbers. Apple's pmem layout requires MSB-first packing:
                 * first declared field gets the HIGH bits of the byte, so
                 * bit_offset (LSB position) = 8 - (cursor_in_byte + W). */
                int placed_byte_offset = offset;
                int placed_bit_offset = 0;
                int placed_bit_width = 0;
                if (packed_bits > 0) {
                    int byte_idx = bit_cursor / 8;
                    int bit_in_byte = bit_cursor % 8;
                    /* Need packed_bits free bits in the current byte. If
                     * bit_in_byte + packed_bits > 8, advance to next byte. */
                    if (bit_in_byte + packed_bits > 8) {
                        byte_idx++;
                        bit_in_byte = 0;
                        bit_cursor = byte_idx * 8;
                    }
                    placed_byte_offset = byte_idx;
                    /* MSB-first: LSB position = 8 - used - width */
                    placed_bit_offset = 8 - bit_in_byte - packed_bits;
                    placed_bit_width = packed_bits;
                    bit_cursor += packed_bits;
                    /* Keep `offset` as the byte just after the highest byte
                     * this field could touch, matching the pre-existing
                     * invariant that offset == end-of-last-placed-field. */
                    offset = byte_idx + 1;
                } else {
                    /* Whole-byte (or multi-byte) field: byte-align cursor. */
                    if (bit_cursor % 8) bit_cursor = (bit_cursor + 7) & ~7;
                    if (bit_cursor / 8 > offset) offset = bit_cursor / 8;
                    placed_byte_offset = offset;
                    bit_cursor = (offset + fs) * 8;
                    /* P90: in packed mode, advance `offset` past this field
                     * (matching the bit-packed branch above and the unpacked
                     * `offset += fs` at line 623). Otherwise variant_max_end
                     * and the final t->size capture only placed_byte_offset,
                     * missing the field's size. c_ne ended up with size=2
                     * instead of 4 because `offset` stuck at 2 (the start of
                     * the `offset: integer` field) instead of 4 (past it),
                     * making LINK A6,#-22 too tight and the 4-byte
                     * `cracked_ne.l := NextEntry` stomp the saved A6. */
                    if (cg->in_packed) offset += fs;
                }
                /* P105: the parser collapses `a, b, c : type` into ONE
                 * AST_FIELD with name="a,b,c". Split on commas so each
                 * sub-field gets its own offset and size. Without this,
                 * `UID = record a,b: longint end;` comes out as 4 bytes
                 * instead of 8, shifting MDDFdb.MDDFaddr from +130 to
                 * +126 and breaking real_mount's (MDDFaddr <> 0) check. */
                const char *nm = field->name;
                int ns = 0;
                int names_off[16];
                int names_len[16];
                int cur_start = 0;
                for (int ci = 0; ; ci++) {
                    char c = nm[ci];
                    if (c == ',' || c == '\0') {
                        if (ns < 16 && ci > cur_start) {
                            names_off[ns] = cur_start;
                            names_len[ns] = ci - cur_start;
                            ns++;
                        }
                        cur_start = ci + 1;
                        if (c == '\0') break;
                    }
                }
                if (ns == 0) ns = 1; /* safety: treat empty name as one unnamed field */

                for (int si = 0; si < ns; si++) {
                    /* Re-align for each sub-field except the first (the
                     * outer alignment already ran for the first). */
                    if (si > 0) {
                        if (packed_bits == 0) {
                            if (fs >= 2 && (offset % 2) && !cg->in_packed) offset++;
                            if (bit_cursor % 8) bit_cursor = (bit_cursor + 7) & ~7;
                            if (bit_cursor / 8 > offset) offset = bit_cursor / 8;
                            placed_byte_offset = offset;
                            bit_cursor = (offset + fs) * 8;
                        } else {
                            int byte_idx = bit_cursor / 8;
                            int bit_in_byte = bit_cursor % 8;
                            if (bit_in_byte + packed_bits > 8) {
                                byte_idx++;
                                bit_in_byte = 0;
                                bit_cursor = byte_idx * 8;
                            }
                            placed_byte_offset = byte_idx;
                            placed_bit_offset = 8 - bit_in_byte - packed_bits;
                            bit_cursor += packed_bits;
                            offset = byte_idx + 1;
                        }
                    }

                    if (t->num_fields < 64) {
                        int nlen = names_len[si];
                        if (nlen > 63) nlen = 63;
                        if (ns == 1 && nm[0]) {
                            /* preserve original behavior: no commas, just copy */
                            strncpy(t->fields[t->num_fields].name, nm, 63);
                        } else if (nlen > 0) {
                            memcpy(t->fields[t->num_fields].name, nm + names_off[si], nlen);
                            t->fields[t->num_fields].name[nlen] = '\0';
                        } else {
                            t->fields[t->num_fields].name[0] = '\0';
                        }
                        t->fields[t->num_fields].name[63] = '\0';
                        t->fields[t->num_fields].type_name[0] = '\0';
                        if (field->children[0] && field->children[0]->name[0])
                            strncpy(t->fields[t->num_fields].type_name,
                                    field->children[0]->name, 11);
                        t->fields[t->num_fields].type_name[11] = '\0';
                        t->fields[t->num_fields].offset = placed_byte_offset +
                            (widen_byte_subrange ? 1 : 0);
                        t->fields[t->num_fields].bit_offset = (unsigned char)placed_bit_offset;
                        t->fields[t->num_fields].bit_width = (unsigned char)placed_bit_width;
                        t->fields[t->num_fields].type = ft;
                        t->fields[t->num_fields].variant_arm = (signed char)current_arm;
                        if (str_eq_nocase(t->fields[t->num_fields].name, "firstfree"))
                            fprintf(stderr, "  [RECORD-LAYOUT] '%s' field '%s' offset=%d fs=%d\n",
                                    t->name[0] ? t->name : "(anon)",
                                    t->fields[t->num_fields].name, offset, fs);
                        t->num_fields++;
                    }
                    /* Advance offset for each sub-field */
                    if (packed_bits == 0 && !cg->in_packed) offset += fs;
                }
            }
            /* Close out any trailing bits in packed mode; t->size always
             * reports a byte count. Unpacked records keep their legacy
             * even-byte pad. */
            if (cg->in_packed) {
                if (bit_cursor % 8) bit_cursor = (bit_cursor + 7) & ~7;
                offset = bit_cursor / 8;
            } else if (offset % 2) offset++;
            t->size = offset;
            /* P80g: post-creation record repair. If the freshly resolved
             * record has all-zero offsets (field types resolved to NULL →
             * default size 2 → offsets 0,2,4... but NOT all zero — this
             * catches cases where all field types have size 0), try to
             * use imported version's offsets directly. Actually, the more
             * common issue: pointer base types reference this record and
             * the pointer type stores a pointer to THIS (local) record.
             * If this record's offsets later get corrupted (by dangling
             * pointer reads), all code using the pointer type gets wrong
             * offsets. Prevent this by COPYING offsets from the imported
             * version into the local record, so both are consistent. */
            if (t->num_fields > 1 && cg->imported_types &&
                t->fields[1].offset == 0) {
                /* Try to find matching imported record and copy its offsets.
                 *
                 * P89d: ONLY apply when local fields[1].offset == 0, which is
                 * the corruption signature this repair was designed for.
                 * Previously the repair fired for any local record with >1
                 * fields whose first field name matched some imported
                 * record's first field name, even when the local had valid
                 * offsets. That merged structurally-similar but semantically-
                 * distinct records: MMPRIM's `p_linkage` (2 × ptr_p_linkage,
                 * 8 bytes) collided with DRIVERDEFS's `linkage` (2 × relptr,
                 * 4 bytes) because both start with fwd_link/bkwd_link. The
                 * loose match copied `linkage`'s offsets (0,2) over the
                 * correct `p_linkage` layout (0,4), truncating sdb.memaddr
                 * from offset 8 to offset 4 in MMPRIM's compile — which
                 * propagated to MAP_SYSLOCAL reading c_sysl_sdb^.memaddr
                 * from the wrong byte and writing origin=0 into
                 * SMT[syslocmmu], zeroing the syslocal MMU mapping at boot
                 * (1-milestone regression blocking SYS_PROC_INIT).
                 *
                 * Records we walked successfully have fields[1].offset > 0
                 * (the first non-zero layout), so they don't need repair. */
                for (int it = 0; it < cg->imported_types_count; it++) {
                    type_desc_t *imp = &cg->imported_types[it];
                    if (imp->kind != TK_RECORD || imp->num_fields != t->num_fields) continue;
                    /* Match by first field name (local has no name at this
                     * point — AST_TYPE_DECL sets it after this returns). */
                    if (!str_eq_nocase(imp->fields[0].name, t->fields[0].name)) continue;
                    if (imp->num_fields > 1 && imp->fields[1].offset > 0) {
                        /* Copy offsets from imported to local */
                        for (int fi = 0; fi < t->num_fields && fi < imp->num_fields; fi++)
                            t->fields[fi].offset = imp->fields[fi].offset;
                        t->size = imp->size;
                        break;
                    }
                }
            }
            /* P80b/c: debug print for records with key field names */
            if (t->num_fields >= 2) {
                for (int fi = 0; fi < t->num_fields; fi++) {
                    if (str_eq_nocase(t->fields[fi].name, "firstfree")) {
                        fprintf(stderr, "  [TYPE] %s (hdr_freepool-like): size=%d fields=%d\n",
                                t->name[0] ? t->name : "(anon)", t->size, t->num_fields);
                        for (int fj = 0; fj < t->num_fields; fj++)
                            fprintf(stderr, "    @%d %s sz=%d kind=%d\n", t->fields[fj].offset,
                                    t->fields[fj].name, t->fields[fj].type ? t->fields[fj].type->size : -1,
                                    t->fields[fj].type ? t->fields[fj].type->kind : -1);
                        break;
                    }
                    if (str_eq_nocase(t->fields[fi].name, "sds_sem")) {
                        fprintf(stderr, "  [TYPE] mmrb-like: size=%d fields=%d\n", t->size, t->num_fields);
                        for (int fj = 0; fj < t->num_fields; fj++)
                            fprintf(stderr, "    @%d %s sz=%d\n", t->fields[fj].offset,
                                    t->fields[fj].name, t->fields[fj].type ? t->fields[fj].type->size : -1);
                        break;
                    }
                }
            }
            return t;
        }

        case AST_TYPE_SET: {
            /* P65: size sets based on their base type's cardinality so
             * record layouts match Apple's. blk_type = set of wait_types
             * (5 values) must be 1 byte, NOT 32. Otherwise PCB.domain
             * lands at offset 48 instead of 17 (per PASCALDEFS).
             *
             * Lisa Pascal rounds the set size up to the next byte
             * boundary; small sets (<= 8 elements) take 1 byte,
             * 9..16 take 2 bytes, etc. Default to 32 bytes (256-bit)
             * when the base type is unknown. */
            int sz = 32;
            if (node->num_children > 0) {
                type_desc_t *base = resolve_type(cg, node->children[0]);
                int card = 0;
                if (base) {
                    if (base->kind == TK_ENUM) {
                        card = base->range_high - base->range_low + 1;
                        if (card <= 0 && base->size > 0)
                            card = base->size * 256;  /* rough estimate */
                    } else if (base->kind == TK_SUBRANGE) {
                        card = base->range_high - base->range_low + 1;
                    } else if (base->kind == TK_BOOLEAN) {
                        card = 2;
                    } else if (base->kind == TK_CHAR) {
                        card = 256;
                    }
                }
                if (card > 0) {
                    int bytes = (card + 7) / 8;
                    /* Align small set sizes to 1/2/4/32 byte slots */
                    if (bytes <= 1) sz = 1;
                    else if (bytes <= 2) sz = 2;
                    else if (bytes <= 4) sz = 4;
                    else sz = 32;
                }
            }
            type_desc_t *t = add_type(cg, "", TK_SET, sz);
            return t;
        }

        case AST_TYPE_PACKED:
            if (node->num_children > 0) {
                cg->in_packed++;
                type_desc_t *pt = resolve_type(cg, node->children[0]);
                cg->in_packed--;
                return pt;
            }
            return find_type(cg, "integer");

        case AST_TYPE_ENUM: {
            /* P82c: enums fitting in a byte are 1 byte natively (matches
             * Apple's layout — Tsdbtype (7 values) is 1 byte so codesdb's
             * case tag fits between lockcount and freechain at offset 14.
             * Record resolver still widens isolated byte fields to 2 for
             * asm-compat MOVE.W reads. */
            int card = 0;
            for (int i = 0; i < node->num_children; i++)
                if (node->children[i]->type == AST_IDENT_EXPR &&
                    node->children[i]->name[0])
                    card++;
            int enum_sz = (card > 0 && card <= 256) ? 1 : 2;
            type_desc_t *t = add_type(cg, "", TK_ENUM, enum_sz);
            /* P79e: register each enum value as a CONST with its ordinal.
             * Without this, identifiers like dsmake_nf (=1) resolve to 0
             * at call sites, breaking enum parameter passing.
             * P80f: also check imported_globals — if a CONST with the same
             * name was already defined (e.g., dsmake_nf=1 in a CONST section),
             * don't overwrite it with the enum ordinal (which would be 4). */
            for (int i = 0; i < node->num_children; i++) {
                if (node->children[i]->type == AST_IDENT_EXPR && node->children[i]->name[0]) {
                    cg_symbol_t *cs = find_global(cg, node->children[i]->name);
                    if (!cs) cs = find_imported(cg, node->children[i]->name);
                    if (!cs) {
                        cs = add_global_sym(cg, node->children[i]->name, t);
                        if (cs) {
                            cs->is_const = true;
                            cs->offset = i;  /* ordinal value */
                        }
                    }
                }
            }
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
        cg->scopes[cg->scope_depth].proc_name[0] = '\0';
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
 * depth=1: parent scope  → A0 = -4(A6) (static link)
 * depth=2: grandparent   → A0 = -4(A6); A0 = -4(A0)
 * Result in A6 (for depth 0) or A0 (for depth > 0).
 *
 * P81: walks the STATIC link chain via the -4(A6) slot that each nested
 * proc saves on entry, not the DYNAMIC link (saved A6 from LINK). The
 * dynamic link equals the static link only when the callee is called by
 * its direct static parent; for sibling-nested calls the dynamic link
 * points at the caller, which is a SIBLING of the static parent — not
 * the parent itself. Using (A6) for outer-scope access then corrupted
 * reads (e.g. SET_INMOTION_SEG reading MOVE_SEG's locals instead of
 * CLEAR_SPACE's, straddling frame-overhead bytes). */
/* P81 static-link ABI: emit the load of A2 with the correct static-parent
 * frame pointer for a call to `sig`. Must be called AFTER all argument
 * pushes and IMMEDIATELY before the JSR so that A2 isn't clobbered by
 * any nested calls inside arg evaluation. Safe to call with sig==NULL or
 * for callees that don't take a static link — in that case, no-op. */
static void emit_static_link_load(codegen_t *cg, cg_proc_sig_t *sig) {
    if (!sig || !sig->takes_static_link) return;
    /* walk_count = caller_depth - callee_depth + 1.
     * - 0: caller is direct static parent of callee → pass caller's A6.
     * - 1: caller is sibling of callee (same depth) → pass caller's static link.
     * - N: caller is N levels deeper → walk N times via static link chain. */
    int caller_depth = cg->scope_depth;
    int callee_depth = sig->nest_depth;
    int walk = caller_depth - callee_depth + 1;
    if (walk < 0) walk = 0;  /* shouldn't happen, but be safe */
    if (walk == 0) {
        emit16(cg, 0x244E);  /* MOVEA.L A6,A2 */
        return;
    }
    /* First hop from A6's static link slot */
    emit16(cg, 0x246E);  /* MOVEA.L d16(A6),A2 */
    emit16(cg, 0xFFFC);  /* disp = -4 */
    for (int i = 1; i < walk; i++) {
        emit16(cg, 0x246A);  /* MOVEA.L d16(A2),A2 */
        emit16(cg, 0xFFFC);  /* disp = -4 */
    }
}

static void emit_frame_access(codegen_t *cg, int depth) {
    if (depth <= 0) return;  /* Current scope, A6 is fine */
    /* First hop: load static link from caller's -4(A6) slot into A0. */
    emit16(cg, 0x206E);  /* MOVEA.L d16(A6),A0 */
    emit16(cg, 0xFFFC);  /* disp = -4 */
    /* Subsequent hops: follow A0's -4(A0) static link. */
    for (int i = 1; i < depth; i++) {
        emit16(cg, 0x2068);  /* MOVEA.L d16(A0),A0 */
        emit16(cg, 0xFFFC);  /* disp = -4 */
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
    if (s) return s;
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
static int expr_size(codegen_t *cg, ast_node_t *node);

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
    /* Also patch MOVE.W (A0),D0 → MOVE.L (A0),D0 (P3 pattern:
     * dereferencing a pointer whose type was unresolved, resulting
     * in a 16-bit read of what should be a 32-bit pointer value). */
    if (after - before >= 2) {
        uint16_t opword = (cg->code[after - 2] << 8) | cg->code[after - 1];
        if (opword == 0x3010) {  /* MOVE.W (A0),D0 */
            cg->code[after - 2] = 0x20;  /* → MOVE.L (A0),D0 = 0x2010 */
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

/* Forward decls */
static type_desc_t *repair_corrupt_record(codegen_t *cg, type_desc_t *rt);
static int with_lookup_field(codegen_t *cg, const char *name,
                             type_desc_t **out_type, int *out_with_idx);
static type_desc_t *lvalue_record_type(codegen_t *cg, ast_node_t *node);

/* P82: resolve the record/pointer type of an lvalue expression.
 * Returns the TK_RECORD type that `node` evaluates to (i.e., the type one
 * would dereference to access fields). Handles IDENT_EXPR, DEREF (p^),
 * and nested AST_FIELD_ACCESS chains like `rec.sub.subsub`.
 *
 * Without this, the AST_FIELD_ACCESS handler in gen_lvalue_addr only knew
 * how to resolve a field on a plain var or a single dereference — so chains
 * like `c_mmrb^.head_sdb.freechain.fwd_link` lost the middle offsets,
 * emitting `ADDA.W #42` (head_sdb offset) but NOT the +14 for freechain,
 * and reading as MOVE.W (2 bytes) instead of MOVE.L (4 bytes). */
static type_desc_t *lvalue_record_type(codegen_t *cg, ast_node_t *node) {
    if (!node) return NULL;
    if (node->type == AST_IDENT_EXPR) {
        cg_symbol_t *sym = find_symbol_any(cg, node->name);
        if (sym && sym->type) {
            type_desc_t *t = sym->type;
            if (t->kind == TK_POINTER && t->base_type) t = t->base_type;
            return (t && t->kind == TK_RECORD) ? t : NULL;
        }
        if (cg->with_depth > 0) {
            type_desc_t *wrt = NULL;
            int wfld = with_lookup_field(cg, node->name, &wrt, NULL);
            if (wfld >= 0 && wrt) {
                type_desc_t *ft = wrt->fields[wfld].type;
                if (ft && ft->kind == TK_POINTER && ft->base_type) ft = ft->base_type;
                return (ft && ft->kind == TK_RECORD) ? ft : NULL;
            }
        }
        return NULL;
    }
    if (node->type == AST_DEREF) {
        /* p^ — get pointer's base-type record */
        ast_node_t *child = node->children[0];
        if (!child) return NULL;
        if (child->type == AST_IDENT_EXPR) {
            cg_symbol_t *psym = find_symbol_any(cg, child->name);
            if (psym && psym->type && psym->type->kind == TK_POINTER &&
                psym->type->base_type)
                return psym->type->base_type->kind == TK_RECORD
                           ? psym->type->base_type : NULL;
            if (!psym && cg->with_depth > 0) {
                type_desc_t *wrt = NULL;
                int wfld = with_lookup_field(cg, child->name, &wrt, NULL);
                if (wfld >= 0 && wrt) {
                    type_desc_t *ft = wrt->fields[wfld].type;
                    if (ft && ft->kind == TK_POINTER && ft->base_type &&
                        ft->base_type->kind == TK_RECORD)
                        return ft->base_type;
                }
            }
            return NULL;
        }
        if (child->type == AST_FIELD_ACCESS) {
            /* (rec.field)^ — resolve rec.field's type, which must be a pointer. */
            type_desc_t *parent = lvalue_record_type(cg, child->children[0]);
            if (parent) parent = repair_corrupt_record(cg, parent);
            if (parent && parent->kind == TK_RECORD) {
                for (int fi = 0; fi < parent->num_fields; fi++) {
                    if (str_eq_nocase(parent->fields[fi].name, child->name)) {
                        type_desc_t *ft = parent->fields[fi].type;
                        if (ft && ft->kind == TK_POINTER && ft->base_type &&
                            ft->base_type->kind == TK_RECORD)
                            return ft->base_type;
                        break;
                    }
                }
            }
            return NULL;
        }
        if (child->type == AST_ARRAY_ACCESS) {
            /* P100: arr[i]^ — element type is a pointer, deref gives its
             * record base. Same pattern as P97 in AST_WITH, surfaces here
             * for `configinfo[i]^.devname` in FIND_EMPTYSLOT — without this,
             * the field offset for devname resolves to 0, so the byte compare
             * reads devrec's entry_pt bytes instead of devname, every slot
             * compares unequal to 'BITBKT', and FIND_EMPTYSLOT returns
             * config_index=0 unconditionally. Every INIT_BOOT_CDS iteration
             * then reuses slot 0, producing the self-referential required_drvr
             * that needs the P97 scaffold. */
            ast_node_t *arr = child->children[0];
            type_desc_t *at = NULL;
            if (arr && arr->type == AST_IDENT_EXPR) {
                cg_symbol_t *sym = find_symbol_any(cg, arr->name);
                if (sym && sym->type) at = sym->type;
                else if (cg->with_depth > 0) {
                    type_desc_t *wrt = NULL;
                    int wfld = with_lookup_field(cg, arr->name, &wrt, NULL);
                    if (wfld >= 0 && wrt) at = wrt->fields[wfld].type;
                }
            }
            if (at && at->kind == TK_POINTER && at->base_type) at = at->base_type;
            if (at && at->kind == TK_ARRAY && at->element_type) {
                type_desc_t *et = at->element_type;
                if (et && et->kind == TK_POINTER && et->base_type) et = et->base_type;
                return (et && et->kind == TK_RECORD) ? et : NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (node->type == AST_FIELD_ACCESS) {
        type_desc_t *parent = lvalue_record_type(cg, node->children[0]);
        if (parent) parent = repair_corrupt_record(cg, parent);
        if (parent && parent->kind == TK_RECORD) {
            for (int fi = 0; fi < parent->num_fields; fi++) {
                if (str_eq_nocase(parent->fields[fi].name, node->name)) {
                    type_desc_t *ft = parent->fields[fi].type;
                    if (ft && ft->kind == TK_POINTER && ft->base_type)
                        ft = ft->base_type;
                    return (ft && ft->kind == TK_RECORD) ? ft : NULL;
                }
            }
        }
        return NULL;
    }
    /* P92: array element lookup. `arr[i]` for `arr: array[..] of record {..}`
     * must resolve to the element RECORD type so `arr[i].field` can find
     * `field` and its size. Without this, `jt[i].routine := @CANCEL_REQ`
     * in INIT_JTDRIVER's `with jtpointer^^ do` WITH block took expr_size's
     * default of 2 for the 4-byte `routine: ^integer` field — compiling
     * the store as MOVE.W instead of MOVE.L, truncating each driver-jump-
     * table routine pointer to its low 16 bits. When the OS later
     * dispatched through jt[0], it jumped to $00007EA2 (low word of
     * $00037EA2 = @CANCEL_REQ) and faulted with SYSTEM_ERROR code=204. */
    if (node->type == AST_ARRAY_ACCESS) {
        ast_node_t *arr = node->children[0];
        if (!arr) return NULL;
        type_desc_t *at = NULL;
        if (arr->type == AST_IDENT_EXPR) {
            cg_symbol_t *sym = find_symbol_any(cg, arr->name);
            if (sym && sym->type) at = sym->type;
            else if (cg->with_depth > 0) {
                type_desc_t *wrt = NULL;
                int wfld = with_lookup_field(cg, arr->name, &wrt, NULL);
                if (wfld >= 0 && wrt) at = wrt->fields[wfld].type;
            }
        } else if (arr->type == AST_DEREF && arr->children[0] &&
                   arr->children[0]->type == AST_IDENT_EXPR) {
            /* P127g: ptr^[i].field case — e.g. c_mrbt^[stackmmu].sdbRP.
             * The base is `ptr^`, an array. Resolve ptr's type chain:
             * ptr → ^array → array (element_type). Previously this fell
             * through to NULL, so field_off defaulted to 0 and array
             * element reads picked up the WRONG field (sdbRP → access). */
            cg_symbol_t *psym = find_symbol_any(cg, arr->children[0]->name);
            if (psym && psym->type && psym->type->kind == TK_POINTER &&
                psym->type->base_type) {
                at = psym->type->base_type;  /* ptr to array → array */
            } else if (!psym && cg->with_depth > 0) {
                type_desc_t *wrt = NULL;
                int wfld = with_lookup_field(cg, arr->children[0]->name, &wrt, NULL);
                if (wfld >= 0 && wrt) {
                    type_desc_t *ft = wrt->fields[wfld].type;
                    if (ft && ft->kind == TK_POINTER && ft->base_type)
                        at = ft->base_type;
                }
            }
        }
        if (at && at->kind == TK_POINTER && at->base_type) at = at->base_type;
        if (at && at->kind == TK_ARRAY && at->element_type) {
            type_desc_t *et = at->element_type;
            if (et->kind == TK_POINTER && et->base_type) et = et->base_type;
            return (et && et->kind == TK_RECORD) ? et : NULL;
        }
        return NULL;
    }
    return NULL;
}

/* P82: resolve field metadata (offset + type) on an lvalue expression
 * `parent.field`. Returns true on success.
 *
 * P87d: also returns bit_offset / bit_width when non-null. bit_width == 0
 * means "whole-byte field". Callers that don't care can pass NULL for
 * the bit pointers. */
static bool lvalue_field_info_full(codegen_t *cg, ast_node_t *parent_node,
                                   const char *field_name, int *out_offset,
                                   type_desc_t **out_type,
                                   int *out_bit_offset, int *out_bit_width) {
    type_desc_t *parent = lvalue_record_type(cg, parent_node);
    if (!parent) return false;
    parent = repair_corrupt_record(cg, parent);
    for (int fi = 0; fi < parent->num_fields; fi++) {
        if (str_eq_nocase(parent->fields[fi].name, field_name)) {
            if (out_offset) *out_offset = parent->fields[fi].offset;
            if (out_type) *out_type = parent->fields[fi].type;
            if (out_bit_offset) *out_bit_offset = parent->fields[fi].bit_offset;
            if (out_bit_width) *out_bit_width = parent->fields[fi].bit_width;
            return true;
        }
    }
    return false;
}

static bool lvalue_field_info(codegen_t *cg, ast_node_t *parent_node,
                              const char *field_name, int *out_offset,
                              type_desc_t **out_type) {
    return lvalue_field_info_full(cg, parent_node, field_name,
                                  out_offset, out_type, NULL, NULL);
}

/* P80f: repair a corrupt record type by finding the imported version.
 * Returns the repaired type, or the original if no repair is needed/possible. */
static type_desc_t *repair_corrupt_record(codegen_t *cg, type_desc_t *rt) {
    if (!rt || rt->kind != TK_RECORD || rt->num_fields <= 1) return rt;
    /* Check for all-zero offsets (corruption signature) */
    int all_zero = 1;
    for (int fj = 1; fj < rt->num_fields; fj++)
        if (rt->fields[fj].offset != 0) { all_zero = 0; break; }
    if (!all_zero) return rt;  /* Offsets look valid */
    if (!cg->imported_types) return rt;
    /* Search imported types for a matching record with valid offsets.
     * Match by name if available, or by field count + first field name
     * for anonymous records. */
    for (int it = 0; it < cg->imported_types_count; it++) {
        type_desc_t *imp = &cg->imported_types[it];
        if (imp->kind != TK_RECORD || imp->num_fields != rt->num_fields) continue;
        /* Named match */
        if (rt->name[0] && imp->name[0] && !str_eq_nocase(imp->name, rt->name)) continue;
        /* Anonymous match: compare first field name */
        if (!rt->name[0] && rt->num_fields > 0 &&
            !str_eq_nocase(imp->fields[0].name, rt->fields[0].name)) continue;
        if (imp->num_fields > 1 && imp->fields[1].offset > 0) {
            static int fix_count = 0;
            if (fix_count++ < 20)
                fprintf(stderr, "  [P80f-REPAIR] '%s' (%d fields) → imported with field[1]@%d\n",
                        rt->name, rt->num_fields, imp->fields[1].offset);
            return imp;
        }
    }
    return rt;
}

/* Check if an identifier is a field of an active WITH record.
 * Returns the field index and sets *out_type to the record type,
 * or returns -1 if not found. Searches from innermost WITH outward. */
static int with_lookup_field(codegen_t *cg, const char *name,
                             type_desc_t **out_type, int *out_with_idx) {
    for (int w = cg->with_depth - 1; w >= 0; w--) {
        type_desc_t *rt = cg->with_stack[w].record_type;
        if (!rt || rt->kind != TK_RECORD) continue;
        /* P80f: auto-repair corrupt records on first access */
        type_desc_t *repaired = repair_corrupt_record(cg, rt);
        if (repaired != rt) {
            rt = repaired;
            cg->with_stack[w].record_type = rt;
        }
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

/* P77: Check whether an RHS expression has any sub-expression that produces
 * a 32-bit value (longint, pointer, address-of, func_call). When this is
 * true, the D0 register already contains a valid 32-bit result after
 * gen_expression, and emitting EXT.L would DESTROY the upper 16 bits.
 * This generalizes P54's FUNC_CALL-only check. */
static bool rhs_has_wide_operand(codegen_t *cg, ast_node_t *node) {
    if (!node) return false;
    switch (node->type) {
        case AST_FUNC_CALL:
        case AST_ADDR_OF:
        case AST_STRING_LITERAL:
            return true;
        case AST_INT_LITERAL:
            return (node->int_val < -32768 || node->int_val > 32767);
        case AST_IDENT_EXPR: {
            cg_symbol_t *sym = find_symbol_any(cg, node->name);
            if (sym && sym->type &&
                (sym->type->kind == TK_POINTER || sym->type->kind == TK_LONGINT))
                return true;
            if (sym && sym->is_const) {
                int cv = sym->offset;
                return (cv < -32768 || cv > 32767);
            }
            if (cg->with_depth > 0) {
                type_desc_t *wrt = NULL;
                int fld = with_lookup_field(cg, node->name, &wrt, NULL);
                if (fld >= 0 && wrt && wrt->fields[fld].type &&
                    (wrt->fields[fld].type->kind == TK_POINTER ||
                     wrt->fields[fld].type->kind == TK_LONGINT))
                    return true;
            }
            return false;
        }
        case AST_BINARY_OP:
            return (node->num_children >= 2 &&
                    (rhs_has_wide_operand(cg, node->children[0]) ||
                     rhs_has_wide_operand(cg, node->children[1])));
        case AST_UNARY_OP:
            return (node->num_children >= 1 &&
                    rhs_has_wide_operand(cg, node->children[0]));
        case AST_FIELD_ACCESS:
        case AST_DEREF:
        case AST_ARRAY_ACCESS:
            return (expr_size(cg, node) >= 4);
        default:
            return false;
    }
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
            /* P93: same scope rule as gen_lvalue_addr — WITH-record fields
             * shadow same-named globals inside a WITH block. Check WITH
             * context BEFORE falling through to find_symbol_any, or
             * e_name/string field sizes get mis-resolved to a stray global's
             * size (e.g. LD_OPENINPUT's cheater.path got sized as 256 bytes
             * instead of e_name's 33, causing the aggregate-copy to DBRA
             * 256 times into adjacent stack bytes). */
            if (cg->with_depth > 0) {
                type_desc_t *wrt = NULL;
                int fld = with_lookup_field(cg, node->name, &wrt, NULL);
                if (fld >= 0 && wrt && wrt->fields[fld].type)
                    return type_load_size(wrt->fields[fld].type);
            }
            cg_symbol_t *sym = find_symbol_any(cg, node->name);
            /* Constants: size is determined by VALUE, not declared type.
             * A const like maxmmusize=131072 is typed "integer" but needs
             * 4 bytes. Check is_const BEFORE type to avoid EXT.L corruption. */
            if (sym && sym->is_const) {
                int cv = sym->offset;
                return (cv < -32768 || cv > 32767) ? 4 : 2;
            }
            if (sym && sym->type) return type_load_size(sym->type);
            /* Check built-in identifiers */
            if (str_eq_nocase(node->name, "nil")) return 4;
            if (str_eq_nocase(node->name, "true") || str_eq_nocase(node->name, "false")) return 2;
            return 2;
        }
        case AST_FIELD_ACCESS: {
            /* P82: generic chain-aware size lookup — handles nested field
             * accesses like `rec.sub.field` where the child is another
             * AST_FIELD_ACCESS. Without this, expr_size defaulted to 2,
             * causing MOVE.W instead of MOVE.L on 4-byte pointer fields
             * in chains — see head_sdb.freechain.fwd_link in FIND_FREE. */
            {
                int _off = 0;
                type_desc_t *_ft = NULL;
                if (lvalue_field_info(cg, node->children[0], node->name,
                                      &_off, &_ft) && _ft)
                    return type_load_size(_ft);
            }
            /* Look up the field type in the record.
             * Must handle both rec.field (IDENT_EXPR) and ptr^.field (DEREF). */
            type_desc_t *rt = NULL;
            if (node->children[0] && node->children[0]->type == AST_IDENT_EXPR) {
                cg_symbol_t *rec = find_symbol_any(cg, node->children[0]->name);
                if (rec && rec->type) {
                    rt = rec->type;
                    if (rt->kind == TK_POINTER && rt->base_type) rt = rt->base_type;
                } else if (cg->with_depth > 0) {
                    /* `foo.bar` where foo is a field of an outer WITH record.
                     * Resolve foo via WITH stack so we can return bar's real
                     * type (and thus width — critical for 4-byte pointer
                     * stores that would otherwise truncate to MOVE.W). */
                    type_desc_t *wrt = NULL;
                    int fld = with_lookup_field(cg, node->children[0]->name, &wrt, NULL);
                    if (fld >= 0 && wrt) {
                        type_desc_t *ft = wrt->fields[fld].type;
                        if (ft && ft->kind == TK_POINTER && ft->base_type)
                            ft = ft->base_type;
                        rt = ft;
                    }
                }
            } else if (node->children[0] && node->children[0]->type == AST_DEREF) {
                ast_node_t *ptr_node = node->children[0]->children[0];
                if (ptr_node && ptr_node->type == AST_IDENT_EXPR) {
                    cg_symbol_t *ptr_sym = find_symbol_any(cg, ptr_node->name);
                    if (ptr_sym && ptr_sym->type && ptr_sym->type->kind == TK_POINTER &&
                        ptr_sym->type->base_type) {
                        rt = ptr_sym->type->base_type;
                        if (rt->kind == TK_POINTER && rt->base_type) rt = rt->base_type;
                    } else if (!ptr_sym && cg->with_depth > 0) {
                        /* WITH field whose value is a pointer: p^.f */
                        type_desc_t *wrt = NULL;
                        int fld = with_lookup_field(cg, ptr_node->name, &wrt, NULL);
                        if (fld >= 0 && wrt) {
                            type_desc_t *ft = wrt->fields[fld].type;
                            if (ft && ft->kind == TK_POINTER && ft->base_type)
                                rt = ft->base_type;
                        }
                    }
                } else if (ptr_node && ptr_node->type == AST_FIELD_ACCESS) {
                    /* `rec.field^.subfield` — resolve rec.field's type.
                     * rec.field has pointer type ptr_p; dereferenced yields p;
                     * .subfield lookup needs p. */
                    ast_node_t *fa = ptr_node;
                    type_desc_t *base = NULL;
                    if (fa->children[0] && fa->children[0]->type == AST_IDENT_EXPR) {
                        cg_symbol_t *bs = find_symbol_any(cg, fa->children[0]->name);
                        if (bs && bs->type) {
                            base = bs->type;
                            if (base->kind == TK_POINTER && base->base_type)
                                base = base->base_type;
                        } else if (!bs && cg->with_depth > 0) {
                            type_desc_t *wrt = NULL;
                            int wfld = with_lookup_field(cg, fa->children[0]->name, &wrt, NULL);
                            if (wfld >= 0 && wrt) {
                                base = wrt->fields[wfld].type;
                                if (base && base->kind == TK_POINTER && base->base_type)
                                    base = base->base_type;
                            }
                        }
                    }
                    if (base && base->kind == TK_RECORD) {
                        for (int i = 0; i < base->num_fields; i++) {
                            if (str_eq_nocase(base->fields[i].name, fa->name)) {
                                type_desc_t *ft = base->fields[i].type;
                                if (ft && ft->kind == TK_POINTER && ft->base_type)
                                    rt = ft->base_type;
                                break;
                            }
                        }
                    }
                }
            }
            /* P80f: repair corrupt record before field lookup */
            if (rt) rt = repair_corrupt_record(cg, rt);
            if (rt && rt->kind == TK_RECORD) {
                for (int i = 0; i < rt->num_fields; i++) {
                    if (str_eq_nocase(rt->fields[i].name, node->name)) {
                        if (rt->fields[i].type)
                            return type_load_size(rt->fields[i].type);
                        break;
                    }
                }
            }
            return 2;
        }
        case AST_ARRAY_ACCESS: {
            if (node->children[0] && node->children[0]->type == AST_IDENT_EXPR) {
                cg_symbol_t *arr = find_symbol_any(cg, node->children[0]->name);
                type_desc_t *at = (arr && arr->type) ? arr->type : NULL;
                /* WITH-field array lookup: arr is a field of an enclosing
                 * WITH record (e.g. `devconfig[i] := empty` inside
                 * `With PMRec^ do`), so find_symbol_any misses it. Without
                 * this fallback, expr_size defaults to 2 and a byte-array
                 * store emits MOVE.W, stomping past the intended element.
                 * Scoped to byte elements only — returning a resolved 4 for
                 * pointer-of-arrays from the WITH path regressed FS_INIT
                 * with unrelated downstream side-effects. */
                if (!at && cg->with_depth > 0) {
                    type_desc_t *wrt = NULL;
                    int wfld = with_lookup_field(cg, node->children[0]->name, &wrt, NULL);
                    if (wfld >= 0 && wrt)
                        at = wrt->fields[wfld].type;
                }
                if (at) {
                    if (at->kind == TK_POINTER && at->base_type) at = at->base_type;
                    if (at->kind == TK_ARRAY && at->element_type) {
                        int es = type_load_size(at->element_type);
                        if (es == 1) return 1;
                        if (arr) return es;
                        /* P106: WITH-field array of non-pointer elements
                         * (longint/integer/subrange). `ar10: array[0..9] of
                         * longint` inside `with cparm do` — codegen was
                         * reading only the high word of each 4-byte element
                         * because expr_size fell through to 2 whenever arr
                         * was NULL (WITH field has no global symbol). That
                         * turned real_mount's `ar10[0] <> SPARES_INTACT(4)`
                         * test into (high-word=0) <> 4 = TRUE, firing
                         * E_SPARES_DAMAGED on a healthy spare-table reply.
                         * Scoped to non-pointer element types — the prior
                         * comment warns about pointer-of-arrays regressing
                         * FS_INIT when we returned 4 unconditionally. */
                        if (at->element_type->kind != TK_POINTER)
                            return es;
                    }
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

/* Emit size-appropriate MOVE (A0),D0 — reads value from address in A0.
 * P80h2: byte loads zero-extend via MOVEQ #0,D0 first. Pascal subrange
 * 0..255 (e.g. PCB.priority) is a 1-byte field; plain MOVE.B leaves the
 * upper 24 bits of D0 with whatever was there (often a pointer value),
 * which poisons any subsequent MOVE.W D0,D2 sign/range test. The
 * Scheduler's `candidate^.priority > 0` check produced false negatives
 * whenever the candidate pointer's lower byte had bit 7 set. */
static void emit_read_a0_to_d0(codegen_t *cg, int sz) {
    if (sz == 4)      emit16(cg, 0x2010);  /* MOVE.L (A0),D0 */
    else if (sz == 1) {
        emit16(cg, 0x7000);                /* MOVEQ #0,D0 — zero-extend byte */
        emit16(cg, 0x1010);                /* MOVE.B (A0),D0 */
    }
    else              emit16(cg, 0x3010);  /* MOVE.W (A0),D0 */
}

/* P90: sign-extend a signed byte-subrange type (e.g. pmbyte = -128..127).
 * Call right after any byte read that put D0 = $000000XX (zero-extended).
 * Converts to $FFFFFFXX for values >= $80, preserving 32-bit sign for
 * word/long compares. Without this, MAKE_INTERNAL's `if chan = empty (-1)`
 * fails: chan byte $FF reads as word $00FF, literal -1 reads as $FFFF,
 * and the match never fires — chan never becomes emptychan, FIND_PM_IDS
 * returns false, and boot takes the twiggy-fallback SYSTEM_ERROR(10738). */
static bool type_is_signed_byte(type_desc_t *t) {
    return (t && t->kind == TK_SUBRANGE && t->size == 1 && t->range_low < 0);
}
static void emit_sign_ext_byte(codegen_t *cg) {
    emit16(cg, 0x4880);  /* EXT.W D0 */
    emit16(cg, 0x48C0);  /* EXT.L D0 */
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

/* P87d: bit-field read. A0 holds the address of the byte containing the
 * field. bit_offset = LSB position (0..7), bit_width = field width (1..7).
 * On exit D0 holds the zero-extended field value. Uses only D0. */
static void emit_read_a0_to_d0_bit(codegen_t *cg, int bit_offset, int bit_width) {
    emit16(cg, 0x7000);                /* MOVEQ #0,D0 */
    emit16(cg, 0x1010);                /* MOVE.B (A0),D0 */
    if (bit_offset > 0) {
        /* LSR.B #bit_offset,D0 — 68k shift immediate uses ROR/LSR with
         * count field in bits 9-11; encoding `1110 ccc0 ss 001 reg` where
         * cc=count (0=8,1..7=that count), ss=size (00=byte), dr=0 (right),
         * ir=0 (immediate), type=01 (LSR). */
        uint16_t cc = (bit_offset & 7);
        emit16(cg, 0xE008 | (cc << 9));  /* LSR.B #cc,D0 */
    }
    int mask = (1 << bit_width) - 1;
    emit16(cg, 0x0200);                /* ANDI.B #imm8,D0 */
    emit16(cg, (uint16_t)mask);        /* immediate byte (pads to word) */
}

/* P87d: bit-field write. Caller has already pushed the value as a word
 * via `MOVE.W D0,-(SP)` and put the byte address in A0. On exit, that
 * byte has its [bit_offset .. bit_offset + bit_width - 1] bits replaced
 * with the low `bit_width` bits of the saved value. The remaining bits
 * of the byte are preserved. Clobbers D0, D1. */
static void emit_write_d0_to_a0_bit(codegen_t *cg, int bit_offset, int bit_width) {
    int mask = (1 << bit_width) - 1;
    int shifted_mask = mask << bit_offset;
    emit16(cg, 0x1210);                /* MOVE.B (A0),D1 */
    emit16(cg, 0x301F);                /* MOVE.W (SP)+,D0 */
    emit16(cg, 0x0200);                /* ANDI.B #mask,D0 */
    emit16(cg, (uint16_t)mask);
    if (bit_offset > 0) {
        uint16_t cc = (bit_offset & 7);
        emit16(cg, 0xE108 | (cc << 9));  /* LSL.B #cc,D0 */
    }
    emit16(cg, 0x0201);                /* ANDI.B #~shifted_mask,D1 */
    emit16(cg, (uint16_t)(0xFF & ~shifted_mask));
    emit16(cg, 0x8200);                /* OR.B D0,D1 (D1 is dest) */
    emit16(cg, 0x1081);                /* MOVE.B D1,(A0) */
}

/* Load a variable's address into A0 */
static void gen_lvalue_addr(codegen_t *cg, ast_node_t *node) {
    if (node->type == AST_IDENT_EXPR) {
        /* P93: inside a WITH block, a bare identifier that matches a
         * field of the active WITH record must resolve to that field,
         * not to a same-named global. Lisa Pascal's scope rules put
         * the with-record fields ABOVE globals. Without this, LD_OPENINPUT's
         * `path := inputfile` (inside `with cheater do`) resolved `path` to
         * some unrelated global symbol and aggregate-copied the filename
         * there instead of into cheater.path — so the subsequent
         * LDR_CALL(cheater) shipped an empty e_name to the loader. */
        if (cg->with_depth > 0) {
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
                return;
            }
        }
        cg_symbol_t *sym = find_symbol_any(cg, node->name);
        if (sym) {
            /* P127: by-ref param detection. Our calling convention passes
             * records/arrays/strings (size > 4) as 4-byte pointers even
             * when declared without `var`. The frame slot holds a pointer
             * to the data, not the data itself — so field/element access
             * must deref, same as explicit VAR params. Without this, a
             * nested proc reading (say) `stk_info.stk_delta` would compute
             * (slot_addr + field_offset) and read garbage from the frame
             * past the slot, when what's needed is (*slot + field_offset). */
            bool is_byref_val = (sym->is_param && !sym->is_var_param &&
                                 sym->type && sym->type->size > 4);
            if ((sym->is_param && sym->is_var_param) || is_byref_val) {
                /* VAR param OR by-ref-value record/array/string:
                 * frame slot contains a pointer to the data */
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
            /* P90: protect A0 (the base addr) across the index expression.
             * If the index involves a WITH-field lookup or any address-of,
             * `gen_expression` clobbers A0 by loading a new base into it
             * (gen_with_base, AST_DEREF, etc.). In GetNxtConfig, the
             * `PMRec[offset]` access where `offset` is a WITH-field of
             * cracked_ne silently dropped the PMRec base, and CRAK_PM got
             * called with a pointer into the caller's stack frame instead
             * of into mypmem — producing entirely bogus pos decoding. */
            emit16(cg, 0x2F08);  /* MOVE.L A0,-(SP) */
            gen_expression(cg, node->children[1]); /* index in D0 */
            emit16(cg, 0x205F);  /* MOVEA.L (SP)+,A0 */
            /* Resolve element size from the array type */
            int elem_size = 2;  /* default word */
            int array_low = 0;
            if (node->children[0]->type == AST_FIELD_ACCESS &&
                node->children[0]->num_children > 0) {
                /* P79f: array access on a record field (e.g. sctab.sc_par_no[i]).
                 * Resolve the record's field type to get the array element size.
                 * Without this, elem_size defaults to 2, making byte-array
                 * strides double (sc_par_no overflows into b_syslocal_ptr). */
                ast_node_t *base_node = node->children[0]->children[0];
                cg_symbol_t *rec_sym = NULL;
                if (base_node && base_node->type == AST_IDENT_EXPR)
                    rec_sym = find_symbol_any(cg, base_node->name);
                type_desc_t *rec_type = (rec_sym && rec_sym->type) ? rec_sym->type : NULL;
                if (rec_type && rec_type->kind == TK_POINTER && rec_type->base_type)
                    rec_type = rec_type->base_type;
                if (rec_type && rec_type->kind == TK_RECORD && node->children[0]->name[0]) {
                    for (int f = 0; f < rec_type->num_fields; f++) {
                        if (str_eq_nocase(rec_type->fields[f].name, node->children[0]->name)) {
                            type_desc_t *ft = rec_type->fields[f].type;
                            if (ft && ft->kind == TK_ARRAY) {
                                array_low = ft->array_low;
                                if (ft->element_type)
                                    elem_size = type_size(ft->element_type);
                            }
                            break;
                        }
                    }
                }
            } else if (node->children[0]->type == AST_DEREF &&
                       node->children[0]->children[0] &&
                       node->children[0]->children[0]->type == AST_IDENT_EXPR) {
                /* ptr^[i]: resolve element size through the pointer.
                 * Without this, elem_size defaults to 2, so for smtent (4 B)
                 * in `ptr_smt^[128*domain+index]` the stride is halved and
                 * the store addresses collide — which is why SETMMU's SMT
                 * writes land on random bytes (or get silently dropped when
                 * paired with the WITH field-lookup miss). */
                const char *ptr_name = node->children[0]->children[0]->name;
                cg_symbol_t *ptr_sym = find_symbol_any(cg, ptr_name);
                type_desc_t *pt = (ptr_sym && ptr_sym->type) ? ptr_sym->type : NULL;
                if (!pt && cg->with_depth > 0) {
                    /* P126: WITH-field fallback. Many record fields like
                     * File_Dir / sSeg_Dir / iUnit_Dir / pSeg_list (LCB) or
                     * ptrStr (WITH-accessed ptrs) are accessed as `fld^[i]`
                     * inside a WITH on the containing record. find_symbol_any
                     * misses them; with_lookup_field resolves them. Without
                     * this, array_low/elem_size default to 0/2 and strides
                     * silently corrupt. Mirrors the arr[i] branch below. */
                    type_desc_t *wrt = NULL;
                    int wfld = with_lookup_field(cg, ptr_name, &wrt, NULL);
                    if (wfld >= 0 && wrt)
                        pt = wrt->fields[wfld].type;
                }
                if (pt && pt->kind == TK_POINTER && pt->base_type &&
                    pt->base_type->kind == TK_ARRAY) {
                    type_desc_t *at = pt->base_type;
                    array_low = at->array_low;
                    if (at->element_type)
                        elem_size = type_size(at->element_type);
                } else if (pt && pt->kind == TK_POINTER && pt->base_type &&
                           pt->base_type->kind == TK_STRING) {
                    /* P126: ptr^[i] on a pointer-to-string (e.g. UPSHIFT's
                     * `ptrStr : pathnm_ptr` where pathnm_ptr = ^pathname
                     * and pathname = string). Each element is a char (1 B).
                     * Pascal string[1..N]: string byte 0 holds length, so
                     * logical index 1 maps to offset 1 — array_low=0 here
                     * is intentional (string[i] compiles to base+i directly). */
                    elem_size = 1;
                } else {
                    static int deref_warn = 0;
                    if (deref_warn++ < 40)
                        fprintf(stderr,
                            "  ARRAY_ACCESS(ptr^[i]): '%s' sym=%p pt=%p kind=%d base=%p bkind=%d (elem_size defaults to 2, array_low=0) in %s\n",
                            ptr_name, (void*)ptr_sym, (void*)pt,
                            pt ? pt->kind : -1,
                            pt ? (void*)pt->base_type : NULL,
                            pt && pt->base_type ? pt->base_type->kind : -1,
                            cg->current_file);
                }
            } else if (node->children[0]->type == AST_IDENT_EXPR) {
                cg_symbol_t *arr_sym = find_symbol_any(cg, node->children[0]->name);
                type_desc_t *at = (arr_sym && arr_sym->type) ? arr_sym->type : NULL;
                if (!at && cg->with_depth > 0) {
                    /* WITH-field array access: the base name is a field of
                     * an outer WITH record (e.g. `jt[i]` inside
                     * `WITH jtpointer^^ do`). Resolve the field's type from
                     * the WITH stack. Without this the element size falls
                     * back to 2 bytes, which for record-of-record-array
                     * collapses the stride to first-field-size and
                     * silently scrambles every store target. */
                    type_desc_t *wrt = NULL;
                    int wfld = with_lookup_field(cg, node->children[0]->name, &wrt, NULL);
                    if (wfld >= 0 && wrt)
                        at = wrt->fields[wfld].type;
                }
                if (at) {
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
        /* P82: generic chain-aware lookup — handles nested field accesses
         * like c_mmrb^.head_sdb.freechain.fwd_link where the child is
         * itself AST_FIELD_ACCESS. Falls through to the legacy case-by-case
         * code below only if the generic resolver can't find the field. */
        {
            int off = 0;
            type_desc_t *ft = NULL;
            if (lvalue_field_info(cg, node->children[0], node->name, &off, &ft)) {
                field_off = off;
                goto emit_field_offset;
            }
        }
        if (node->children[0]->type == AST_IDENT_EXPR) {
            cg_symbol_t *rec_sym = find_symbol_any(cg, node->children[0]->name);
            if (rec_sym && rec_sym->type && rec_sym->type->kind == TK_RECORD) {
                type_desc_t *rec_rt = repair_corrupt_record(cg, rec_sym->type);
                for (int fi = 0; fi < rec_rt->num_fields; fi++) {
                    if (str_eq_nocase(rec_rt->fields[fi].name, node->name)) {
                        field_off = rec_rt->fields[fi].offset;
                        break;
                    }
                }
            } else if (rec_sym && rec_sym->type && rec_sym->type->kind == TK_POINTER &&
                       rec_sym->type->base_type && rec_sym->type->base_type->kind == TK_RECORD) {
                type_desc_t *rt = repair_corrupt_record(cg, rec_sym->type->base_type);
                for (int fi = 0; fi < rt->num_fields; fi++) {
                    if (str_eq_nocase(rt->fields[fi].name, node->name)) {
                        field_off = rt->fields[fi].offset;
                        break;
                    }
                }
            } else if (!rec_sym && cg->with_depth > 0) {
                /* WITH-field base: `foo.bar` where `foo` is a field of an outer
                 * WITH record, not a standalone symbol. Find foo's record type
                 * via the WITH stack and look up bar within it. Without this
                 * the field offset stays 0, which silently corrupts the store
                 * target (and, paired with the size miscalculation, causes the
                 * sentinel writes in MM_INIT's head_sdb init to drop bytes). */
                type_desc_t *wrt = NULL;
                int wfld = with_lookup_field(cg, node->children[0]->name, &wrt, NULL);
                if (wfld >= 0 && wrt) {
                    type_desc_t *ft = wrt->fields[wfld].type;
                    if (ft && ft->kind == TK_POINTER && ft->base_type)
                        ft = ft->base_type;
                    if (ft) ft = repair_corrupt_record(cg, ft);
                    if (ft && ft->kind == TK_RECORD) {
                        for (int fi = 0; fi < ft->num_fields; fi++) {
                            if (str_eq_nocase(ft->fields[fi].name, node->name)) {
                                field_off = ft->fields[fi].offset;
                                break;
                            }
                        }
                    }
                }
            }
        } else if (node->children[0]->type == AST_DEREF) {
            /* p^.field — the deref child has the pointer variable */
            ast_node_t *ptr_node = node->children[0]->children[0];
            type_desc_t *rec_type = NULL;
            if (ptr_node && ptr_node->type == AST_IDENT_EXPR) {
                cg_symbol_t *ptr_sym = find_symbol_any(cg, ptr_node->name);
                if (ptr_sym && ptr_sym->type && ptr_sym->type->kind == TK_POINTER &&
                    ptr_sym->type->base_type && ptr_sym->type->base_type->kind == TK_RECORD) {
                    rec_type = repair_corrupt_record(cg, ptr_sym->type->base_type);
                } else if (!ptr_sym && cg->with_depth > 0) {
                    /* WITH field whose type is a pointer to a record */
                    type_desc_t *wrt = NULL;
                    int wfld = with_lookup_field(cg, ptr_node->name, &wrt, NULL);
                    if (wfld >= 0 && wrt) {
                        type_desc_t *ft = wrt->fields[wfld].type;
                        if (ft && ft->kind == TK_POINTER && ft->base_type &&
                            ft->base_type->kind == TK_RECORD)
                            rec_type = ft->base_type;
                    }
                }
            } else if (ptr_node && ptr_node->type == AST_FIELD_ACCESS) {
                /* rec.field^.subfield — resolve rec.field's pointer type. */
                ast_node_t *fa = ptr_node;
                type_desc_t *base = NULL;
                if (fa->children[0] && fa->children[0]->type == AST_IDENT_EXPR) {
                    cg_symbol_t *bs = find_symbol_any(cg, fa->children[0]->name);
                    if (bs && bs->type) {
                        base = bs->type;
                        if (base->kind == TK_POINTER && base->base_type)
                            base = base->base_type;
                    } else if (!bs && cg->with_depth > 0) {
                        type_desc_t *wrt = NULL;
                        int wfld = with_lookup_field(cg, fa->children[0]->name, &wrt, NULL);
                        if (wfld >= 0 && wrt) {
                            base = wrt->fields[wfld].type;
                            if (base && base->kind == TK_POINTER && base->base_type)
                                base = base->base_type;
                        }
                    }
                }
                if (base && base->kind == TK_RECORD) {
                    for (int i = 0; i < base->num_fields; i++) {
                        if (str_eq_nocase(base->fields[i].name, fa->name)) {
                            type_desc_t *ft = base->fields[i].type;
                            if (ft && ft->kind == TK_POINTER && ft->base_type &&
                                ft->base_type->kind == TK_RECORD)
                                rec_type = ft->base_type;
                            break;
                        }
                    }
                }
            }
            if (rec_type) {
                for (int fi = 0; fi < rec_type->num_fields; fi++) {
                    if (str_eq_nocase(rec_type->fields[fi].name, node->name)) {
                        field_off = rec_type->fields[fi].offset;
                        break;
                    }
                }
            }
        }
    emit_field_offset:
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
                /* MOVEQ #imm,D0 — sets all 32 bits (sign-extended) */
                emit16(cg, 0x7000 | ((int8_t)node->int_val & 0xFF));
            } else {
                /* MOVE.L #imm,D0 — always use 32-bit load.
                 * MOVE.W only sets the low 16 bits of D0, leaving the upper
                 * word stale.  When the value participates in a 32-bit
                 * operation (ADD.L, CMP.L, etc.), the stale bits corrupt
                 * the result.  The 2-byte code-size cost is worth correct
                 * semantics everywhere. */
                emit16(cg, 0x203C);
                emit32(cg, (uint32_t)(int32_t)node->int_val);
            }
            break;

        case AST_STRING_LITERAL: {
            /* P128h — in CHAR context (case label, char-var assignment,
             * char comparison), a 1-char string literal is semantically
             * a CHAR value, not a string pointer. Emit MOVEQ so D0 holds
             * the char value (not the address of the length-prefixed
             * string data). See codegen_t.char_literal_context. */
            if (cg->char_literal_context &&
                strlen(node->str_val) == 1) {
                emit16(cg,
                    0x7000 | (uint8_t)node->str_val[0]);  /* MOVEQ #ch,D0 */
                break;
            }
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
                emit_static_link_load(cg, ident_sig);
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
            /* P105: inside a WITH block, a bare identifier that matches
             * a field of the active WITH record must resolve to that
             * field, not to a same-named global — even if the global is
             * a CONST (e.g. an enum ordinal). Mirrors P93's fix in
             * gen_lvalue_addr and expr_size. Without this, sfileio2's
             * `with MDDFdata^ do ... fsversion <> REL1_VERSION` resolves
             * `fsversion` to the LDUTIL enum-ordinal CONST (=0, since
             * `terror = (fsversion, fserror, ...)` in source-LDUTIL.TEXT)
             * instead of the MDDFdb field — boot fails with E_FS_VERSION
             * because the compare is `0 <> 14..17` = always true. */
            if (cg->with_depth > 0) {
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
                    int fbw = wrt->fields[fld].bit_width;
                    if (fbw > 0) {
                        emit_read_a0_to_d0_bit(cg, wrt->fields[fld].bit_offset, fbw);
                    } else {
                        int fsz = type_load_size(wrt->fields[fld].type);
                        emit_read_a0_to_d0(cg, fsz);
                        if (fsz == 1 && type_is_signed_byte(wrt->fields[fld].type))
                            emit_sign_ext_byte(cg);
                    }
                    break;
                }
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
                    if (type_is_signed_byte(sym->type)) emit_sign_ext_byte(cg);
                } else if (sym->is_param || !sym->is_global) {
                    /* Local/param: size-aware load from stack frame.
                     * For outer-scope variables, follow frame chain first. */
                    int depth = find_local_depth(cg, node->name);
                    int sz = type_load_size(sym->type);
                    int byte_offset_adj = 0;
                    /* P84: 1-byte parameters are pushed as word (2 bytes) for
                     * alignment. On 68k big-endian, the byte value lives in
                     * the LOW byte (offset+1), not offset+0. Reading MOVE.B
                     * at offset+0 pulls the zero padding byte, so `kind`
                     * params (Tsdbtype, ord 0..6) silently read as 0. Offset
                     * +1 for byte reads of params; locals (allocated by us
                     * via LINK) start at negative offsets with no padding. */
                    if (sym->is_param && sz == 1 && sym->offset > 0)
                        byte_offset_adj = 1;
                    /* P85a: for byte-sized loads, zero-extend D0 first via
                     * MOVEQ #0,D0 so subsequent MOVE.W D0,Dn doesn't pick
                     * up garbage in the high byte. Same bug class P80h2
                     * fixed for (A0) indirect — repeat it here for the
                     * direct (A6)/(A0) param/local load path. MAP_SEGMENT
                     * was reading `domain` (domainRange 0..3 byte-param)
                     * and pushing it as a word to PROG_MMU with the high
                     * byte leaking stale data, making tar_domain=$B600. */
                    if (sz == 1) emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
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
                    emit16(cg, (uint16_t)(int16_t)(sym->offset + byte_offset_adj));
                    if (sz == 1 && type_is_signed_byte(sym->type)) emit_sign_ext_byte(cg);
                    /* P91: for integer params (TK_INTEGER size=2) at POSITIVE
                     * frame offset (= arg slot), emit EXT.L D0 to sign-extend.
                     * The caller pushes these as MOVE.W (2-byte slot); the
                     * callee's MOVE.W read gets the right 16 bits, but any
                     * subsequent use as a 32-bit value (longint store, pointer
                     * arithmetic) needs sign-extension — OR the post-hoc
                     * widening patches will turn MOVE.W into MOVE.L and read
                     * 2 bytes of stack garbage above the slot. See LD_READSEQ
                     * `count := pages` in SOURCE-CD.TEXT — the original bug
                     * gave count=$0001_00CB (pages=1 in high half + stack
                     * junk in low half) instead of count=1. */
                    if (sym->is_param && sz == 2 && sym->offset > 0 &&
                        sym->type && sym->type->kind == TK_INTEGER) {
                        emit16(cg, 0x48C0); /* EXT.L D0 */
                    }
                } else {
                    /* Global: size-aware load from A5 */
                    int sz = type_load_size(sym->type);
                    /* P85a: zero-extend for byte loads — see comment above. */
                    if (sz == 1) emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
                    if (sz == 4) {
                        emit16(cg, 0x202D);
                    } else if (sz == 1) {
                        emit16(cg, 0x102D);
                    } else {
                        emit16(cg, 0x302D);  /* MOVE.W offset(A5),D0 */
                    }
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                    if (sz == 1 && type_is_signed_byte(sym->type)) emit_sign_ext_byte(cg);
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
                    /* P87d: bit-packed WITH fields (e.g. MouseOn inside
                     * `With PMRec^ do`) need a bit-read, not a byte-read. */
                    int fbw = wrt->fields[fld].bit_width;
                    if (fbw > 0) {
                        emit_read_a0_to_d0_bit(cg, wrt->fields[fld].bit_offset, fbw);
                    } else {
                        int fsz = type_load_size(wrt->fields[fld].type);
                        emit_read_a0_to_d0(cg, fsz);
                        if (fsz == 1 && type_is_signed_byte(wrt->fields[fld].type))
                            emit_sign_ext_byte(cg);
                    }
                } else {
                    /* Not a WITH field — check for built-in constants
                     * before falling through to placeholder */
                    if (str_eq_nocase(node->name, "true")) {
                        emit16(cg, 0x7001);  /* MOVEQ #1,D0 */
                    } else if (str_eq_nocase(node->name, "false")) {
                        emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
                    } else if (str_eq_nocase(node->name, "nil")) {
                        emit16(cg, 0x7000);  /* MOVEQ #0,D0 (nil = 0) */
                    } else {
                        emit16(cg, 0x303C);  /* MOVE.W #0,D0 placeholder */
                        emit16(cg, 0);
                    }
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
                /* Check if operand is boolean (comparison result, function returning
                 * boolean, or boolean variable). Boolean NOT needs logical negation
                 * (1→0, 0→1), not bitwise NOT (1→$FFFE, 0→$FFFF).
                 * Use: TST.W D0; SEQ D0; ANDI.W #1,D0 for boolean operands.
                 * Bitwise NOT for integer/longint operands (Pascal BIT operations). */
                bool is_boolean = false;
                ast_node_t *child = node->children[0];
                if (child->type == AST_FUNC_CALL || child->type == AST_BINARY_OP)
                    is_boolean = true;  /* function results and comparisons are boolean */
                if (child->type == AST_IDENT_EXPR) {
                    cg_symbol_t *sym = find_symbol_any(cg, child->name);
                    if (sym && sym->type && sym->type->kind == TK_BOOLEAN)
                        is_boolean = true;
                    /* P80e: parameterless function calls parsed as identifiers.
                     * If the identifier is a known function (via proc sig),
                     * treat as boolean — Pascal's NOT is boolean for functions.
                     * Fixes: `not SYS_CALLED` where SYS_CALLED is an external
                     * function whose return type was hidden by (* *) comments. */
                    if (!is_boolean && child->name[0]) {
                        cg_proc_sig_t *sig = find_proc_sig(cg, child->name);
                        if (sig) is_boolean = true;
                    }
                    /* P121: WITH-scope bare-ident booleans. `not blockstructured`
                     * inside `with devrec do` (UltraIO) compiled to bitwise NOT.W
                     * because the ident misses both find_symbol_any and
                     * find_proc_sig. Stored TRUE=$0001 from NEW_DEVICE's
                     * SEQ+NEG.B+ANDI.W#1, NOT.W turns it into $FFFE (still
                     * nonzero, still TRUE) — E_IO_MODE_BAD on every disk read.
                     * Narrowly scoped: only when both prior lookups missed
                     * AND the WITH context resolves the name to a TK_BOOLEAN
                     * field. */
                    if (!is_boolean && child->name[0] && cg->with_depth > 0) {
                        type_desc_t *wrt = NULL;
                        int fi = with_lookup_field(cg, child->name, &wrt, NULL);
                        if (fi >= 0 && wrt && fi < wrt->num_fields &&
                            wrt->fields[fi].type &&
                            wrt->fields[fi].type->kind == TK_BOOLEAN) {
                            is_boolean = true;
                        }
                    }
                }
                /* P84: field-access of boolean. Without this, `not sdbstate.memoryF`
                 * emits bitwise `NOT.W D0` which only turns true(=1)→$FFFE (nonzero).
                 * MAKE_MRDATA's `while not sdbstate.memoryF do CHECK_DS(c_sdb)` loop
                 * then never exits even after INIT_SWAPIN sets memoryF:=true, because
                 * the compiled test reads the word, bitwise-inverts, and exits only
                 * on $0000 (i.e. pre-NOT value must be $FFFF). Treat AST_FIELD_ACCESS
                 * as boolean if the resolved field type is TK_BOOLEAN. */
                if (!is_boolean && child->type == AST_FIELD_ACCESS &&
                    child->num_children > 0 && child->name[0]) {
                    type_desc_t *ft = NULL;
                    if (lvalue_field_info(cg, child->children[0], child->name, NULL, &ft)) {
                        if (ft && ft->kind == TK_BOOLEAN) is_boolean = true;
                    }
                }
                if (is_boolean) {
                    emit16(cg, 0x4A40);  /* TST.W D0 */
                    emit16(cg, 0x57C0);  /* SEQ D0 — set if zero (NOT logic) */
                    emit16(cg, 0x0240);  /* ANDI.W #1,D0 */
                    emit16(cg, 0x0001);
                } else {
                    emit16(cg, (sz == 4) ? 0x4680 : 0x4640);  /* NOT.L/W D0 */
                }
            }
            break;

        case AST_BINARY_OP: {
            /* Determine if 32-bit operation needed */
            int opsz = expr_size(cg, node);
            bool use_long = (opsz == 4);

            /* Pascal string equality: `s = 'literal'` or `s1 = s2` where
             * at least one side is a string literal (or a known TK_STRING
             * operand > 4 bytes). Without this the codegen below treats
             * string fields as 2-byte scalars and emits CMP.L vs a pointer
             * to the literal — never equal, so tests like
             * `configinfo[i]^.devname = 'BITBKT'` always return false. */
            if ((node->op == TOK_EQ || node->op == TOK_NE)) {
                ast_node_t *lhs_n = node->children[0];
                ast_node_t *rhs_n = node->children[1];
                /* Either side being a STRING_LITERAL is a strong signal:
                 * Pascal only allows `=`/`<>` against a string literal when
                 * the other side is also a string. expr_size's heuristic
                 * (size > 4) misses many cases where the field type didn't
                 * resolve (e.g. `configinfo[i]^.devname`'s AST path with
                 * array-access-inside-deref), so use "has a string literal"
                 * as the trigger. */
                bool l_is_str = (lhs_n->type == AST_STRING_LITERAL) ||
                                (expr_size(cg, lhs_n) > 4);
                bool r_is_str = (rhs_n->type == AST_STRING_LITERAL) ||
                                (expr_size(cg, rhs_n) > 4);
                if (l_is_str || r_is_str) {
                    /* Load LHS address into A0; RHS address into A1.
                     * For STRING_LITERAL, gen_expression returns a pointer
                     * in D0. For an lvalue (field/var/array), use
                     * gen_lvalue_addr to get an address in A0. */
                    if (lhs_n->type == AST_STRING_LITERAL) {
                        gen_expression(cg, lhs_n);
                        emit16(cg, 0x2040);  /* MOVEA.L D0,A0 */
                    } else {
                        gen_lvalue_addr(cg, lhs_n);
                    }
                    emit16(cg, 0x2F08);  /* MOVE.L A0,-(SP)  save LHS ptr */
                    if (rhs_n->type == AST_STRING_LITERAL) {
                        gen_expression(cg, rhs_n);
                        emit16(cg, 0x2240);  /* MOVEA.L D0,A1 */
                    } else {
                        gen_lvalue_addr(cg, rhs_n);
                        emit16(cg, 0x2248);  /* MOVEA.L A0,A1 */
                    }
                    emit16(cg, 0x205F);  /* MOVEA.L (SP)+,A0  restore LHS */
                    /* Inline byte-compare:
                     *     MOVE.B (A0)+,D2       ; lhs length
                     *     MOVE.B (A1)+,D1       ; rhs length
                     *     CMP.B  D1,D2
                     *     BNE    .ne            ; lengths differ → not equal
                     *     TST.B  D2
                     *     BEQ    .eq            ; both empty
                     * .loop:
                     *     CMP.B  (A0)+,(A1)+
                     *     BNE    .ne
                     *     SUBQ.B #1,D2
                     *     BNE    .loop
                     * .eq: MOVEQ #1,D0 ; BRA .done
                     * .ne: MOVEQ #0,D0
                     * .done:
                     * For TOK_NE, EOR #1,D0 at the end. */
                    /* P98: the original branch displacements here were all
                     * off by 2, landing on the subsequent BRA.S instead of
                     * on MOVEQ #1 / #0. That made `string = 'literal'` return
                     * whatever stale value D0 held before the compare — and
                     * for FIND_EMPTYSLOT's `configinfo[i]^.devname = 'BITBKT'`
                     * that was almost always "truthy", so it returned TRUE
                     * on the first iteration and returned config_index=maxdev
                     * unconditionally, regardless of which slot was free.
                     * Layout (offsets relative to start of sequence):
                     *   0:  MOVE.B (A0)+,D2
                     *   2:  MOVE.B (A1)+,D1
                     *   4:  CMP.B  D1,D2
                     *   6:  BNE.S  .ne      ; disp = 20-(6+2) = $10
                     *   8:  TST.B  D2
                     *  10:  BEQ.S  .eq      ; disp = 16-(10+2) = $08
                     *  12:  CMPM.B (A0)+,(A1)+
                     *  14:  BNE.S  .ne      ; disp = 20-(14+2) = $08
                     *  16:  SUBQ.B #1,D2
                     *  18:  BNE.S  .loop    ; disp = 12-(18+2) = -$08 = $F8
                     *  20:  .eq: MOVEQ #1,D0
                     *  22:  BRA.S .done     ; disp = 26-(22+2) = $02
                     *  24:  .ne: MOVEQ #0,D0
                     *  26:  .done */
                    emit16(cg, 0x1418);  /* MOVE.B (A0)+,D2 */
                    emit16(cg, 0x1219);  /* MOVE.B (A1)+,D1 */
                    emit16(cg, 0xB401);  /* CMP.B D1,D2 */
                    emit16(cg, 0x6610);  /* BNE.S +$10 → .ne */
                    emit16(cg, 0x4A02);  /* TST.B D2 */
                    emit16(cg, 0x6708);  /* BEQ.S +$08 → .eq */
                    /* .loop: */
                    emit16(cg, 0xB308);  /* CMPM.B (A0)+,(A1)+ */
                    emit16(cg, 0x6608);  /* BNE.S +$08 → .ne */
                    emit16(cg, 0x5302);  /* SUBQ.B #1,D2 */
                    emit16(cg, 0x66F8);  /* BNE.S -$08 → .loop */
                    /* .eq: */
                    emit16(cg, 0x7001);  /* MOVEQ #1,D0 */
                    emit16(cg, 0x6002);  /* BRA.S +$02 → .done */
                    /* .ne: */
                    emit16(cg, 0x7000);  /* MOVEQ #0,D0 */
                    /* .done */
                    if (node->op == TOK_NE) {
                        emit16(cg, 0x0A40); emit16(cg, 0x0001);  /* EORI.W #1,D0 */
                    }
                    break;
                }
            }

            gen_expression(cg, node->children[0]);
            /* If using 32-bit ops but left operand is 16-bit, sign-extend.
             * MOVE.W ea,D0 only sets D0[15:0]; the stale upper word corrupts
             * any subsequent 32-bit operation (ADD.L, SUB.L, MULS, etc.).
             * BUT: skip EXT.L for function calls — they already return
             * properly-sized results in D0. EXT.L would zero the upper word
             * of a 32-bit return value like MMU_BASE → $CC0000 → $0000. */
            if (use_long && expr_size(cg, node->children[0]) <= 2 &&
                !rhs_has_wide_operand(cg, node->children[0]))
                emit16(cg, 0x48C0);  /* EXT.L D0 */
            /* Save left result for later.
             * If the right operand is complex (binary op, function call,
             * unary op), it will use D2 internally and clobber our saved
             * value. In that case, save to the stack instead. */
            {
                ast_node_t *rhs = node->children[1];
                bool rhs_complex = (rhs->type == AST_BINARY_OP ||
                                    rhs->type == AST_UNARY_OP ||
                                    rhs->type == AST_FUNC_CALL);
                if (rhs_complex) {
                    emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) — save left on stack */
                } else {
                    emit16(cg, use_long ? 0x2400 : 0x3400);  /* MOVE.L/W D0,D2 */
                }
                gen_expression(cg, rhs);
                /* Same for right operand */
                if (use_long && expr_size(cg, rhs) <= 2 &&
                    !rhs_has_wide_operand(cg, rhs))
                    emit16(cg, 0x48C0);  /* EXT.L D0 */
                /* D0 = right, restore left from D2 or stack */
                emit16(cg, use_long ? 0x2200 : 0x3200);  /* MOVE.L/W D0,D1 (right → D1) */
                if (rhs_complex) {
                    emit16(cg, 0x201F);  /* MOVE.L (SP)+,D0 — restore left from stack */
                } else {
                    emit16(cg, use_long ? 0x2002 : 0x3002);  /* MOVE.L/W D2,D0 */
                }
            }

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

            /* ORD(x): convert to integer — identity operation.
             * ORD4(x): convert int2 → longint — sign-extend if arg is 16-bit.
             * Without EXT.L, MOVE.W from a local/field leaves D0[31:16] stale,
             * and any subsequent 32-bit operation (multiply, add) is corrupted. */
            if (str_eq_nocase(fn, "ORD") || str_eq_nocase(fn, "ORD4")) {
                if (node->num_children > 0) {
                    gen_expression(cg, node->children[0]);
                    if (str_eq_nocase(fn, "ORD4")) {
                        int arg_sz = expr_size(cg, node->children[0]);
                        if (arg_sz <= 2)
                            emit16(cg, 0x48C0);  /* EXT.L D0 */
                    }
                }
                break;
            }

            /* POINTER(x): cast integer to pointer — ensure 32-bit value.
             * The argument might have been loaded as 16-bit (MOVE.W) if its
             * type was unresolvable. Since the result is a pointer (32-bit),
             * use gen_ptr_expression to retroactively patch 16→32-bit loads. */
            if (str_eq_nocase(fn, "POINTER")) {
                if (node->num_children > 0) gen_ptr_expression(cg, node->children[0]);
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
                            /* P127-NEXT: also check WITH-scoped fields.
                             * `Sizeof(initSegMap)` inside `with c_syslocal_ptr^ do`
                             * references a record field — not a type and not a
                             * standalone symbol. Without this, sz defaulted to 2,
                             * causing MM_Setup's initSegMap-clearing loop
                             * (`long_ptr := @initSegMap + Sizeof(initSegMap)`,
                             * decrement by 4, until == @initSegMap) to start at
                             * +2 instead of +16 and never hit the target, spinning
                             * forever inside SYS_PROC_INIT's Build_Syslocal. */
                            else if (cg->with_depth > 0) {
                                type_desc_t *wrt = NULL;
                                int fld = with_lookup_field(cg, arg->name, &wrt, NULL);
                                if (fld >= 0 && wrt && wrt->fields[fld].type &&
                                    wrt->fields[fld].type->size > 0) {
                                    sz = wrt->fields[fld].type->size;
                                }
                            }
                        }
                    }
                }
                /* Use MOVEQ for 0..127 (sets full 32-bit D0), MOVE.L otherwise.
                 * MOVE.W leaves the upper word of D0 stale — causes
                 * corruption when the result is stored to an int4 field. */
                if (sz >= 0 && sz <= 127) {
                    emit16(cg, 0x7000 | (sz & 0xFF));  /* MOVEQ #sz,D0 */
                } else {
                    emit16(cg, 0x203C);  /* MOVE.L #sz,D0 */
                    emit32(cg, (uint32_t)sz);
                }
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

            /* LENGTH(s): first byte of string = length.
             * If `s` is a direct string variable/field (TK_STRING value,
             * size > 4), use its ADDRESS — gen_ptr_expression would
             * load the string's first 4 content bytes and deref them as
             * if they were a pointer, corrupting the length read. If
             * `s` is a pointer-to-string (4 bytes), load the pointer. */
            if (str_eq_nocase(fn, "LENGTH")) {
                if (node->num_children > 0) {
                    ast_node_t *arg = node->children[0];
                    int asz = expr_size(cg, arg);
                    bool need_addr = false;
                    /* Direct string: expr_size returns the string's byte
                     * size (>4). Also explicitly check the identifier's
                     * type so we catch edge cases where expr_size falls
                     * back to 2. */
                    if (asz > 4) need_addr = true;
                    if (!need_addr && arg->type == AST_IDENT_EXPR) {
                        type_desc_t *rt = NULL;
                        if (cg->with_depth > 0) {
                            (void)with_lookup_field(cg, arg->name, &rt, NULL);
                            if (rt && rt->kind == TK_STRING) need_addr = true;
                        }
                        if (!need_addr) {
                            cg_symbol_t *sym = find_symbol_any(cg, arg->name);
                            if (sym && sym->type && sym->type->kind == TK_STRING)
                                need_addr = true;
                        }
                    }
                    if (need_addr) {
                        gen_lvalue_addr(cg, arg);       /* A0 = &string */
                    } else {
                        gen_ptr_expression(cg, arg);    /* D0 = ptr */
                        emit16(cg, 0x2040);             /* MOVEA.L D0,A0 */
                    }
                }
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

            /* EXIT: return from named procedure.
             *
             * Targeted fix: Pascal's exit(P) means "unwind to P's frame
             * and return". Our old codegen walked scope_depth-1 hops
             * unconditionally — lexically wrong for exit(CurrentProc)
             * (would unwind the caller's frame too), but happened to
             * match what many exit(EnclosingProc) sites expect.
             *
             * Fix only the exit(CurrentProc) case: no walk, just UNLK+RTS.
             * For exit(Named) where the target doesn't match the current
             * proc, keep the old walk-to-outermost behavior — changing it
             * cascades regressions in callers that rely on walking past
             * intended targets (still not lexically correct, but works
             * in current boot paths).
             *
             * The exit(CurrentProc)-only fix unblocks INIT_BOOT_CDS's
             * EXIT(init_boot_cds) which was mis-unwinding its caller
             * BOOT_IO_INIT, skipping BOOT_IO_INIT's case-4 → FS_INIT. */
            if (str_eq_nocase(fn, "EXIT")) {
                const char *target = NULL;
                if (node->num_children > 0 &&
                    node->children[0]->type == AST_IDENT_EXPR &&
                    node->children[0]->name[0]) {
                    target = node->children[0]->name;
                }
                cg_scope_t *cur = current_scope(cg);
                bool target_is_current = (target && cur &&
                                          str_eq_nocase(cur->proc_name, target));
                if (!target_is_current && cg->scope_depth > 1) {
                    for (int d = cg->scope_depth - 1; d >= 1; d--)
                        emit16(cg, 0x2C56);  /* MOVEA.L (A6),A6 */
                }
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
            /* P79: prefer the sig's is_external over the symbol's. The sig
             * was already resolved by find_proc_sig to prefer the non-external
             * body sig (P20 fix). But find_global may return the EXTERNAL
             * declaration symbol from an $I'd interface (e.g., BLD_SEG declared
             * external in MMPRIM but body defined in MM1). If the sig says
             * non-external, trust it — the push direction must match the body,
             * not the forward declaration. */
            bool is_callee_clean = sig ? sig->is_external :
                                   (callee_sym && callee_sym->is_external);
            {

            /* Push order: left-to-right for callee-clean (assembly),
             * right-to-left for caller-clean (Pascal). */
            if (is_callee_clean) {
                for (int i = 0; i < node->num_children; i++) {
                    bool is_var_arg = (sig && i < sig->num_params && sig->param_is_var[i]);
                    bool by_ref = ARG_BY_REF(cg, sig, i);
                    int psize = (sig && i < sig->num_params) ? sig->param_size[i] : 4;
                    /* P128c: when the callee is asm (callee-clean) and the
                     * actual param is 1-byte (byte/char/enum/bool), push via
                     * MOVE.B D0,-(SP) so the value lands in the HIGH byte of
                     * the 2-byte slot. Apple's asm uses `MOVE.B (SP)+,Dn`
                     * which reads the HIGH byte. Our Pascal→Pascal convention
                     * puts the byte in the LOW byte (MOVE.W D0,-(SP)) which
                     * asm reads as $00. QUEUE_PR(Blocked) silently took the
                     * Ready path pre-P128c because of this mismatch. */
                    int pkind = (sig && i < sig->num_params) ? sig->param_type_kind[i] : 0;
                    bool is_byte_param = !is_var_arg && !by_ref &&
                        sig && i < sig->num_params && sig->param_type[i] &&
                        type_size(sig->param_type[i]) == 1 &&
                        (pkind == TK_CHAR || pkind == TK_BOOLEAN ||
                         pkind == TK_ENUM || pkind == TK_BYTE ||
                         pkind == TK_SUBRANGE);
                    if (is_var_arg || by_ref) {
                        gen_lvalue_addr(cg, node->children[i]);
                        emit16(cg, 0x2F08);  /* MOVE.L A0,-(SP) */
                    } else if (is_byte_param) {
                        gen_expression(cg, node->children[i]);
                        emit16(cg, 0x1F00);  /* MOVE.B D0,-(SP) — high byte of word slot */
                    } else if (psize <= 2) {
                        gen_expression(cg, node->children[i]);
                        emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */
                    } else {
                        gen_expression(cg, node->children[i]);
                        /* P108: widen narrow-typed expressions (e.g. a
                         * 2-byte integer field) before pushing as a 4-byte
                         * longint arg. Without EXT.L the high word of D0
                         * holds whatever was there before the MOVE.W load
                         * (often the base pointer used to reach the field),
                         * and the callee reads garbage. Skip for expressions
                         * that naturally produce a full 32-bit result. */
                        if (expr_size(cg, node->children[i]) <= 2 &&
                            !rhs_has_wide_operand(cg, node->children[i]))
                            emit16(cg, 0x48C0);  /* EXT.L D0 */
                        emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
                    }
                }
            } else {
                for (int i = node->num_children - 1; i >= 0; i--) {
                    bool is_var_arg = (sig && i < sig->num_params && sig->param_is_var[i]);
                    bool by_ref = ARG_BY_REF(cg, sig, i);
                    int psize = (sig && i < sig->num_params) ? sig->param_size[i] : 4;
                    if (is_var_arg || by_ref) {
                        gen_lvalue_addr(cg, node->children[i]);
                        emit16(cg, 0x2F08);  /* MOVE.L A0,-(SP) */
                    } else if (psize <= 2) {
                        gen_expression(cg, node->children[i]);
                        emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */
                    } else {
                        gen_expression(cg, node->children[i]);
                        /* P108: see callee-clean branch above. */
                        if (expr_size(cg, node->children[i]) <= 2 &&
                            !rhs_has_wide_operand(cg, node->children[i]))
                            emit16(cg, 0x48C0);  /* EXT.L D0 */
                        emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
                    }
                }
            }
            }
            emit_static_link_load(cg, sig);
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

        case AST_FIELD_ACCESS: {
            /* P87d: check for a bit-packed field. gen_lvalue_addr already
             * emits ADDA.W to reach the containing byte, but we need to
             * issue a bit-read rather than a byte-read afterwards. */
            int bit_off = 0, bit_w = 0;
            type_desc_t *fld_type = NULL;
            bool have_field = (node->num_children > 0 &&
                lvalue_field_info_full(cg, node->children[0], node->name,
                                       NULL, &fld_type, &bit_off, &bit_w));
            if (have_field && bit_w > 0) {
                gen_lvalue_addr(cg, node);
                emit_read_a0_to_d0_bit(cg, bit_off, bit_w);
            } else {
                gen_lvalue_addr(cg, node);
                int fsz = expr_size(cg, node);
                emit_read_a0_to_d0(cg, fsz);
                if (fsz == 1 && have_field && type_is_signed_byte(fld_type))
                    emit_sign_ext_byte(cg);
            }
            break;
        }

        case AST_DEREF:
            gen_ptr_expression(cg, node->children[0]);
            emit16(cg, 0x2040);  /* MOVEA.L D0,A0 */
            emit_read_a0_to_d0(cg, expr_size(cg, node));
            break;

        case AST_ADDR_OF: {
            /* P80h2: @proc — procedure identifiers need the CODE address, not
             * an A5-relative global slot. Without this, ord(@MemMgr) resolves
             * via find_symbol_any to whatever stale global happens to share
             * the name and emits LEA offset(A5),A0 — yielding e.g. $CCB802
             * instead of the entry-point $043F56. Detect proc-sig first and
             * emit MOVE.L #procaddr,D0 with a linker relocation, mirroring
             * the bare-ident branch at AST_IDENT_EXPR above. */
            ast_node_t *child = node->children[0];
            if (child && child->type == AST_IDENT_EXPR && child->name[0]) {
                cg_proc_sig_t *psig = find_proc_sig(cg, child->name);
                if (psig) {
                    emit16(cg, 0x203C);  /* MOVE.L #imm32,D0 */
                    emit32(cg, 0);       /* relocated to proc address */
                    if (cg->num_relocs < CODEGEN_MAX_RELOCS) {
                        cg_reloc_t *r = &cg->relocs[cg->num_relocs++];
                        r->offset = cg->code_size - 4;
                        strncpy(r->symbol, child->name, sizeof(r->symbol) - 1);
                        r->size = 4;
                        r->pc_relative = false;
                    }
                    break;
                }
            }
            gen_lvalue_addr(cg, node->children[0]);
            emit16(cg, 0x2008);  /* MOVE.L A0,D0 */
            break;
        }

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
            /* P90: aggregate assignment (record/array). LHS and RHS are both
             * structured and exceed 4 bytes — emit a byte-copy loop instead
             * of the scalar-assignment path (which would truncate to a
             * single MOVE.W). Without this, `mypmem := pmptr^` in FIND_PM_IDS
             * copies only 2 bytes of the 64-byte pmemrec, and
             * `booter[i] := booter[devcd]` in INIT_BOOT_CDS leaves for_pos[2]
             * and for_pos[3] uninitialized — so the match loop compares
             * against garbage and foundall stays false. */
            {
                ast_node_t *lhs_a = node->children[0];
                ast_node_t *rhs_a = node->children[1];
                int lhs_type_sz = expr_size(cg, lhs_a);
                int rhs_type_sz = expr_size(cg, rhs_a);
                /* P128i: use destination size (LHS) as the copy count. Apple
                 * Pascal string assignment with different declared max lengths
                 * (e.g. `name: e_name (33 B) := volPath: pathname (256 B)`)
                 * must truncate the source to fit the destination — NOT
                 * copy the larger source size past the destination bounds.
                 * The old MAX rule smashed the stack frame header (saved-A6
                 * and return-PC slots) of the enclosing procedure whenever
                 * a larger string was assigned to a smaller one (ReadDir's
                 * `name := volPath` corrupted its own return address).
                 * Pure MIN broke unrelated assignments where expr_size
                 * under-reports RHS size due to type-resolution misses, so
                 * stick with LHS: always safe for the destination and
                 * preserves record/same-size-array semantics (where LHS ==
                 * RHS size). When LHS > RHS, we read some trailing bytes
                 * past the source's declared end but stay inside the dest —
                 * not crashy (cf. MAX's stack-smash failure mode). */
                int agg_sz = lhs_type_sz;
                if (agg_sz < rhs_type_sz && rhs_type_sz > 4) {
                    /* Keep the old larger size IF LHS looks under-resolved
                     * (<=4 bytes, i.e. default fallback) AND RHS is a clear
                     * structured type. This handles codegen corner cases
                     * where expr_size(LHS) falls through to the default 2. */
                    if (lhs_type_sz <= 4) agg_sz = rhs_type_sz;
                }
                if (agg_sz > 4) {
                    /* Compute LHS address on stack. */
                    gen_lvalue_addr(cg, lhs_a);
                    emit16(cg, 0x2F08);  /* MOVE.L A0,-(SP) */
                    /* RHS address into A0 — AST_DEREF uses the evaluated
                     * pointer, AST_STRING_LITERAL and any address-returning
                     * expression run as a value expression and move D0→A0,
                     * otherwise use lvalue_addr.
                     * P99: without the STRING_LITERAL branch, gen_lvalue_addr
                     * did nothing (no case for literals) and A0 kept its LHS
                     * value, so aggregate assignments like
                     * `devname := 'BITBKT'` silently copied the LHS onto
                     * itself. That left the BITBKT devrec's devname empty —
                     * FIND_EMPTYSLOT then couldn't find any BITBKT slot and
                     * MAKE_BUILTIN fired 10758 (cdtoomany). */
                    if (rhs_a->type == AST_DEREF && rhs_a->num_children > 0) {
                        gen_ptr_expression(cg, rhs_a->children[0]);
                        emit16(cg, 0x2040);  /* MOVEA.L D0,A0 */
                    } else if (rhs_a->type == AST_STRING_LITERAL) {
                        gen_expression(cg, rhs_a);
                        emit16(cg, 0x2040);  /* MOVEA.L D0,A0 */
                    } else {
                        gen_lvalue_addr(cg, rhs_a);
                    }
                    emit16(cg, 0x225F);  /* MOVEA.L (SP)+,A1  (LHS addr) */
                    /* MOVE.W #(agg_sz - 1),D0 */
                    emit16(cg, 0x303C);
                    emit16(cg, (uint16_t)(agg_sz - 1));
                    uint32_t loop_pc = cg->code_size;
                    emit16(cg, 0x12D8);  /* MOVE.B (A0)+,(A1)+ */
                    emit16(cg, 0x51C8);  /* DBRA D0,<offset> */
                    /* DBRA displacement is relative to the displacement
                     * word's address (= cg->code_size immediately after the
                     * opcode emit). target = disp_addr + displacement. */
                    int32_t rel = (int32_t)loop_pc - (int32_t)cg->code_size;
                    emit16(cg, (uint16_t)(int16_t)rel);
                    break;
                }
            }
            /* Evaluate RHS into D0. P128h: if LHS resolves to a char-sized
             * target (1 byte), enable char_literal_context so a 1-char
             * string-literal RHS compiles to MOVEQ #ch,D0 (value) instead
             * of a string-pointer. Fixes `delimiter := ' '` (Gobble) and
             * every similar char-var assignment. */
            {
                int lhs_byte_sz = expr_size(cg, node->children[0]);
                if (lhs_byte_sz == 1) cg->char_literal_context = true;
            }
            int rhs_sz = expr_size(cg, node->children[1]);
            gen_expression(cg, node->children[1]);
            cg->char_literal_context = false;
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
            /* P3 post-hoc: if the LHS is pointer-sized (4 bytes) but the RHS
             * emitted MOVE.W (A0),D0 or MOVE.W disp(An),D0, patch it to
             * MOVE.L. This handles ptr := ptr^.field where the field type
             * couldn't be resolved through the type chain. Note: this does
             * NOT apply to TK_INTEGER params at positive frame offsets —
             * those emit MOVE.W + EXT.L at the load site (see P91 in the
             * AST_IDENT_EXPR load path) so they already present a 32-bit
             * D0 here and the MOVE.L-detector at the earlier post-hoc
             * bumps rhs_sz to 4. */
            {
                ast_node_t *lhs = node->children[0];
                int lhs_sz = expr_size(cg, lhs);
                /* Also check direct type_load_size for LHS ident */
                if (lhs_sz < 4 && lhs->type == AST_IDENT_EXPR) {
                    cg_symbol_t *ls = find_symbol_any(cg, lhs->name);
                    if (ls && ls->type) {
                        int tls = type_load_size(ls->type);
                        if (tls > lhs_sz) lhs_sz = tls;
                    }
                }
                if (lhs_sz >= 4 && rhs_sz < 4 && cg->code_size >= 2) {
                    uint16_t last_op = (cg->code[cg->code_size - 2] << 8) | cg->code[cg->code_size - 1];
                    if (last_op == 0x3010 ||  /* MOVE.W (A0),D0 */
                        last_op == 0x3018) {  /* MOVE.W (A0)+,D0 */
                        cg->code[cg->code_size - 2] = 0x20;  /* → MOVE.L */
                        rhs_sz = 4;
                    }
                }
                if (lhs_sz >= 4 && rhs_sz < 4 && cg->code_size >= 4) {
                    uint16_t prev_op = (cg->code[cg->code_size - 4] << 8) | cg->code[cg->code_size - 3];
                    if (prev_op == 0x3028 || /* MOVE.W disp(A0),D0 */
                        prev_op == 0x302E || /* MOVE.W disp(A6),D0 */
                        prev_op == 0x302D) { /* MOVE.W disp(A5),D0 */
                        cg->code[cg->code_size - 4] = 0x20;  /* → MOVE.L */
                        rhs_sz = 4;
                    }
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
                    /* P87d: bit-packed WITH field write. Emit read-modify-
                     * write so we don't clobber neighboring bits (e.g.
                     * assigning NormCont must leave BootVol intact). */
                    int fbw = wrt->fields[fld].bit_width;
                    if (fbw > 0) {
                        emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */
                        gen_with_base(cg, widx);
                        int foff = wrt->fields[fld].offset;
                        if (foff != 0) {
                            emit16(cg, 0xD0FC);
                            emit16(cg, (uint16_t)(int16_t)foff);
                        }
                        emit_write_d0_to_a0_bit(cg, wrt->fields[fld].bit_offset, fbw);
                        break;
                    }
                    /* WITH field assignment: save RHS, load base, add offset, store */
                    int fsz = type_load_size(wrt->fields[fld].type);
                    if (fsz > 4) fsz = 4;
                    /* Only widen store for pointer/longint fields — widening
                     * int2 fields with MOVE.L overwrites adjacent record fields. */
                    if (rhs_sz > fsz && wrt->fields[fld].type &&
                        (wrt->fields[fld].type->kind == TK_POINTER ||
                         (wrt->fields[fld].type->kind == TK_LONGINT && fsz < 4)))
                        fsz = rhs_sz;
                    /* Widen narrow RHS to match a longint/pointer field so the
                     * upper 16 bits of D0 aren't stale scratch. Mirrors ORD4. */
                    if (fsz == 4 && rhs_sz < 4 && wrt->fields[fld].type &&
                        (wrt->fields[fld].type->kind == TK_LONGINT ||
                         wrt->fields[fld].type->kind == TK_POINTER) &&
                        !rhs_has_wide_operand(cg, node->children[1])) {
                        emit16(cg, 0x48C0);  /* EXT.L D0 */
                    }
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
                    /* Only widen for pointer/longint — same guard as global store */
                    if (rhs_sz > sz && sym->type &&
                        (sym->type->kind == TK_POINTER ||
                         (sym->type->kind == TK_LONGINT && sz < 4)))
                        sz = rhs_sz;
                    /* Narrow RHS → wide longint/pointer LHS: sign-extend to
                     * prevent stale upper word. P77: skip if RHS already 32-bit. */
                    if (sz == 4 && rhs_sz < 4 && sym->type &&
                        (sym->type->kind == TK_LONGINT ||
                         sym->type->kind == TK_POINTER) &&
                        !rhs_has_wide_operand(cg, node->children[1])) {
                        emit16(cg, 0x48C0);  /* EXT.L D0 */
                    }
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
                    /* Only widen for pointer-typed LHS */
                    if (rhs_sz > sz && sym->type &&
                        (sym->type->kind == TK_POINTER ||
                         (sym->type->kind == TK_LONGINT && sz < 4)))
                        sz = rhs_sz;
                    /* Narrow RHS → wide longint/pointer LHS: sign-extend.
                     * P77: skip if RHS already 32-bit. */
                    if (sz == 4 && rhs_sz < 4 && sym->type &&
                        (sym->type->kind == TK_LONGINT ||
                         sym->type->kind == TK_POINTER) &&
                        !rhs_has_wide_operand(cg, node->children[1])) {
                        emit16(cg, 0x48C0);  /* EXT.L D0 */
                    }
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
                    /* Only widen store for pointer-typed LHS to prevent pointer
                     * truncation. Non-pointer variables (int2, boolean, etc.)
                     * must use their declared size — widening overwrites adjacent
                     * globals in the shared A5 data area. */
                    if (rhs_sz > sz && sym->type &&
                        (sym->type->kind == TK_POINTER ||
                         (sym->type->kind == TK_LONGINT && sz < 4)))
                        sz = rhs_sz;
                    /* Narrow RHS → wide longint/pointer global: sign-extend.
                     * P77: skip if RHS already 32-bit. */
                    if (sz == 4 && rhs_sz < 4 && sym->type &&
                        (sym->type->kind == TK_LONGINT ||
                         sym->type->kind == TK_POINTER) &&
                        !rhs_has_wide_operand(cg, node->children[1])) {
                        emit16(cg, 0x48C0);  /* EXT.L D0 */
                    }
                    if (sz == 4) emit16(cg, 0x2B40);
                    else if (sz == 1) emit16(cg, 0x1B40);
                    else emit16(cg, 0x3B40);  /* MOVE.W D0,offset(A5) */
                    emit16(cg, (uint16_t)(int16_t)sym->offset);
                }
            } else if (lhs->type == AST_ARRAY_ACCESS || lhs->type == AST_FIELD_ACCESS || lhs->type == AST_DEREF) {
                /* P87d: bit-packed FIELD_ACCESS write. Emit a read-modify-
                 * write that leaves neighbor bits intact. */
                if (lhs->type == AST_FIELD_ACCESS && lhs->num_children > 0) {
                    int bit_off = 0, bit_w = 0;
                    if (lvalue_field_info_full(cg, lhs->children[0], lhs->name,
                                               NULL, NULL, &bit_off, &bit_w) &&
                        bit_w > 0) {
                        emit16(cg, 0x3F00);          /* MOVE.W D0,-(SP) */
                        gen_lvalue_addr(cg, lhs);
                        emit_write_d0_to_a0_bit(cg, bit_off, bit_w);
                        break;
                    }
                }
                /* Complex LHS: size-aware save/restore/store.
                 * expr_size already returns 4 for pointer/longint via type_load_size,
                 * so no unconditional widening — that overwrites adjacent fields. */
                int sz = expr_size(cg, lhs);
                if (sz > 4) sz = 4;
                /* Narrow RHS → wide longint/pointer target via field/array:
                 * the element type drives sz, so when sz=4 but rhs_sz<4 and
                 * the load was MOVE.W, the upper word of D0 is stale. EXT.L
                 * sign-extends to a proper 32-bit value. */
                /* P54: skip EXT.L if RHS computes from a function whose
                 * return type might be 4 bytes. expr_size's default of 2
                 * for unresolved-type FUNC_CALLs mis-classifies pointer
                 * returns; EXT.L would then zero the upper word of a
                 * legitimate 32-bit pointer result (e.g. `MMU_Base(x) +
                 * Sizeof(y)` where MMU_Base returns absptr). Conservatively
                 * preserve the full longword when we see a func call in
                 * the RHS subtree. */
                bool rhs_has_funcall = false;
                {
                    ast_node_t *rhs_root = node->children[1];
                    ast_node_t *stk[16]; int stki = 0;
                    if (rhs_root) stk[stki++] = rhs_root;
                    while (stki && !rhs_has_funcall) {
                        ast_node_t *nd = stk[--stki];
                        if (nd->type == AST_FUNC_CALL) { rhs_has_funcall = true; break; }
                        for (int i = 0; i < nd->num_children && stki < 16; i++)
                            if (nd->children[i]) stk[stki++] = nd->children[i];
                    }
                }
                if (sz == 4 && rhs_sz < 4 && !rhs_has_funcall &&
                    !rhs_has_wide_operand(cg, node->children[1])) {
                    emit16(cg, 0x48C0);  /* EXT.L D0 */
                }
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
                /* P79: same fix as PROC_CALL — prefer sig's is_external */
                bool call_callee_clean = call_sig ? call_sig->is_external :
                                         (call_sym && call_sym->is_external);

                /* Push arguments with correct sizes.
                 * Non-primitive value params (RECORD/STRING/ARRAY) are passed
                 * by reference per P16 — push @arg, not the value. */
                if (call_callee_clean) {
                    for (int a = 0; a < node->num_children; a++) {
                        bool is_var_arg = (call_sig && a < call_sig->num_params && call_sig->param_is_var[a]);
                        bool by_ref = ARG_BY_REF(cg, call_sig, a);
                        int psize = (call_sig && a < call_sig->num_params) ? call_sig->param_size[a] : 4;
                        /* P128c: see PROC_CALL site. Byte-sized args to asm
                         * procs must land in the HIGH byte of the 2-byte slot. */
                        int pkind = (call_sig && a < call_sig->num_params) ? call_sig->param_type_kind[a] : 0;
                        bool is_byte_param = !is_var_arg && !by_ref &&
                            call_sig && a < call_sig->num_params && call_sig->param_type[a] &&
                            type_size(call_sig->param_type[a]) == 1 &&
                            (pkind == TK_CHAR || pkind == TK_BOOLEAN ||
                             pkind == TK_ENUM || pkind == TK_BYTE ||
                             pkind == TK_SUBRANGE);
                        if (is_var_arg || by_ref) {
                            gen_lvalue_addr(cg, node->children[a]);
                            emit16(cg, 0x2F08);
                        } else if (is_byte_param) {
                            gen_expression(cg, node->children[a]);
                            emit16(cg, 0x1F00);  /* MOVE.B D0,-(SP) */
                        } else if (psize <= 2) {
                            gen_expression(cg, node->children[a]);
                            emit16(cg, 0x3F00);
                        } else {
                            gen_expression(cg, node->children[a]);
                            emit16(cg, 0x2F00);
                        }
                    }
                } else {
                    for (int a = node->num_children - 1; a >= 0; a--) {
                        bool is_var_arg = (call_sig && a < call_sig->num_params && call_sig->param_is_var[a]);
                        bool by_ref = ARG_BY_REF(cg, call_sig, a);
                        int psize = (call_sig && a < call_sig->num_params) ? call_sig->param_size[a] : 4;
                        if (is_var_arg || by_ref) {
                            gen_lvalue_addr(cg, node->children[a]);
                            emit16(cg, 0x2F08);
                        } else if (psize <= 2) {
                            gen_expression(cg, node->children[a]);
                            emit16(cg, 0x3F00);
                        } else {
                            gen_expression(cg, node->children[a]);
                            emit16(cg, 0x2F00);
                        }
                    }
                }
                emit_static_link_load(cg, call_sig);
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

        case AST_LABEL_DECL: {
            /* Record label's code offset, patch any pending forward GOTOs,
             * then emit the labeled statement. */
            int lbl = (int)node->int_val;
            if (cg->num_labels < 128) {
                cg->labels[cg->num_labels].label = lbl;
                cg->labels[cg->num_labels].code_offset = cg->code_size;
                cg->num_labels++;
            }
            /* Patch any pending forward BRA.W's targeting this label.
             * BRA.W displacement = (target - branch_instr_addr - 2), where
             * the 16-bit offset slot lives at patch_offset (2 bytes after
             * the opcode). So displacement is (target - (patch_offset - 2) - 2)
             * = target - patch_offset. */
            for (int g = 0; g < cg->num_pending_gotos; g++) {
                if (cg->pending_gotos[g].label == lbl) {
                    uint32_t patch = cg->pending_gotos[g].patch_offset;
                    int32_t disp = (int32_t)cg->code_size - (int32_t)patch;
                    if (disp > 32767 || disp < -32768) {
                        /* Out of BRA.W range — not expected for normal Pascal
                         * procedures; emit an error and proceed. */
                        fprintf(stderr, "GOTO: label %d out of BRA.W range (disp=%d) in %s\n",
                                lbl, disp, cg->current_file);
                    }
                    cg->code[patch]     = (disp >> 8) & 0xFF;
                    cg->code[patch + 1] = disp & 0xFF;
                    cg->pending_gotos[g] = cg->pending_gotos[--cg->num_pending_gotos];
                    g--;
                }
            }
            if (node->num_children > 0)
                gen_statement(cg, node->children[0]);
            break;
        }

        case AST_GOTO: {
            int lbl = (int)node->int_val;
            uint32_t target = 0xFFFFFFFF;
            for (int i = 0; i < cg->num_labels; i++) {
                if (cg->labels[i].label == lbl) { target = cg->labels[i].code_offset; break; }
            }
            /* P80e: non-local goto — restore A6 by following the static link
             * chain. A nested procedure's A6 frame contains the saved A6 of
             * the enclosing scope at [A6]. For gotos from nested procs to
             * enclosing labels (e.g., RECOVER's goto 10 → MAKE_DATASEG),
             * we must unwind A6 so the target code accesses the right locals.
             * Also restore SP from the unwound A6. */
            if (cg->scope_depth > 1) {
                /* P80e: Non-local goto from nested procedure. Restore A6 to
                 * the enclosing scope's frame so the label's code accesses
                 * the right locals. Follow the static link chain (saved A6
                 * at [A6]) for each nesting level.
                 * DON'T adjust SP — the label code manages its own stack.
                 * For non-local exits like RECOVER's goto, the stack is
                 * deeper than the target scope expects, but A6-relative
                 * access will work correctly. */
                for (int d = cg->scope_depth - 1; d >= 1; d--) {
                    emit16(cg, 0x2C56);  /* MOVEA.L (A6),A6 — follow static link */
                }
            }
            emit16(cg, 0x6000);  /* BRA.W */
            uint32_t patch = cg->code_size;
            emit16(cg, 0);       /* 16-bit displacement placeholder */
            if (target != 0xFFFFFFFF) {
                int32_t disp = (int32_t)target - (int32_t)patch;
                if (disp > 32767 || disp < -32768) {
                    fprintf(stderr, "GOTO: label %d back-branch out of range (disp=%d) in %s\n",
                            lbl, disp, cg->current_file);
                }
                cg->code[patch]     = (disp >> 8) & 0xFF;
                cg->code[patch + 1] = disp & 0xFF;
            } else {
                /* Forward reference — queue for patching */
                if (cg->num_pending_gotos < 128) {
                    cg->pending_gotos[cg->num_pending_gotos].label = lbl;
                    cg->pending_gotos[cg->num_pending_gotos].patch_offset = patch;
                    cg->num_pending_gotos++;
                }
            }
            break;
        }

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

            /* P75: evaluate end value and PUSH onto stack (not D3).
             * Previously we kept end value in D3 across the body, but
             * any proc call inside the body (or case statement) can
             * clobber D3, causing the next iteration's CMP to use
             * garbage and the loop to exit early. Push to stack; reload
             * into D3 before each CMP; pop at loop exit. */
            int vsz = type_load_size(var ? var->type : NULL);
            gen_expression(cg, node->children[1]);
            if (vsz == 4)
                emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
            else
                emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */

            /* Loop start */
            uint32_t loop_start = cg->code_size;

            /* Reload end value into D3 from stack top. */
            if (vsz == 4) emit16(cg, 0x262F);  /* MOVE.L 0(SP),D3 */
            else          emit16(cg, 0x362F);  /* MOVE.W 0(SP),D3 */
            emit16(cg, 0);

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
            /* Pop the end value we pushed at loop entry. */
            if (vsz == 2)      emit16(cg, 0x544F);  /* ADDQ.W #2,A7 */
            else if (vsz == 4) emit16(cg, 0x588F);  /* ADDQ.L #4,A7 */
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
                        /* P105: `with MDDFdata^ do ...` where MDDFdata is a
                         * FIELD of an outer WITH's record (not a standalone
                         * symbol). Resolve via the WITH stack, then deref
                         * its pointer base type. Without this the inner
                         * WITH's record_type is NULL, all field lookups
                         * fall through to find_symbol_any, and names that
                         * collide with enum-ordinal globals (e.g.
                         * `fsversion` == terror.fsversion==0 from LDUTIL)
                         * resolve to 0. Mirrors the non-deref nested-WITH
                         * fallback a few lines below. */
                        if (!rt && cg->with_depth > 0) {
                            type_desc_t *wrt = NULL;
                            int widx = -1;
                            int fld = with_lookup_field(cg, ptr_node->name, &wrt, &widx);
                            if (fld >= 0 && wrt) {
                                type_desc_t *ft = wrt->fields[fld].type;
                                if (ft && ft->kind == TK_POINTER && ft->base_type)
                                    rt = ft->base_type;
                            }
                        }
                    } else if (ptr_node->type == AST_DEREF && ptr_node->children[0]) {
                        /* WITH ptr^^ DO ... — double-deref: follow the
                         * pointer chain twice. Required for patterns like
                         * INIT_JTDRIVER's `WITH jtpointer^^ do`. */
                        ast_node_t *inner = ptr_node->children[0];
                        if (inner->type == AST_IDENT_EXPR) {
                            cg_symbol_t *sym = find_symbol_any(cg, inner->name);
                            if (sym && sym->type && sym->type->kind == TK_POINTER && sym->type->base_type) {
                                type_desc_t *t1 = sym->type->base_type;
                                if (t1 && t1->kind == TK_POINTER && t1->base_type)
                                    rt = t1->base_type;
                                else
                                    rt = t1;  /* one level was enough */
                            }
                        }
                    } else if (ptr_node->type == AST_FIELD_ACCESS && ptr_node->children[0]) {
                        /* P107: WITH rec.fieldptr^ DO ... — the WITH expr
                         * is a dereferenced pointer field of a record
                         * (local variable or outer structure). fs_mount's
                         * `with ptrDCB^.MDDFdata^ do ...` pre-P107 hit this
                         * branch with NULL rt because no FIELD_ACCESS
                         * handler existed — the inner AST_DEREF(ptr_node)
                         * saw ptr_node->type != IDENT/DEREF/ARRAY and fell
                         * through. Result: WITH record_type stayed NULL,
                         * and `rootsnum` inside the body couldn't resolve
                         * via with_lookup_field. find_symbol_any returned
                         * NULL too, so codegen emitted MOVE.W #0,D0 — i.e.
                         * open_sfile was called with snum=0 instead of
                         * MDDFdata.rootsnum(=3). The sfile-0 s_entry is
                         * all-zeros on our disk image, so slist_io returned
                         * hintaddr=0 and OPEN_SFILE fired E1_SENTRY_BAD →
                         * 10707 stup_fsinit.
                         *
                         * Resolve the field via lvalue_field_info on the
                         * parent node; if the field type is a pointer,
                         * deref to get the record type. */
                        int fld_off;
                        type_desc_t *fld_type = NULL;
                        if (lvalue_field_info(cg, ptr_node->children[0],
                                              ptr_node->name, &fld_off, &fld_type)) {
                            if (fld_type && fld_type->kind == TK_POINTER &&
                                fld_type->base_type)
                                rt = fld_type->base_type;
                        }
                    } else if (ptr_node->type == AST_ARRAY_ACCESS && ptr_node->children[0]) {
                        /* P97: WITH arr[i]^ DO ... — array element is a
                         * pointer, deref gives the record. Used throughout
                         * NEW_CONFIG: `with configinfo[config_index]^ do`.
                         * Without this, the inner WITH's record_type was
                         * NULL and all field references (drvrec_ptr,
                         * required_drvr, permanent) fell through to the
                         * placeholder path — silently dropping 3 field
                         * writes and causing workptr to read uninitialized
                         * stack memory, UP() to never get a valid driver,
                         * and 10740/10741 at bootdev init. */
                        ast_node_t *arr = ptr_node->children[0];
                        type_desc_t *at = NULL;
                        if (arr->type == AST_IDENT_EXPR) {
                            cg_symbol_t *sym = find_symbol_any(cg, arr->name);
                            if (sym && sym->type) at = sym->type;
                        }
                        if (at) {
                            if (at->kind == TK_POINTER && at->base_type)
                                at = at->base_type;
                            if (at->kind == TK_ARRAY && at->element_type) {
                                type_desc_t *et = at->element_type;
                                if (et && et->kind == TK_POINTER && et->base_type)
                                    et = et->base_type;
                                rt = et;
                            }
                        }
                    }
                } else if (rec_expr->type == AST_IDENT_EXPR) {
                    /* WITH var DO ... — get variable's type */
                    cg_symbol_t *sym = find_symbol_any(cg, rec_expr->name);
                    if (sym && sym->type) {
                        rt = sym->type;
                        if (rt->kind == TK_POINTER && rt->base_type)
                            rt = rt->base_type;
                    } else if (cg->with_depth > 0) {
                        /* Nested WITH: `with c_mmrb^ do ... with head_sdb do ...`
                         * head_sdb is a field of the outer WITH's record, not a
                         * standalone symbol. Resolve it via the WITH stack.
                         * gen_with_base/gen_lvalue_addr already know how to emit
                         * the field's address via the outer WITH — we just need
                         * the correct field type here so the inner WITH's
                         * record_type is non-NULL (otherwise field name lookups
                         * in the body fall through to 'Unknown symbol' and emit
                         * LEA #0,A0 — silently corrupting low memory). */
                        type_desc_t *wrt = NULL;
                        int widx = -1;
                        int fld = with_lookup_field(cg, rec_expr->name, &wrt, &widx);
                        if (fld >= 0 && wrt) {
                            type_desc_t *ft = wrt->fields[fld].type;
                            if (ft && ft->kind == TK_POINTER && ft->base_type)
                                ft = ft->base_type;
                            rt = ft;
                        }
                    }
                } else if (rec_expr->type == AST_ARRAY_ACCESS && rec_expr->children[0]) {
                    /* WITH arr[i] DO ... — get element type. Cases:
                     *   arr[i]     where arr is a direct array variable
                     *   ptr^[i]    where ptr is a pointer-to-array — used in
                     *              SETMMU's `with ptr_smt^[128*domain+index] do`
                     *              and source-LOADER's `with smt_adr^[i] do`.
                     *              Without this branch the WITH body loses its
                     *              record context; field writes silently become
                     *              no-ops and the SMT never gets written. */
                    ast_node_t *base = rec_expr->children[0];
                    if (base->type == AST_IDENT_EXPR) {
                        cg_symbol_t *sym = find_symbol_any(cg, base->name);
                        type_desc_t *at = (sym && sym->type) ? sym->type : NULL;
                        if (at && at->kind == TK_POINTER && at->base_type)
                            at = at->base_type;
                        if (at && at->kind == TK_ARRAY && at->element_type)
                            rt = at->element_type;
                    } else if (base->type == AST_DEREF && base->children[0] &&
                               base->children[0]->type == AST_IDENT_EXPR) {
                        cg_symbol_t *sym = find_symbol_any(cg, base->children[0]->name);
                        if (sym && sym->type && sym->type->kind == TK_POINTER &&
                            sym->type->base_type &&
                            sym->type->base_type->kind == TK_ARRAY &&
                            sym->type->base_type->element_type)
                            rt = sym->type->base_type->element_type;
                    }
                } else if (rec_expr->type == AST_FIELD_ACCESS) {
                    /* P89e — WITH rec.subfield DO ... — walk the field chain
                     * via lvalue_record_type, which handles AST_IDENT_EXPR /
                     * AST_DEREF / nested AST_FIELD_ACCESS bases. Without this,
                     * Build_Stack's `with sloc_ptr^.env_save_area, stk_handle
                     * do begin SR := 0; PC := ord(@Initiate) end` had rt=NULL,
                     * field stores silently became no-ops, env_save_area.PC
                     * stayed $0 (later stomped to $CBFF by adjacent code), and
                     * Launch RTE'd to an odd PC → hard_excep → boot halt. */
                    type_desc_t *parent = lvalue_record_type(cg, rec_expr->children[0]);
                    if (parent && parent->kind == TK_RECORD) {
                        for (int pfi = 0; pfi < parent->num_fields; pfi++) {
                            if (str_eq_nocase(parent->fields[pfi].name, rec_expr->name)) {
                                type_desc_t *ft = parent->fields[pfi].type;
                                if (ft && ft->kind == TK_POINTER && ft->base_type)
                                    ft = ft->base_type;
                                rt = ft;
                                break;
                            }
                        }
                    }
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
            /* P66: Evaluate selector into D0 and PUSH onto stack. Case
             * bodies may call procs that clobber D3 (our prior scratch
             * register), which broke dispatch for later arms — e.g.
             * case 1,2's MAKE_NAME call corrupted D3 so CMP for case 4
             * compared against a stale register. Safer: keep selector
             * on stack, reload into D3 before each CMP. */
            int csz = expr_size(cg, node->children[0]);
            gen_expression(cg, node->children[0]);
            if (csz == 4)
                emit16(cg, 0x2F00);  /* MOVE.L D0,-(SP) */
            else
                emit16(cg, 0x3F00);  /* MOVE.W D0,-(SP) */

            uint32_t end_fixups[128];
            int num_fixups = 0;

            /* Case branches: pairs of (label_or_labels_group, statement)
             * starting at child[1]. Single-label arm: child = expr.
             * Multi-label arm: child = AST_CASE_LABELS with N label
             * children; emit one BEQ per label to the shared body. */
            int ci = 1;
            while (ci + 1 < node->num_children) {
                ast_node_t *label_node = node->children[ci];
                uint32_t body_pos = 0;
                uint32_t skip_fixups[16]; int num_skips = 0;

                if (label_node->type == AST_CASE_LABELS) {
                    uint32_t beq_positions[16]; int num_beqs = 0;
                    for (int li = 0; li < label_node->num_children; li++) {
                        /* P128h: case label is semantically a CHAR (for
                         * `case of char`) or an ordinal. Enable the
                         * char-literal short-circuit so 1-char string
                         * literals compile to MOVEQ #ch,D0 (value), not
                         * a string pointer. See comment in AST_STRING_LITERAL. */
                        cg->char_literal_context = true;
                        gen_expression(cg, label_node->children[li]);
                        cg->char_literal_context = false;
                        /* Reload selector from stack top into D3. */
                        if (csz == 4) emit16(cg, 0x262F);  /* MOVE.L 0(SP),D3 */
                        else          emit16(cg, 0x362F);  /* MOVE.W 0(SP),D3 */
                        emit16(cg, 0);
                        if (csz == 4) emit16(cg, 0xB083);  /* CMP.L D3,D0 */
                        else          emit16(cg, 0xB043);  /* CMP.W D3,D0 */
                        emit16(cg, 0x6700);  /* BEQ.W body */
                        if (num_beqs < 16) beq_positions[num_beqs++] = cg->code_size;
                        emit16(cg, 0);
                    }
                    emit16(cg, 0x6000);  /* BRA.W past_body */
                    if (num_skips < 16) skip_fixups[num_skips++] = cg->code_size;
                    emit16(cg, 0);
                    align_code(cg);
                    body_pos = cg->code_size;
                    for (int bi = 0; bi < num_beqs; bi++) {
                        patch16(cg, beq_positions[bi],
                                (uint16_t)(body_pos - beq_positions[bi]));
                    }
                } else {
                    /* Single-label arm — same P128h char-label context. */
                    cg->char_literal_context = true;
                    gen_expression(cg, label_node);
                    cg->char_literal_context = false;
                    if (csz == 4) emit16(cg, 0x262F);
                    else          emit16(cg, 0x362F);
                    emit16(cg, 0);
                    if (csz == 4) emit16(cg, 0xB083);
                    else          emit16(cg, 0xB043);
                    emit16(cg, 0x6600);  /* BNE.W next_case */
                    if (num_skips < 16) skip_fixups[num_skips++] = cg->code_size;
                    emit16(cg, 0);
                }

                /* Case body */
                gen_statement(cg, node->children[ci + 1]);

                /* BRA.W end_case */
                align_code(cg);
                emit16(cg, 0x6000);
                if (num_fixups < 128) end_fixups[num_fixups++] = cg->code_size;
                emit16(cg, 0);

                /* Patch the "skip body" branches to land here. */
                align_code(cg);
                for (int si = 0; si < num_skips; si++) {
                    patch16(cg, skip_fixups[si],
                            (uint16_t)(cg->code_size - skip_fixups[si]));
                }
                ci += 2;
                (void)body_pos;
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
            /* Pop the selector we pushed at case entry. */
            if (csz == 2)      emit16(cg, 0x544F);  /* ADDQ.W #2,A7 */
            else if (csz == 4) emit16(cg, 0x588F);  /* ADDQ.L #4,A7 */
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
    /* P39 structural: PASCALDEFS-pinned globals. When a Pascal VAR name
     * matches one of these names (case-insensitive), force its A5-relative
     * offset to the PASCALDEFS-hardcoded value. This retires a whole class
     * of tactical HLE bypasses (P32, P33 etc.) where Pascal-compiled global
     * offsets disagree with asm code's hardcoded PASCALDEFS offsets. */
    struct { const char *name; int offset; } pdefs_pins[] = {
        /* Scheduler queue heads — asm PROCASM.TEXT addresses them
         * directly as PFWD_REA(A5), PFWD_BLO(A5), etc. The asm treats each
         * head as a 2-field struct { next, prev } where prev is at
         * PREV_SCH(head) = 4(head). Apple's layout therefore places
         * bkwd_* at fwd_* + 4 (higher A5-relative offset = less negative).
         * Pre-P128c had bkwd_* at fwd_* - 4 which caused QUEUE_PR's
         * `MOVEA.L PREV_SCH(A0),A2` to read the WRONG global (0 for an
         * unused slot), which then corrupted the vector table on the
         * subsequent Q_PCB enqueue. */
        { "fwd_ReadyQ",  -1116 },   /* PFWD_REA */
        { "bkwd_ReadyQ", -1112 },   /* PFWD_REA + 4 */
        { "fwd_BlockQ",  -1108 },   /* PFWD_BLO */
        { "bkwd_BlockQ", -1104 },   /* PFWD_BLO + 4 */
        /* Sysglobal pointer — asm uses B_SYSLOCAL_PTR(A5) at -24785 */
        { "b_syslocal_ptr", -24785 },
        /* System globals referenced from asm */
        { "Invoke_sched", -24786 },
        { "sct_ptr",      -24781 },
        { "c_pcb_ptr",    -24617 },
        { "sysA5",        -24613 },
        { "port_cb_ptrs", -24609 },
        { "size_sglobal", -24577 },
        { "sg_free_pool_addr", -24575 },
        /* P62: add remaining PASCALDEFS-hardcoded globals so MM_INIT and
         * other early init code can find these via asm-pinned offsets.
         * Without these, Pascal's natural A5-relative placement differs
         * from PASCALDEFS and asm code reads wrong memory. */
        { "param_mem",    -24853 },
        { "dct",          -24883 },
        { "smt_addr",     -24887 },
        { "pe_loc1",      -24889 },
        { "pe_loc2",      -24893 },
        { "pe_phase",     -24894 },
        { "pe_access",    -24895 },
        { "membase",      -24899 },
        { "memleng",      -24903 },
        { "schdaddr",     -24907 },
        { "stataddr",     -24911 },
        { "sctab",        -25661 },
        { "lb_loaded",    -25662 },
        { "lb_enabled",   -25663 },
        { "domsmtba",     -25671 },
        { "mmrb_addr",    -25691 },
        { "meastabl",     -26491 },
        { NULL, 0 }
    };

    char *tok = strtok(names, ",");
    while (tok) {
        while (*tok == ' ') tok++;
        int sz = type_size(type);
        if (is_global) {
            /* Check if this global was already defined in a previously compiled
             * unit (imported). If so, reuse its offset to avoid double-allocation
             * of the shared A5-relative global data area. All OS kernel units share
             * one A5 frame; allocating a new offset for an already-placed global
             * causes overlapping variables (e.g., sg_free_pool_addr at A5-192
             * overlapping with size_sglobal at A5-194). */
            cg_symbol_t *existing_import = NULL;
            if (cg->imported_globals) {
                for (int ig = 0; ig < cg->imported_globals_count; ig++) {
                    if (str_eq_nocase(cg->imported_globals[ig].name, tok)) {
                        existing_import = &cg->imported_globals[ig];
                        break;
                    }
                }
            }
            /* Check PASCALDEFS-pinned list (P39 structural). Case-insensitive. */
            int pinned_offset = 0;
            for (int pi = 0; pdefs_pins[pi].name; pi++) {
                if (str_eq_nocase(pdefs_pins[pi].name, tok)) {
                    pinned_offset = pdefs_pins[pi].offset;
                    break;
                }
            }
            cg_symbol_t *s = add_global_sym(cg, tok, type);
            if (s) {
                bool trace = s->name[0] && (strcasecmp(s->name, "param_mem") == 0 ||
                                            strcasecmp(s->name, "Invoke_sched") == 0 ||
                                            strcasecmp(s->name, "b_syslocal_ptr") == 0 ||
                                            strcasecmp(s->name, "sct_ptr") == 0);
                if (trace) {
                    fprintf(stderr, "[GA-ENTER] %s sz=%d pinned=%d import=%p(off=%d) type_kind=%d\n",
                            s->name, sz, pinned_offset,
                            (void*)existing_import,
                            existing_import ? existing_import->offset : 0,
                            type ? (int)type->kind : -1);
                }
                if (pinned_offset != 0) {
                    /* PASCALDEFS pin wins over natural offset */
                    s->offset = pinned_offset;
                    if (trace) fprintf(stderr, "  -> PINNED offset=%d\n", s->offset);
                } else if (existing_import && existing_import->offset != 0) {
                    /* Use the previously assigned offset */
                    s->offset = existing_import->offset;
                    if (trace) fprintf(stderr, "  -> IMPORTED offset=%d\n", s->offset);
                } else {
                    /* Assign new global offset (grow DOWNWARD from A5).
                     * Lisa Pascal convention: A5 points to the top of the global
                     * data area. Globals use negative offsets: A5-2, A5-4, etc.
                     * Uses a process-wide counter because all kernel units
                     * share the same A5-relative global data area. */
                    static int global_offset = 0;
                    if (sz >= 2 && (global_offset % 2)) global_offset++;
                    global_offset += sz;
                    s->offset = -global_offset;
                    if (s->name[0] && (strcasecmp(s->name, "param_mem") == 0 ||
                                       strcasecmp(s->name, "Invoke_sched") == 0 ||
                                       strcasecmp(s->name, "b_syslocal_ptr") == 0 ||
                                       strcasecmp(s->name, "sct_ptr") == 0)) {
                        fprintf(stderr, "[GLOBAL_ALLOC] %s: sz=%d global_offset(after)=%d offset=%d (=$%08X) type_kind=%d\n",
                                s->name, sz, global_offset, s->offset,
                                (uint32_t)s->offset,
                                s->type ? (int)s->type->kind : -1);
                    }
                }
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
                    /* P71: evaluate simple constant expressions. Apple's
                     * Pascal allows `const X = -1` (unary minus on literal)
                     * which parses as UNOP(MINUS, INT(1)). Taking int_val
                     * directly from the UNOP node yields 0, not -1.
                     * Handle unary +/- and identifier refs to prior CONSTs. */
                    ast_node_t *v = child->children[0];
                    int val = 0;
                    if (v->type == AST_UNARY_OP &&
                        v->num_children > 0 &&
                        v->children[0]->type == AST_INT_LITERAL) {
                        int inner = (int)v->children[0]->int_val;
                        val = (v->op == TOK_MINUS) ? -inner : inner;
                    } else if (v->type == AST_IDENT_EXPR) {
                        cg_symbol_t *ref = find_global(cg, v->name);
                        if (!ref) ref = find_imported(cg, v->name);
                        if (ref && ref->is_const) val = ref->offset;
                    } else {
                        val = (int)v->int_val;
                    }
                    s->offset = val;
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
                        /* INTERFACE/top-level declarations: nest_depth = 1. */
                        register_proc_sig(cg, child->name,
                                          child->children[j]->children,
                                          child->children[j]->num_children,
                                          1);
                        has_params = true;
                        break;
                    }
                }
                /* P80h2: always register a sig for every proc/func decl, even
                 * parameterless non-external ones. Without this, bodies like
                 * `procedure MEMMGR;` never enter proc_sigs, so find_proc_sig
                 * returns NULL at @MEMMGR sites and the codegen falls back to
                 * treating the name as an A5-relative global variable. That
                 * shipped garbage addresses into PCB.start_PC (e.g. $CCB802
                 * instead of MEMMGR's actual entry $043F56). */
                if (!has_params && !find_proc_sig(cg, child->name) &&
                    cg->proc_sigs && cg->num_proc_sigs < CODEGEN_MAX_PROC_SIGS) {
                    cg_proc_sig_t *sig = &cg->proc_sigs[cg->num_proc_sigs++];
                    memset(sig, 0, sizeof(*sig));
                    strncpy(sig->name, child->name, 63);
                    sig->is_external = is_ext;
                    sig->is_function = is_func;
                    /* Interface/top-level declaration: nest_depth = 1. */
                    sig->nest_depth = 1;
                    sig->takes_static_link = false;
                }
                /* Set is_function flag on the registered signature */
                {
                    cg_proc_sig_t *sig = find_proc_sig(cg, child->name);
                    if (sig) sig->is_function = is_func;
                }
                /* If external declaration, mark existing signature */
                if (is_ext) {
                    cg_proc_sig_t *existing = find_proc_sig(cg, child->name);
                    if (existing) { existing->is_external = true; existing->is_function = is_func; }
                }
                /* P54: capture the return type of functions so expr_size()
                 * correctly sizes FUNC_CALL expressions. Without this, a
                 * function returning absptr (e.g. MMU_Base) looks like a
                 * 2-byte return → binary-op use_long=false → ADD.W + an
                 * EXT.L store-widen that zeroes the high word of a 32-bit
                 * pointer sum. Bug: Build_Syslocal's final assignment
                 * `sloc_ptr^.sl_free_pool_addr := MMU_Base(syslocmmu) +
                 * Sizeof(syslocal)` wrote \$0B0000 (EXT.L-of-\$01EE) to
                 * the wrong place, cascading into the unitio spin. */
                type_desc_t *ret_type = NULL;
                if (is_func) {
                    for (int j = 0; j < child->num_children; j++) {
                        ast_node_t *c = child->children[j];
                        /* RETURN_TYPE is a TYPE_* node sitting between
                         * PARAM_LIST and the body (BLOCK/decls). */
                        if (c->type == AST_TYPE_IDENT ||
                            c->type == AST_TYPE_SUBRANGE ||
                            c->type == AST_TYPE_ARRAY ||
                            c->type == AST_TYPE_RECORD ||
                            c->type == AST_TYPE_SET ||
                            c->type == AST_TYPE_POINTER ||
                            c->type == AST_TYPE_STRING ||
                            c->type == AST_TYPE_PACKED ||
                            c->type == AST_TYPE_ENUM ||
                            c->type == AST_TYPE_FILE) {
                            ret_type = resolve_type(cg, c);
                            break;
                        }
                    }
                }
                /* Only register as a linker symbol if EXTERNAL.
                 * INTERFACE/FORWARD declarations must NOT create linker symbols
                 * at offset 0 — that shadows the real IMPLEMENTATION symbol.
                 * gen_proc_or_func handles symbol creation for implementations. */
                if (is_ext) {
                    cg_symbol_t *psym = add_global_sym(cg, child->name, ret_type);
                    if (psym) psym->is_external = true;
                }
                break;
            }
            default:
                break;
        }
    }

    /* Forward-reference fixup: resolve pointer base_type that was NULL
     * because the pointed-to type hadn't been declared yet. */
    for (int i = 0; i < cg->num_types; i++) {
        type_desc_t *t = &cg->types[i];
        if (t->kind == TK_POINTER && !t->base_type && t->base_name[0]) {
            t->base_type = find_type(cg, t->base_name);
        }
    }
}

static void gen_proc_or_func(codegen_t *cg, ast_node_t *node) {
    if (!node->name[0]) return;

    /* Check for EXTERNAL/FORWARD */
    if (str_eq_nocase(node->str_val, "EXTERNAL") || str_eq_nocase(node->str_val, "FORWARD")) {
        /* P54: capture the function return type so callers' expr_size()
         * can correctly size FUNC_CALL expressions. Crucial for pointer-
         * returning funcs like MMU_Base(). */
        type_desc_t *ret_type = NULL;
        if (node->type == AST_FUNC_DECL) {
            for (int j = 0; j < node->num_children; j++) {
                ast_node_t *c = node->children[j];
                if (c->type == AST_TYPE_IDENT ||
                    c->type == AST_TYPE_SUBRANGE ||
                    c->type == AST_TYPE_ARRAY ||
                    c->type == AST_TYPE_RECORD ||
                    c->type == AST_TYPE_SET ||
                    c->type == AST_TYPE_POINTER ||
                    c->type == AST_TYPE_STRING ||
                    c->type == AST_TYPE_PACKED ||
                    c->type == AST_TYPE_ENUM ||
                    c->type == AST_TYPE_FILE) {
                    ret_type = resolve_type(cg, c);
                    break;
                }
            }
        }
        cg_symbol_t *s = add_global_sym(cg, node->name, ret_type);
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
    /* P80h3: remember this scope's proc name so exit(name) can tell
     * whether it's exiting the current proc (no static-link walk) or an
     * enclosing proc (walk up N levels). */
    {
        cg_scope_t *sc = current_scope(cg);
        if (sc && node->name[0])
            strncpy(sc->proc_name, node->name, sizeof(sc->proc_name) - 1);
    }
    /* P81 static-link ABI: for procs at depth >= 2, reserve 4 bytes at the
     * top of the local area (-4(A6)) for a static-link slot. Caller passes
     * the correct static-parent A6 in A2; callee copies A2 to -4(A6) in
     * its prologue. emit_frame_access walks this chain to reach outer
     * scopes' locals. Without this, sibling-nested calls corrupted A0
     * because the compiler used the dynamic link as the static link. */
    {
        cg_scope_t *sc = current_scope(cg);
        if (sc && cg->scope_depth >= 2) {
            sc->frame_size = 4;  /* reserved for static link at -4(A6) */
        }
    }

    /* Reset per-procedure label state */
    cg->num_labels = 0;
    cg->num_pending_gotos = 0;

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
                    int psz;
                    if (is_var) psz = 4;
                    else if (!ptype) psz = 2;
                    else if (ptype->size == 4) psz = 4;
                    else if (ptype->size == 1 || ptype->size == 2) psz = 2;
                    else psz = 4;  /* strings/records/arrays by ref (match register_proc_sig) */
                    param_offset += psz;
                    if (param_offset % 2) param_offset++;
                }
            }
        }
    }
    /* If IMPLEMENTATION body has no param list, reconstruct params from
     * the INTERFACE/FORWARD declaration's stored signature */
    if (!has_param_list) {
        cg_proc_sig_t *sig = find_proc_sig(cg, node->name);
        if (!sig && strcasestr(cg->current_file, "SYSG1"))
            fprintf(stderr, "  WARNING: no sig for '%s' in %s (has_param_list=0)\n",
                    node->name, cg->current_file);
        if (sig && sig->num_params > 0) {
            if (strcasestr(cg->current_file, "SYSG1"))
                fprintf(stderr, "  RECONST '%s': %d params\n", node->name, sig->num_params);
            for (int j = 0; j < sig->num_params; j++) {
                type_desc_t *ptype = sig->param_type[j];
                if (!ptype) ptype = find_type(cg, "integer");
                cg_symbol_t *s = add_local(cg, sig->param_name[j], ptype, true, sig->param_is_var[j]);
                if (s) {
                    s->offset = param_offset;
                    /* Prefer sig->param_size[j] (set correctly at
                     * register_proc_sig time when types were fully
                     * resolved). Fall back to ptype-derived calc. */
                    int psz;
                    if (sig->param_size[j] == 2 || sig->param_size[j] == 4) {
                        psz = sig->param_size[j];
                    } else if (sig->param_is_var[j]) psz = 4;
                    else if (!ptype) psz = 2;
                    else if (ptype->size == 4) psz = 4;
                    else if (ptype->size == 1 || ptype->size == 2) psz = 2;
                    else psz = 4;
                    param_offset += psz;
                    if (param_offset % 2) param_offset++;
                }
            }
        }
    }

    /* Register procedure signature for VAR parameter tracking at call sites.
     * nest_depth is the current scope depth (already pushed above).
     * P81b: register a sig for EVERY proc/func body, including parameterless
     * ones. Without this, nested parameterless procs (e.g. SET_INMOTION_SEG)
     * had no sig, so callers didn't pass the static link — the callee's
     * prologue then saved stale A2 into -4(A6), poisoning the static chain.
     *
     * P103: IMPLEMENTATION bodies declared with `(* var x:T *)` asterisk-
     * comment args (Apple Pascal convention to avoid repeating signature)
     * arrive here with NO PARAM_LIST child — the lexer ate the entire
     * `( ... )` as a comment. The INTERFACE pre-pass already registered
     * the real signature (e.g. sfileio's def_mount/real_mount/fs_mount
     * with `var ecode: error; device: integer`). Re-registering with 0
     * params would create a duplicate sig that fs_mount's callers could
     * find first — pushing (value, value) instead of (@ecode, device),
     * passing a nil ecode pointer into def_mount and aborting FS_Mount
     * with error>0 → SYSTEM_ERROR(10707). So only create a new sig when
     * the body genuinely has no prior-registered sig. */
    {
        bool had_params = false;
        for (int i = 0; i < node->num_children; i++) {
            if (node->children[i]->type == AST_PARAM_LIST) {
                ast_node_t *params = node->children[i];
                register_proc_sig(cg, node->name, params->children,
                                  params->num_children, cg->scope_depth);
                had_params = true;
                break;
            }
        }
        if (!had_params) {
            cg_proc_sig_t *existing = find_proc_sig(cg, node->name);
            if (!existing || existing->num_params == 0) {
                register_proc_sig(cg, node->name, NULL, 0, cg->scope_depth);
            } else if (existing) {
                /* Body for an INTERFACE-declared proc. Keep the real sig;
                 * just refresh its nest_depth to the body's scope. */
                existing->nest_depth = cg->scope_depth;
                existing->takes_static_link = (cg->scope_depth >= 2);
            }
        }
    }

    /* Compiling a procedure BODY clears any prior EXTERNAL flag on this
     * procedure's signature — the body exists, so callers should use the
     * Pascal caller-clean convention for it. */
    {
        cg_proc_sig_t *own_sig = find_proc_sig(cg, node->name);
        if (own_sig) own_sig->is_external = false;
        cg_symbol_t *own_sym = find_global(cg, node->name);
        if (own_sym) own_sym->is_external = false;
    }

    /* Process local declarations (vars, consts, types).
     * Order matters: CONSTs first (so TYPEs can reference them in
     * array bounds and enum ranges), then TYPEs (so VARs can resolve
     * their type names), then VARs. Previously TYPEs were never
     * registered as local, so a local `type djt = ^driverjt` inside
     * a proc stayed unresolved and `var x: ^djt` got an opaque base,
     * making sizeof and array-stride wrong — the bug that let
     * INIT_JTDRIVER corrupt the CPU vector table. */
    for (int i = 0; i < node->num_children; i++) {
        ast_node_t *child = node->children[i];
        if (child->type == AST_CONST_DECL) {
            type_desc_t *tc = find_type(cg, "integer");
            cg_symbol_t *s = add_global_sym(cg, child->name, tc);
            if (s && child->num_children > 0) {
                /* P71: evaluate unary-minus + ident-ref CONST expressions. */
                ast_node_t *v = child->children[0];
                int val = 0;
                if (v->type == AST_UNARY_OP &&
                    v->num_children > 0 &&
                    v->children[0]->type == AST_INT_LITERAL) {
                    int inner = (int)v->children[0]->int_val;
                    val = (v->op == TOK_MINUS) ? -inner : inner;
                } else if (v->type == AST_IDENT_EXPR) {
                    cg_symbol_t *ref = find_global(cg, v->name);
                    if (!ref) ref = find_imported(cg, v->name);
                    if (ref && ref->is_const) val = ref->offset;
                } else {
                    val = (int)v->int_val;
                }
                s->offset = val;
                s->is_const = true;
            }
        }
    }
    /* Two-pass TYPE processing: (1) register each name as a stub so
     * forward references resolve; (2) resolve the actual type
     * definitions. This mirrors what process_declarations does for
     * global types and lets `type djt = ^driverjt; driverjt = record...`
     * work in either source order. */
    for (int i = 0; i < node->num_children; i++) {
        ast_node_t *child = node->children[i];
        if (child->type == AST_TYPE_DECL && child->name[0]) {
            if (!find_type(cg, child->name))
                add_type(cg, child->name, TK_VOID, 0);
        }
    }
    for (int i = 0; i < node->num_children; i++) {
        ast_node_t *child = node->children[i];
        if (child->type == AST_TYPE_DECL && child->num_children > 0 && child->name[0]) {
            type_desc_t *t = resolve_type(cg, child->children[0]);
            if (t) {
                if (t->name[0] && !str_eq_nocase(t->name, child->name)) {
                    type_desc_t *alias = find_type(cg, child->name);
                    if (!alias) alias = add_type(cg, child->name, t->kind, t->size);
                    if (alias) {
                        char saved_name[64];
                        strncpy(saved_name, child->name, sizeof(saved_name) - 1);
                        saved_name[63] = '\0';
                        *alias = *t;
                        strncpy(alias->name, saved_name, sizeof(alias->name) - 1);
                    }
                } else {
                    /* If stub already exists with this name, overwrite in place.
                     * P80c: if the existing type is an IMPORTED record with valid
                     * offsets (from the pre-pass fixup), DON'T overwrite it — the
                     * local resolve_type may produce a copy with zeroed offsets
                     * due to the struct copy corruption bug. Instead, just discard
                     * the locally resolved type and keep the imported one. */
                    type_desc_t *existing = find_type(cg, child->name);
                    if (existing && existing != t) {
                        /* P80g: protect ANY record with valid field offsets from
                     * being overwritten by *existing = *t. The struct copy
                     * creates dangling type pointers (into cg->types which
                     * gets freed after compilation), causing the safety-net
                     * offset recomputation to read garbage sizes. If the
                     * existing record already has correct offsets, keep it. */
                        bool keep_existing = false;
                        if (existing->kind == TK_RECORD &&
                            existing->num_fields > 1 && existing->fields[1].offset > 0) {
                            keep_existing = true;
                        }
                        if (!keep_existing) {
                        char saved_name[64];
                        strncpy(saved_name, child->name, sizeof(saved_name) - 1);
                        saved_name[63] = '\0';
                        *existing = *t;
                        strncpy(existing->name, saved_name, sizeof(existing->name) - 1);
                        /* P80c: verify record field offsets after struct copy.
                         * The *existing = *t copy sometimes produces zeroed offsets
                         * (suspected memory corruption from large type arrays).
                         * Recompute offsets from field sizes as a safety net. */
                        if (existing->kind == TK_RECORD && existing->num_fields > 1 &&
                            existing->fields[1].offset == 0) {
                            int off = 0;
                            for (int fi = 0; fi < existing->num_fields; fi++) {
                                int fs = existing->fields[fi].type ? existing->fields[fi].type->size : 2;
                                if (existing->fields[fi].type && existing->fields[fi].type->kind == TK_STRING && (fs % 2)) fs++;
                                if (fs == 1 && existing->fields[fi].type &&
                                    (existing->fields[fi].type->kind == TK_BYTE || existing->fields[fi].type->kind == TK_CHAR ||
                                     existing->fields[fi].type->kind == TK_SUBRANGE))
                                    fs = 2;
                                if (fs >= 2 && (off % 2)) off++;
                                existing->fields[fi].offset = off;
                                off += fs;
                            }
                            if (off % 2) off++;
                            existing->size = off;
                        }
                        } /* end if (!imported_ok) */
                    } else {
                        strncpy(t->name, child->name, sizeof(t->name) - 1);
                    }
                }
            }
        }
    }
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

    /* For functions, create a local variable for the return value.
     * Must be done BEFORE LINK so frame_size includes the result slot. */
    /* For functions, create a local variable for the return value.
     * In Lisa Pascal, `funcname := expr` assigns to the result slot.
     * We load D0 from this slot before RTS. */
    bool is_function = (node->type == AST_FUNC_DECL);
    int func_result_offset = 0;
    if (is_function && node->name[0]) {
        cg_symbol_t *existing = find_local(cg, node->name);
        if (!existing) {
            /* Try to find return type from function declaration children */
            type_desc_t *rtype = NULL;
            for (int i = 0; i < node->num_children; i++) {
                ast_node_t *ch = node->children[i];
                if (ch->type == AST_IDENT_EXPR && ch->name[0]) {
                    type_desc_t *t = find_type(cg, ch->name);
                    if (t) { rtype = t; break; }
                }
            }
            if (!rtype) rtype = find_type(cg, "boolean"); /* common default for functions */
            int rsz = rtype ? rtype->size : 2;
            if (rsz < 2) rsz = 2;
            cg_scope_t *fsc2 = current_scope(cg);
            if (fsc2) {
                if (rsz >= 2 && (fsc2->frame_size % 2)) fsc2->frame_size++;
                fsc2->frame_size += rsz;
                cg_symbol_t *rv = add_local(cg, node->name, rtype, false, false);
                if (rv) {
                    rv->offset = -(int)fsc2->frame_size;
                    func_result_offset = rv->offset;
                    if (strcasestr(node->name, "GETSPACE") || strcasestr(node->name, "GETFREE"))
                        fprintf(stderr, "  FUNC_RESULT '%s' offset=%d sz=%d type=%s\n",
                                node->name, rv->offset, rsz, rtype ? rtype->name : "?");
                }
            }
        } else {
            func_result_offset = existing->offset;
        }
    }

    cg_scope_t *sc = current_scope(cg);
    int frame_size = sc ? sc->frame_size : 0;
    /* P89g: round frame_size up to an even byte count. LINK A6,#-disp pushes
     * A6 (4 bytes, keeps alignment) then subtracts disp from A7. An odd disp
     * leaves A7 misaligned, which 68000 word/long memory ops require to be
     * even. Block_Process had a single int1 (1-byte) local — frame_size=3
     * (rounded up from 1 to even isn't applied because the local-add code
     * only pads when the next field is >=2 bytes; a lone odd-sized last
     * local stays odd). The misaligned A7 propagates: an interrupt fired
     * on odd SP corrupts the saved exception frame, dispatching Scheduler
     * with garbage and crashing on POP's uninitialized env_save_area. */
    if (frame_size & 1) frame_size++;

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
    /* P81 static-link ABI: stash caller-provided static parent (passed in A2)
     * into the reserved -4(A6) slot so emit_frame_access can find it. */
    if (cg->scope_depth >= 2) {
        emit16(cg, 0x2D4A);  /* MOVE.L A2,-4(A6) */
        emit16(cg, 0xFFFC);
    }

    /* Generate body */
    for (int i = 0; i < node->num_children; i++) {
        if (node->children[i]->type == AST_BLOCK) {
            gen_statement(cg, node->children[i]);
        }
    }

    /* For functions: load result variable into D0 before returning */
    if (is_function && func_result_offset != 0) {
        int rsz = 2; /* default */
        cg_symbol_t *rv = find_local(cg, node->name);
        if (rv && rv->type) rsz = type_load_size(rv->type);
        if (rsz == 4)
            emit16(cg, 0x202E);  /* MOVE.L offset(A6),D0 */
        else
            emit16(cg, 0x302E);  /* MOVE.W offset(A6),D0 */
        emit16(cg, (uint16_t)(int16_t)func_result_offset);
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
            (void)impl_funcs;

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
static void register_proc_sig(codegen_t *cg, const char *name, ast_node_t *params[], int num_params, int nest_depth) {
    if (!cg->proc_sigs || cg->num_proc_sigs >= CODEGEN_MAX_PROC_SIGS) return;
    /* If a sig for this name already exists (forward/interface declaration),
     * update its nest_depth when we see the real body. */
    for (int i = 0; i < cg->num_proc_sigs; i++) {
        if (strcasecmp(cg->proc_sigs[i].name, name) == 0) {
            if (nest_depth > 0) {
                cg->proc_sigs[i].nest_depth = nest_depth;
                cg->proc_sigs[i].takes_static_link = (nest_depth >= 2);
            }
            /* Fall through and still create a new entry to keep existing
             * behavior (caller may add another sig for the body). */
            break;
        }
    }
    /* Log signature registrations for key boot procedures */
    cg_proc_sig_t *sig = &cg->proc_sigs[cg->num_proc_sigs++];
    strncpy(sig->name, name, 63);
    sig->nest_depth = nest_depth;
    sig->takes_static_link = (nest_depth >= 2);
    /* P128k: capture the enclosing (parent) procedure's name so
     * find_proc_sig can prefer a nested sig whose parent matches the
     * current compilation scope. A top-level (depth 1) proc has no
     * parent. When nest_depth >= 2, the parent is the innermost scope
     * that's ALREADY open at registration time. register_proc_sig is
     * called during parse/analysis of the parent's declaration block —
     * the parent's scope is cg->scopes[cg->scope_depth-1]. */
    /* register_proc_sig is called AFTER push_scope for the procedure being
     * registered — so scopes[scope_depth-1] is the current proc's own scope.
     * The real enclosing parent is at scopes[scope_depth-2]. Top-level procs
     * have scope_depth == 1 and thus no parent. */
    sig->parent_proc[0] = '\0';
    if (nest_depth >= 2 && cg->scope_depth >= 2) {
        const char *p = cg->scopes[cg->scope_depth - 2].proc_name;
        if (p && p[0]) strncpy(sig->parent_proc, p, 63);
    }
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
        /* P102: snapshot the type name (if any) at registration time so
         * callers can re-resolve by name if the pointer becomes stale
         * across compilation-unit boundaries. Also capture from the AST
         * directly when the type node is a plain identifier — even if
         * resolve_type returned NULL or an entry with a different name
         * (e.g. 8-char collision mapping LogicalAddress → logicaladr). */
        sig->param_type_name[i][0] = '\0';
        if (params[i]->num_children > 0) {
            ast_node_t *tn = params[i]->children[0];
            if (tn && tn->type == AST_TYPE_IDENT && tn->name[0])
                strncpy(sig->param_type_name[i], tn->name, 63);
        }
        if (!sig->param_type_name[i][0] && ptype && ptype->name[0])
            strncpy(sig->param_type_name[i], ptype->name, 63);
        /* Snapshot the kind at registration time. The ptype pointer can go
         * stale across compilation units because shared_types entries get
         * re-used for unrelated types with 8-char-colliding names (e.g.
         * HWINT's LogicalAddress and EXCEPRES's local logicaladr record).
         * This captures the correct kind so ARG_BY_REF doesn't flip. */
        sig->param_type_kind[i] = ptype ? (int)ptype->kind : 0;
        if (sig->param_is_var[i]) {
            sig->param_size[i] = 4;  /* VAR params are always pointers */
        } else if (!ptype) {
            sig->param_size[i] = 2;  /* unresolved — fallback to word */
        } else if (ptype->size == 4) {
            sig->param_size[i] = 4;  /* longint, pointer, real */
        } else if (ptype->size == 1 || ptype->size == 2) {
            sig->param_size[i] = 2;  /* byte/char/int/bool/enum all push as word */
        } else {
            /* String, record, array value params: Apple Pascal passes by
             * reference (4-byte pointer to the data). Frame layout below
             * must match this — non-primitive value params occupy 4 bytes. */
            sig->param_size[i] = 4;
        }
    }
}

/* Helper — does this sig have all params resolved to non-null types? */
static bool sig_is_fully_resolved(cg_proc_sig_t *sig) {
    for (int j = 0; j < sig->num_params; j++) {
        if (!sig->param_type[j]) return false;
    }
    return true;
}

/* Look up a procedure signature by name.
 *
 * Priority (most to least preferred):
 *   1. Local non-external with all params resolved.
 *   2. Local non-external (params may be partial).
 *   3. Imported non-external with all params resolved.
 *   4. Imported non-external (params may be partial).
 *   5. Any external declaration (forward decl).
 *
 * P89d: added the "fully resolved" tier. Without it, the first non-external
 * entry wins even if its param_type[*] are NULL (forward-decl parsed before
 * the param's type was known). For MAP_SYSLOCAL(c_pcb: ptr_pcb) that meant
 * Scheduler's cross-unit call pushed c_pcb as a WORD (param_size=2 fallback
 * for unresolved ptype), truncating the pointer and making MAP_SYSLOCAL
 * read junk via the MMU-mapped frame. With this fix, Scheduler picks the
 * later registration where ptr_PCB resolved to a 4-byte pointer. */
static cg_proc_sig_t *find_proc_sig(codegen_t *cg, const char *name) {
    cg_proc_sig_t *local_partial = NULL;
    cg_proc_sig_t *imp_resolved = NULL;
    cg_proc_sig_t *imp_partial = NULL;
    cg_proc_sig_t *fallback = NULL;
    (void)0; /* P128k: parent_proc tracking kept in sig for future use; lookup
              * disabled because it requires symbol-table mangling to emit
              * different addresses for same-named nested procs across parents,
              * which in turn collides with the linker's 8-char prefix match
              * and breaks other call paths. Kept the parent_proc field so
              * find_proc_sig can tier on it later when the linker learns to
              * suppress prefix match for dot-mangled names. */
    /* Search local signatures */
    if (cg->proc_sigs) {
        for (int i = 0; i < cg->num_proc_sigs; i++) {
            cg_proc_sig_t *s = &cg->proc_sigs[i];
            if (strcasecmp(s->name, name) != 0) continue;
            if (!s->is_external) {
                if (sig_is_fully_resolved(s)) return s;   /* tier 1 */
                if (!local_partial) local_partial = s;
            } else if (!fallback) {
                fallback = s;
            }
        }
    }
    if (local_partial) return local_partial;              /* tier 2 */
    /* Search imported signatures */
    if (cg->imported_proc_sigs) {
        for (int i = 0; i < cg->imported_proc_sigs_count; i++) {
            cg_proc_sig_t *s = &cg->imported_proc_sigs[i];
            if (strcasecmp(s->name, name) != 0) continue;
            if (!s->is_external) {
                if (sig_is_fully_resolved(s)) {
                    if (!imp_resolved) imp_resolved = s;
                } else if (!imp_partial) {
                    imp_partial = s;
                }
            } else if (!fallback) {
                fallback = s;
            }
        }
    }
    if (imp_resolved) return imp_resolved;                /* tier 3 */
    if (imp_partial) return imp_partial;                  /* tier 4 */
    if (fallback) return fallback;                        /* tier 5 */

    /* P102b: 8-char-significant fallback, mirroring the linker.
     * STARTUP calls INIT_TWIGGGLOB (3 Gs, 14 chars); the decl is
     * INIT_TWIGGLOB (2 Gs, 13 chars). No exact strcasecmp match —
     * but under Apple's 8-char rule they're the same identifier.
     * Pick the candidate with the longest common prefix (LCP) so
     * INIT_TWIGGGLOB → INIT_TWIGGLOB (LCP 10) beats
     * INIT_TWIGGGLOB → INIT_TWIG_TABLE (LCP 9).
     *
     * P102c: require BOTH the reference and the candidate to be >= 8
     * chars before the prefix match applies. Short names (< 8 chars)
     * are distinct identifiers under Apple's rule and must only match
     * via exact strcasecmp (the tiers above). Without this guard, a
     * WITH-field ident like `Dimcont` (7 chars) LCP-matches
     * `DimContrast` (11 chars) and the compiler treats a field read as
     * a no-arg proc call — emitting a spurious JSR that corrupts the
     * stack and, downstream, the sysglobal free-list fwdlink RELSPACE
     * later walks. */
    size_t rlen = strlen(name);
    if (rlen < 8) return NULL;
    cg_proc_sig_t *best_local = NULL,   *best_imp = NULL,   *best_fb = NULL;
    size_t best_local_lcp = 0,          best_imp_lcp = 0,   best_fb_lcp = 0;
    #define LCP_UPDATE(cand, best, best_lcp) do {                          \
        if (strlen((cand)->name) < 8) break;                              \
        const char *a = (cand)->name; const char *b = name; size_t l = 0; \
        while (a[l] && b[l] &&                                             \
               toupper((unsigned char)a[l]) == toupper((unsigned char)b[l])) \
            l++;                                                           \
        if (!(best) || l > (best_lcp)) { (best) = (cand); (best_lcp) = l; } \
    } while (0)
    if (cg->proc_sigs) {
        for (int i = 0; i < cg->num_proc_sigs; i++) {
            cg_proc_sig_t *s = &cg->proc_sigs[i];
            if (strncasecmp(s->name, name, 8) != 0) continue;
            if (!s->is_external && sig_is_fully_resolved(s)) {
                LCP_UPDATE(s, best_local, best_local_lcp);
            } else if (s->is_external) {
                LCP_UPDATE(s, best_fb, best_fb_lcp);
            }
        }
    }
    if (best_local) return best_local;
    if (cg->imported_proc_sigs) {
        for (int i = 0; i < cg->imported_proc_sigs_count; i++) {
            cg_proc_sig_t *s = &cg->imported_proc_sigs[i];
            if (strncasecmp(s->name, name, 8) != 0) continue;
            if (!s->is_external && sig_is_fully_resolved(s)) {
                LCP_UPDATE(s, best_imp, best_imp_lcp);
            } else if (s->is_external) {
                LCP_UPDATE(s, best_fb, best_fb_lcp);
            }
        }
    }
    #undef LCP_UPDATE
    if (best_imp) return best_imp;
    return best_fb;
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
