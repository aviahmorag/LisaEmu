/*
 * LisaEm Toolchain — Linker
 *
 * Links multiple .OBJ files into executables.
 * Handles:
 *   - Reading Lisa OBJ files (our LOBJ format)
 *   - Symbol resolution across modules
 *   - Relocation patching
 *   - Segment layout
 *   - Output as linked binary or intrinsic library
 */

#ifndef LINKER_H
#define LINKER_H

#include <stdint.h>
#include <stdbool.h>

/* Limits */
#define LINK_MAX_OBJECTS    2048
#define LINK_MAX_SEGMENTS   128
#define LINK_MAX_SYMBOLS    16384
#define LINK_MAX_RELOCS     65536
#define LINK_MAX_OUTPUT     (2 * 1024 * 1024)  /* 2MB max output */

/* Symbol types in the linker */
typedef enum {
    LSYM_LOCAL,      /* Local symbol (not visible outside module) */
    LSYM_ENTRY,      /* Entry point (exported from module) */
    LSYM_EXTERN,     /* External reference (imported) */
} link_sym_type_t;

/* Symbol in the linker's global table */
typedef struct {
    char name[64];
    link_sym_type_t type;
    int32_t value;           /* Resolved address */
    int module_idx;          /* Which module defines this */
    int segment_idx;         /* Which segment */
    bool resolved;
} link_symbol_t;

/* A loaded object module */
typedef struct {
    char filename[256];
    char name[64];           /* Module name (from .PROC/.FUNC or unit name) */
    uint8_t *code;
    uint32_t code_size;
    bool is_kernel;          /* True = include in system.os output */

    /* Symbols from this module */
    struct {
        char name[64];
        uint8_t type;        /* 0=entry, 1=external */
        int32_t value;       /* Offset within module's code */
    } symbols[16384];
    int num_symbols;

    /* Relocations */
    struct {
        uint32_t offset;     /* Offset in code */
        char symbol[64];     /* Symbol name to resolve */
        int size;            /* 2 or 4 bytes */
    } relocs[16384];
    int num_relocs;

    /* Assigned base address after layout */
    uint32_t base_addr;
    int segment;             /* Which segment this belongs to */
} link_module_t;

/* Segment descriptor */
typedef struct {
    char name[64];
    uint32_t base_addr;      /* Start address of segment */
    uint32_t size;           /* Total size */
    int module_start;        /* First module index in this segment */
    int module_count;        /* Number of modules */
} link_segment_t;

/* Linker state */
typedef struct {
    /* Loaded modules */
    link_module_t *modules[LINK_MAX_OBJECTS];
    int num_modules;

    /* Segments */
    link_segment_t segments[LINK_MAX_SEGMENTS];
    int num_segments;

    /* Global symbol table */
    link_symbol_t symbols[LINK_MAX_SYMBOLS];
    int num_symbols;

    /* Output buffer */
    uint8_t *output;
    uint32_t output_size;
    uint32_t output_capacity;

    /* Global data area */
    uint32_t global_data_size;

    /* Error tracking */
    int num_errors;
    char errors[100][256];
} linker_t;

/* Public API */
void linker_init(linker_t *lk);
void linker_free(linker_t *lk);

/* Load an OBJ file into the linker */
bool linker_load_obj(linker_t *lk, const char *filename);

/* Load from codegen output directly (in-memory) */
bool linker_load_codegen(linker_t *lk, const char *name,
                         const uint8_t *code, uint32_t code_size,
                         const void *symbols, int num_symbols,
                         const void *relocs, int num_relocs);

/* Perform linking: resolve symbols and apply relocations */
bool linker_link(linker_t *lk);

/* Write output to a binary file */
bool linker_write_binary(linker_t *lk, const char *filename);

/* Write output as a Lisa intrinsic library (.lib) */
bool linker_write_lib(linker_t *lk, const char *filename);

/* Get linked output */
const uint8_t *linker_get_output(linker_t *lk, uint32_t *size);

/* Error count */
int linker_get_error_count(linker_t *lk);

/* Print linkmap (for comparison with reference linkmaps) */
void linker_print_linkmap(linker_t *lk);

#endif /* LINKER_H */
