/*
 * LisaEm Toolchain — Lisa Pascal Code Generator
 *
 * Takes a parser AST and generates Motorola 68000 machine code
 * in Lisa object format.
 *
 * Calling convention (Lisa Pascal):
 *   - Parameters pushed right-to-left on stack
 *   - Function results: byte/word in D0, long in D0,
 *     real on stack, records/strings via hidden pointer
 *   - A5 = global data pointer
 *   - A6 = frame pointer (LINK/UNLK)
 *   - A7 = stack pointer
 *   - Caller cleans up parameters
 *   - Local variables below frame pointer (negative offsets from A6)
 */

#ifndef PASCAL_CODEGEN_H
#define PASCAL_CODEGEN_H

#include "pascal_parser.h"
#include <stdint.h>
#include <stdbool.h>

/* Code buffer limits */
#define CODEGEN_MAX_OUTPUT   (256 * 1024)
#define CODEGEN_MAX_SYMBOLS  4096
#define CODEGEN_MAX_RELOCS   16384
#define CODEGEN_MAX_STRINGS  1024
#define CODEGEN_MAX_LOCALS   256
#define CODEGEN_MAX_SCOPE    32

/* Type kinds */
typedef enum {
    TK_VOID,
    TK_INTEGER,     /* 16-bit signed */
    TK_LONGINT,     /* 32-bit signed */
    TK_BYTE,        /* 8-bit unsigned */
    TK_BOOLEAN,     /* 8-bit */
    TK_CHAR,        /* 8-bit */
    TK_REAL,        /* 32-bit IEEE float */
    TK_STRING,      /* Pascal string[n] */
    TK_POINTER,     /* 32-bit pointer */
    TK_ARRAY,
    TK_RECORD,
    TK_SET,
    TK_FILE,
    TK_ENUM,
    TK_SUBRANGE,
    TK_PROC,        /* procedure type */
    TK_FUNC,        /* function type */
} type_kind_t;

/* Type descriptor */
typedef struct type_desc {
    type_kind_t kind;
    int size;               /* Size in bytes */
    char name[64];          /* Type name */

    /* For arrays */
    int array_low, array_high;
    struct type_desc *element_type;

    /* For records */
    struct {
        char name[64];
        char type_name[12];  /* Original type name for re-resolution (8-char significant) */
        int offset;          /* byte offset of the containing byte */
        struct type_desc *type;
        /* P82: variant record tracking.
         * variant_arm = 0 for fixed fields, 1..N for fields in variant arm N.
         * Pre-pass Phase-2 layout uses this to reset offset when crossing arm
         * boundaries so variant arms overlap (Pascal CASE-in-record semantics).
         * Without this, recomputed layouts lay arms out sequentially and
         * downstream field offsets in e.g. codesdb.freechain get wrong. */
        signed char variant_arm;
        /* P87d: bit-packed field tracking (only meaningful in packed records).
         * bit_width == 0 means "whole-byte field" — read/write the byte at
         * `offset` normally. bit_width > 0 means a bit-packed field at
         * bits [bit_offset .. bit_offset + bit_width - 1] of the byte at
         * `offset`. Used for Lisa Pascal's Tnibble (0..15, 4 bits) and
         * booleans in packed records — e.g. pmem's BootVol/NormCont share
         * byte 4 (bits 7-4 and 3-0 respectively). */
        unsigned char bit_offset;   /* LSB position within the byte, 0..7 */
        unsigned char bit_width;    /* 0 = whole-byte; 1..7 = bit-packed */
    } fields[64];
    int num_fields;
    /* P82: variant layout bookkeeping. variant_start is the offset at which
     * the variant region begins (i.e. first variant arm's first field offset).
     * Used during Phase-2 fixup to correctly overlap arms. 0 when no variants. */
    int variant_start;
    /* P87c: record was declared `packed`. Both the initial resolver and the
     * Phase-2 fixup in toolchain_bridge use this to skip word-alignment and
     * to pack booleans at 1 byte. Without it, pmem's DevConfig lands at
     * offset 22 (booleans word-sized, fields word-aligned) instead of 18,
     * and the DEFAULTPM init loop overruns the 64-byte param_mem backing
     * store, corrupting b_syslocal_ptr. */
    bool is_packed;

    /* For pointers */
    struct type_desc *base_type;
    char base_name[64];         /* Unresolved base type name (for forward refs) */

    /* For strings */
    int max_length;

    /* For subrange */
    int range_low, range_high;

    /* For sets */
    struct type_desc *set_base;
} type_desc_t;

/* Symbol in the code generator */
typedef struct {
    char name[64];
    type_desc_t *type;
    int offset;             /* Stack offset (locals) or global offset */
    bool is_global;
    bool is_param;
    bool is_var_param;      /* VAR parameter (passed by reference) */
    bool is_external;
    bool is_forward;
    bool is_const;          /* CONST declaration — offset holds value, not address */
} cg_symbol_t;

/* Local scope for variables */
typedef struct {
    cg_symbol_t locals[CODEGEN_MAX_LOCALS];
    int num_locals;
    int frame_size;         /* Total size of local variables */
    char proc_name[64];     /* Procedure this scope belongs to (for exit()) */
} cg_scope_t;

/* Procedure signature — tracks VAR params, sizes, and calling convention */
#define CODEGEN_MAX_PROC_SIGS 4096
#define CODEGEN_MAX_PARAMS 16
typedef struct {
    char name[64];
    int num_params;
    bool param_is_var[CODEGEN_MAX_PARAMS];  /* true = VAR parameter */
    int param_size[CODEGEN_MAX_PARAMS];     /* Parameter size in bytes (2 or 4) */
    char param_name[CODEGEN_MAX_PARAMS][64]; /* Parameter names for forward-decl matching */
    struct type_desc *param_type[CODEGEN_MAX_PARAMS]; /* Parameter types (for IMPL body reconstruction) */
    char param_type_name[CODEGEN_MAX_PARAMS][64]; /* P102: stable type name for call-site re-resolution */
    int  param_type_kind[CODEGEN_MAX_PARAMS]; /* P102: snapshot of type kind at registration */
    bool is_external;                       /* true = assembly/external (callee-clean) */
    bool is_function;                       /* true = function (returns value), false = procedure */
    /* P81 static-link ABI. nest_depth = Pascal lexical depth of the proc
     * (1 = top-level proc declared in a unit; 2 = proc nested in a depth-1
     * proc; …). Procs at depth>=2 need a static link pointing to their
     * static parent's frame. The caller passes this in A2 before JSR; the
     * callee copies A2 to -4(A6) in its prologue. takes_static_link is
     * true iff nest_depth>=2. */
    int nest_depth;
    bool takes_static_link;
} cg_proc_sig_t;

/* Relocation */
typedef struct {
    uint32_t offset;        /* Offset in code where relocation needed */
    char symbol[64];        /* Symbol name to resolve */
    int size;               /* 2 or 4 bytes */
    bool pc_relative;
} cg_reloc_t;

/* Main code generator state */
typedef struct {
    /* Output buffer */
    uint8_t code[CODEGEN_MAX_OUTPUT];
    uint32_t code_size;

    /* Global symbols */
    cg_symbol_t globals[CODEGEN_MAX_SYMBOLS];
    int num_globals;

    /* Types */
    type_desc_t types[CODEGEN_MAX_SYMBOLS];
    int num_types;

    /* Scope stack */
    cg_scope_t scopes[CODEGEN_MAX_SCOPE];
    int scope_depth;

    /* Relocations */
    cg_reloc_t relocs[CODEGEN_MAX_RELOCS];
    int num_relocs;

    /* String literals */
    struct {
        char value[256];
        uint32_t offset;    /* Offset in code where string data is */
    } strings[CODEGEN_MAX_STRINGS];
    int num_strings;

    /* Current segment name */
    char segment[64];

    /* Global data area size (A5-relative offsets for this module) */
    uint32_t global_data_size;

    /* Label counter for internal labels */
    int label_counter;

    /* Error tracking */
    int num_errors;
    char errors[100][256];
    char current_file[256];

    /* Imported globals from previously compiled units (set by toolchain bridge) */
    cg_symbol_t *imported_globals;
    int imported_globals_count;

    /* Imported types from previously compiled units (set by toolchain bridge) */
    type_desc_t *imported_types;
    int imported_types_count;

    /* Procedure signatures — shared across units for VAR parameter handling */
    cg_proc_sig_t *proc_sigs;
    int num_proc_sigs;

    /* Imported procedure signatures from previously compiled units */
    cg_proc_sig_t *imported_proc_sigs;
    int imported_proc_sigs_count;

    /* WITH statement context stack.
     * Each active WITH pushes the record expression's base address
     * and type so field names can be resolved implicitly. */
    struct {
        type_desc_t *record_type;   /* Record type for field lookup */
        ast_node_t  *record_expr;   /* AST node for the record expression */
    } with_stack[16];
    int with_depth;

    /* Per-procedure label map: numeric labels declared inside the current
     * procedure body. Reset by gen_proc_or_func at procedure entry.
     * `pending_gotos` stores forward-reference GOTOs that need patching
     * when the matching label is emitted (or at procedure end if unresolved). */
    struct { int label; uint32_t code_offset; } labels[128];
    int num_labels;
    struct { int label; uint32_t patch_offset; } pending_gotos[128];
    int num_pending_gotos;

    /* P48: nesting depth of PACKED record/array construction. When > 0,
     * 0..255 subrange fields pack to 1 byte; otherwise they default to
     * word-sized to match Lisa Pascal's real layout (needed so asm
     * code reading PRIORITY(A1) as CMP.W sees positive word values for
     * priorities 128..255). */
    int in_packed;
} codegen_t;

/* Public API */
void codegen_init(codegen_t *cg);
void codegen_free(codegen_t *cg);

/* Generate code from an AST */
bool codegen_generate(codegen_t *cg, ast_node_t *ast);

/* Get the generated code */
const uint8_t *codegen_get_code(codegen_t *cg, uint32_t *size);

/* Write output as Lisa object file */
bool codegen_write_obj(codegen_t *cg, const char *filename);

/* Error count */
int codegen_get_error_count(codegen_t *cg);

#endif /* PASCAL_CODEGEN_H */
