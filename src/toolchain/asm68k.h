/*
 * LisaEm Toolchain — 68000 Cross-Assembler
 *
 * Assembles Lisa-style 68000 assembly into Lisa object format.
 *
 * Syntax supported (from Lisa_Source analysis):
 *   Directives: .PROC .FUNC .DEF .REF .INCLUDE .EQU .MACRO .ENDM
 *               .IF .ENDC .ELSE .BYTE .WORD .LONG .ASCII .BLOCK
 *   Labels:     name (at start of line) or name: with colon
 *   Comments:   ; to end of line
 *   Instructions: Full MC68000 instruction set, Motorola syntax
 *   Addressing:   Dn, An, (An), (An)+, -(An), d(An), d(An,Xn),
 *                 abs.W, abs.L, d(PC), d(PC,Xn), #imm
 */

#ifndef ASM68K_H
#define ASM68K_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Limits */
#define ASM_MAX_SYMBOLS     8192
#define ASM_MAX_LINE        1024
#define ASM_MAX_LABEL       64
#define ASM_MAX_SECTIONS    64
#define ASM_MAX_INCLUDES    16
#define ASM_MAX_MACROS      512
#define ASM_MAX_MACRO_LINES 256
#define ASM_MAX_OUTPUT      (512 * 1024)  /* 512KB max per file */
#define ASM_MAX_RELOCS      4096
#define ASM_MAX_ERRORS      100

/* Symbol types */
typedef enum {
    SYM_LABEL,      /* Code/data label (address) */
    SYM_EQU,        /* Equate (constant value) */
    SYM_DEF,        /* Exported (.DEF) symbol */
    SYM_REF,        /* External (.REF) reference */
    SYM_PROC,       /* Procedure entry (.PROC) */
    SYM_FUNC,       /* Function entry (.FUNC) */
    SYM_MACRO,      /* Macro name */
} sym_type_t;

/* Symbol entry */
typedef struct {
    char name[ASM_MAX_LABEL];
    sym_type_t type;
    int32_t value;
    bool defined;
    bool exported;      /* .DEF */
    bool external;      /* .REF */
    int section;        /* Which section this belongs to */
} asm_symbol_t;

/* Relocation entry */
typedef struct {
    uint32_t offset;    /* Offset in output where relocation applies */
    int symbol_idx;     /* Symbol index for external refs */
    int size;           /* 1=byte, 2=word, 4=long */
    bool pc_relative;
} asm_reloc_t;

/* Section (segment) */
typedef struct {
    char name[ASM_MAX_LABEL];
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
    uint32_t origin;    /* Base address for section */
} asm_section_t;

/* Macro definition */
typedef struct {
    char name[ASM_MAX_LABEL];
    char *lines[ASM_MAX_MACRO_LINES];
    int line_count;
    int param_count;
} asm_macro_t;

/* Conditional assembly state */
typedef struct {
    bool active;        /* Currently assembling? */
    bool had_true;      /* Has the condition been true? */
    int depth;          /* Nesting depth */
} asm_cond_t;

#define ASM_MAX_COND_DEPTH 16

/* Error entry */
typedef struct {
    int line;
    char filename[256];
    char message[256];
} asm_error_t;

/* Main assembler state */
typedef struct {
    /* Symbol table */
    asm_symbol_t symbols[ASM_MAX_SYMBOLS];
    int num_symbols;

    /* Sections */
    asm_section_t sections[ASM_MAX_SECTIONS];
    int num_sections;
    int current_section;

    /* Relocations */
    asm_reloc_t relocs[ASM_MAX_RELOCS];
    int num_relocs;

    /* Macros */
    asm_macro_t macros[ASM_MAX_MACROS];
    int num_macros;
    bool in_macro_def;
    int current_macro;

    /* Conditional assembly */
    asm_cond_t cond_stack[ASM_MAX_COND_DEPTH];
    int cond_depth;

    /* Assembly state */
    int pass;           /* 1 or 2 */
    uint32_t pc;        /* Current program counter */
    int line_num;
    char current_file[256];
    int local_scope;    /* Scope counter for @-labels (increments at each major label) */

    /* Include path for .INCLUDE resolution */
    char include_paths[ASM_MAX_INCLUDES][256];
    int num_include_paths;
    char base_dir[256]; /* Base directory of source tree */

    /* Errors */
    asm_error_t errors[ASM_MAX_ERRORS];
    int num_errors;

    /* Output */
    uint8_t *output;
    uint32_t output_size;
    uint32_t output_capacity;
} asm68k_t;

/* Public API */

/* Initialize assembler state */
void asm68k_init(asm68k_t *as);

/* Free assembler state */
void asm68k_free(asm68k_t *as);

/* Set the base directory for resolving .INCLUDE paths */
void asm68k_set_base_dir(asm68k_t *as, const char *dir);

/* Add an include search path */
void asm68k_add_include_path(asm68k_t *as, const char *path);

/* Assemble a source file. Returns true on success. */
bool asm68k_assemble_file(asm68k_t *as, const char *filename);

/* Assemble from a string buffer. Returns true on success. */
bool asm68k_assemble_string(asm68k_t *as, const char *source, const char *filename);

/* Get assembled output */
const uint8_t *asm68k_get_output(asm68k_t *as, uint32_t *size);

/* Get error count */
int asm68k_get_error_count(asm68k_t *as);

/* Get error message by index */
const char *asm68k_get_error(asm68k_t *as, int index);

/* Write output to Lisa object file format */
bool asm68k_write_obj(asm68k_t *as, const char *filename);

/* Debug: print symbol table */
void asm68k_dump_symbols(asm68k_t *as);

#endif /* ASM68K_H */
