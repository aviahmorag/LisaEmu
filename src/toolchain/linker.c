/*
 * LisaEm Toolchain — Linker
 *
 * Links multiple .OBJ files into a single executable or library.
 * Resolves symbols across modules and applies relocations.
 */

#include "linker.h"
#include "pascal_codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* ========================================================================
 * Helpers
 * ======================================================================== */

static void link_error(linker_t *lk, const char *fmt, ...) {
    if (lk->num_errors >= 100) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(lk->errors[lk->num_errors], 256, fmt, args);
    va_end(args);
    fprintf(stderr, "linker: %s\n", lk->errors[lk->num_errors]);
    lk->num_errors++;
}

static bool str_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

/* ========================================================================
 * Symbol table
 * ======================================================================== */

static int find_global_symbol(linker_t *lk, const char *name) {
    /* Exact match (case-insensitive), preferring ENTRY over EXTERN */
    int extern_match = -1;
    for (int i = 0; i < lk->num_symbols; i++) {
        if (str_eq_nocase(lk->symbols[i].name, name)) {
            if (lk->symbols[i].type == LSYM_ENTRY)
                return i;  /* ENTRY always wins */
            if (extern_match < 0)
                extern_match = i;  /* Remember first EXTERN match */
        }
    }
    /* 8-char prefix match — Lisa assembler truncates symbols to 8 significant
     * characters. Match in both directions:
     * - Short ref (ENTER_SC) matches long def (ENTER_SCHEDULER): prefix of def
     * - Long ref (ENTER_SCHEDULER) matches short def (ENTER_SC): truncate ref to 8 */
    size_t len = strlen(name);
    size_t match_len = (len <= 8) ? len : 8;
    if (match_len >= 3) {
        for (int i = 0; i < lk->num_symbols; i++) {
            if (strncasecmp(lk->symbols[i].name, name, match_len) == 0 &&
                lk->symbols[i].type == LSYM_ENTRY)
                return i;
        }
    }
    return extern_match;  /* Fall back to EXTERN if no ENTRY found */
}

static int add_global_symbol(linker_t *lk, const char *name, link_sym_type_t type,
                              int32_t value, int module_idx) {
    int existing = find_global_symbol(lk, name);
    if (existing >= 0) {
        /* Update existing symbol — allow redefinition (last one wins).
         * In Lisa OS, the same method names (CREATE, Draw, etc.) appear in
         * different compilation units for different classes. */
        if (type == LSYM_ENTRY) {
            lk->symbols[existing].type = LSYM_ENTRY;
            lk->symbols[existing].value = value;
            lk->symbols[existing].module_idx = module_idx;
            lk->symbols[existing].resolved = true;
        }
        return existing;
    }

    if (lk->num_symbols >= LINK_MAX_SYMBOLS) {
        link_error(lk, "symbol table full");
        return -1;
    }

    int idx = lk->num_symbols++;
    strncpy(lk->symbols[idx].name, name, 63);
    lk->symbols[idx].type = type;
    lk->symbols[idx].value = value;
    lk->symbols[idx].module_idx = module_idx;
    lk->symbols[idx].resolved = (type == LSYM_ENTRY);
    return idx;
}

/* ========================================================================
 * OBJ file loading
 * ======================================================================== */

/* Read our LOBJ format:
 *   "LOBJ" magic (4 bytes)
 *   uint32 version
 *   uint32 code_size
 *   uint32 num_symbols
 *   code (code_size bytes)
 *   symbols (variable)
 *   uint32 num_relocs
 *   relocations (variable)
 */
bool linker_load_obj(linker_t *lk, const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        link_error(lk, "cannot open '%s'", filename);
        return false;
    }

    /* Read header */
    char magic[4];
    uint32_t version, code_size, num_syms;
    fread(magic, 1, 4, f);
    if (memcmp(magic, "LOBJ", 4) != 0) {
        link_error(lk, "'%s' is not a valid OBJ file", filename);
        fclose(f);
        return false;
    }
    fread(&version, 4, 1, f);
    fread(&code_size, 4, 1, f);
    fread(&num_syms, 4, 1, f);

    /* Allocate module */
    link_module_t *mod = calloc(1, sizeof(link_module_t));
    if (!mod) { fclose(f); return false; }
    strncpy(mod->filename, filename, sizeof(mod->filename) - 1);

    /* Extract module name from filename */
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    strncpy(mod->name, base, sizeof(mod->name) - 1);
    char *dot = strchr(mod->name, '.');
    if (dot) *dot = '\0';

    /* Read code */
    mod->code = malloc(code_size);
    if (mod->code) {
        fread(mod->code, 1, code_size, f);
        mod->code_size = code_size;
    }

    /* Read symbols */
    for (uint32_t i = 0; i < num_syms && mod->num_symbols < 4096; i++) {
        uint8_t type, flags, namelen;
        int32_t value;
        fread(&type, 1, 1, f);
        fread(&flags, 1, 1, f);
        fread(&namelen, 1, 1, f);
        char name[64] = "";
        if (namelen > 63) namelen = 63;
        fread(name, 1, namelen, f);
        name[namelen] = '\0';
        fread(&value, 4, 1, f);

        int si = mod->num_symbols++;
        strncpy(mod->symbols[si].name, name, 63);
        mod->symbols[si].type = type;
        mod->symbols[si].value = value;
    }

    /* Read relocations */
    uint32_t num_relocs;
    fread(&num_relocs, 4, 1, f);
    for (uint32_t i = 0; i < num_relocs && mod->num_relocs < 4096; i++) {
        uint32_t offset;
        int32_t size;
        uint8_t namelen;
        fread(&offset, 4, 1, f);
        fread(&size, 4, 1, f);
        fread(&namelen, 1, 1, f);
        char name[64] = "";
        if (namelen > 63) namelen = 63;
        fread(name, 1, namelen, f);
        name[namelen] = '\0';

        int ri = mod->num_relocs++;
        mod->relocs[ri].offset = offset;
        strncpy(mod->relocs[ri].symbol, name, 63);
        mod->relocs[ri].size = size;
    }

    fclose(f);

    /* Add to linker */
    if (lk->num_modules >= LINK_MAX_OBJECTS) {
        link_error(lk, "too many modules");
        free(mod->code);
        free(mod);
        return false;
    }

    int mod_idx = lk->num_modules++;
    lk->modules[mod_idx] = mod;

    /* Register symbols in global table */
    for (int i = 0; i < mod->num_symbols; i++) {
        link_sym_type_t st = (mod->symbols[i].type == 1) ? LSYM_EXTERN : LSYM_ENTRY;
        add_global_symbol(lk, mod->symbols[i].name, st, mod->symbols[i].value, mod_idx);
    }

    return true;
}

/* Load directly from codegen output */
bool linker_load_codegen(linker_t *lk, const char *name,
                         const uint8_t *code, uint32_t code_size,
                         const void *sym_data, int num_symbols,
                         const void *rel_data, int num_relocs) {
    link_module_t *mod = calloc(1, sizeof(link_module_t));
    if (!mod) return false;

    strncpy(mod->name, name, sizeof(mod->name) - 1);
    strncpy(mod->filename, name, sizeof(mod->filename) - 1);

    /* Copy code */
    mod->code = malloc(code_size);
    if (mod->code && code_size > 0) {
        memcpy(mod->code, code, code_size);
        mod->code_size = code_size;
    }

    /* Copy symbols from codegen format */
    const cg_symbol_t *cg_syms = (const cg_symbol_t *)sym_data;
    for (int i = 0; i < num_symbols && mod->num_symbols < 4096; i++) {
        if (!cg_syms[i].name[0]) continue;
        int si = mod->num_symbols++;
        strncpy(mod->symbols[si].name, cg_syms[i].name, 63);
        mod->symbols[si].type = cg_syms[i].is_external ? 1 : 0;
        mod->symbols[si].value = cg_syms[i].offset;
    }

    /* Copy relocations from codegen format */
    const cg_reloc_t *cg_rels = (const cg_reloc_t *)rel_data;
    for (int i = 0; i < num_relocs && mod->num_relocs < 4096; i++) {
        int ri = mod->num_relocs++;
        mod->relocs[ri].offset = cg_rels[i].offset;
        strncpy(mod->relocs[ri].symbol, cg_rels[i].symbol, 63);
        mod->relocs[ri].size = cg_rels[i].size;
    }

    if (lk->num_modules >= LINK_MAX_OBJECTS) {
        link_error(lk, "too many modules");
        free(mod->code);
        free(mod);
        return false;
    }

    int mod_idx = lk->num_modules++;
    lk->modules[mod_idx] = mod;

    /* Register symbols */
    for (int i = 0; i < mod->num_symbols; i++) {
        link_sym_type_t st = (mod->symbols[i].type == 1) ? LSYM_EXTERN : LSYM_ENTRY;
        add_global_symbol(lk, mod->symbols[i].name, st, mod->symbols[i].value, mod_idx);
    }

    return true;
}

/* ========================================================================
 * Layout and linking
 * ======================================================================== */

bool linker_link(linker_t *lk) {
    if (lk->num_modules == 0) {
        link_error(lk, "no modules to link");
        return false;
    }

    /* Phase 1: Layout — assign base addresses to each module.
     * Start at $400 to leave room for the 68000 exception vector table ($0-$3FF).
     * The emulator sets up proper vectors in the $0-$3FF area. */
    uint32_t current_addr = 0x400;

    /* Create a single segment for now */
    if (lk->num_segments == 0) {
        lk->num_segments = 1;
        strncpy(lk->segments[0].name, "CODE", 63);
        lk->segments[0].base_addr = 0;
        lk->segments[0].module_start = 0;
        lk->segments[0].module_count = lk->num_modules;
    }

    for (int i = 0; i < lk->num_modules; i++) {
        link_module_t *mod = lk->modules[i];
        /* Word-align */
        if (current_addr & 1) current_addr++;
        mod->base_addr = current_addr;
        current_addr += mod->code_size;
    }

    lk->segments[0].size = current_addr;

    /* Phase 2: Resolve symbols — update entry point addresses with base offsets */
    for (int i = 0; i < lk->num_symbols; i++) {
        link_symbol_t *sym = &lk->symbols[i];
        if (sym->type == LSYM_ENTRY && sym->module_idx >= 0 &&
            sym->module_idx < lk->num_modules) {
            sym->value += lk->modules[sym->module_idx]->base_addr;
            sym->resolved = true;
        }
    }

    /* Phase 3: Apply relocations */
    for (int m = 0; m < lk->num_modules; m++) {
        link_module_t *mod = lk->modules[m];
        for (int r = 0; r < mod->num_relocs; r++) {
            const char *sym_name = mod->relocs[r].symbol;
            uint32_t offset = mod->relocs[r].offset;
            int size = mod->relocs[r].size;

            int sym_idx = find_global_symbol(lk, sym_name);
            if (sym_idx < 0 || !lk->symbols[sym_idx].resolved) {
                link_error(lk, "unresolved symbol '%s' in module '%s'",
                          sym_name, mod->name);
                continue;
            }

            int32_t target = lk->symbols[sym_idx].value;

            /* Patch the code */
            uint32_t abs_offset = mod->base_addr + offset;
            if (offset < mod->code_size) {
                if (size == 4) {
                    mod->code[offset]     = (target >> 24) & 0xFF;
                    mod->code[offset + 1] = (target >> 16) & 0xFF;
                    mod->code[offset + 2] = (target >> 8)  & 0xFF;
                    mod->code[offset + 3] = target & 0xFF;
                } else if (size == 2) {
                    mod->code[offset]     = (target >> 8) & 0xFF;
                    mod->code[offset + 1] = target & 0xFF;
                }
            }
            (void)abs_offset;
        }
    }

    /* Phase 4: Build output — concatenate all module code + stub area */
    uint32_t total_size = 0;
    for (int i = 0; i < lk->num_modules; i++) {
        total_size += lk->modules[i]->code_size;
        if (total_size & 1) total_size++; /* word-align */
    }

    /* Add a stub function at the end for unresolved symbols.
     * The stub just does RTS (return to caller) so the OS doesn't crash. */
    uint32_t stub_addr = total_size;
    if (stub_addr & 1) stub_addr++;
    total_size = stub_addr + 4;  /* Room for CLR.L D0 + RTS */

    lk->output = malloc(total_size);
    if (!lk->output) {
        link_error(lk, "cannot allocate %u bytes for output", total_size);
        return false;
    }
    lk->output_size = total_size;
    lk->output_capacity = total_size;

    memset(lk->output, 0, total_size);

    /* Write stub: CLR.L D0; RTS — returns 0 for any unresolved function */
    lk->output[stub_addr]     = 0x42; /* CLR.L D0 = $4280 */
    lk->output[stub_addr + 1] = 0x80;
    lk->output[stub_addr + 2] = 0x4E; /* RTS = $4E75 */
    lk->output[stub_addr + 3] = 0x75;

    /* Resolve all unresolved externals to point to the stub */
    for (int i = 0; i < lk->num_symbols; i++) {
        if (lk->symbols[i].type == LSYM_EXTERN && !lk->symbols[i].resolved) {
            lk->symbols[i].value = stub_addr;
            lk->symbols[i].resolved = true;
        }
    }

    /* Re-apply relocations for newly resolved symbols */
    for (int m = 0; m < lk->num_modules; m++) {
        link_module_t *mod = lk->modules[m];
        for (int r = 0; r < mod->num_relocs; r++) {
            int sym_idx = find_global_symbol(lk, mod->relocs[r].symbol);
            if (sym_idx >= 0 && lk->symbols[sym_idx].resolved) {
                uint32_t offset = mod->relocs[r].offset;
                int size = mod->relocs[r].size;
                int32_t target = lk->symbols[sym_idx].value;
                if (offset < mod->code_size) {
                    if (size == 4) {
                        mod->code[offset]     = (target >> 24) & 0xFF;
                        mod->code[offset + 1] = (target >> 16) & 0xFF;
                        mod->code[offset + 2] = (target >> 8)  & 0xFF;
                        mod->code[offset + 3] = target & 0xFF;
                    } else if (size == 2) {
                        mod->code[offset]     = (target >> 8) & 0xFF;
                        mod->code[offset + 1] = target & 0xFF;
                    }
                }
            }
        }
    }
    for (int i = 0; i < lk->num_modules; i++) {
        link_module_t *mod = lk->modules[i];
        if (mod->code && mod->code_size > 0) {
            memcpy(lk->output + mod->base_addr, mod->code, mod->code_size);
        }
    }

    /* Check for unresolved externals and dump debug info */
    int unresolved = 0;
    for (int i = 0; i < lk->num_symbols; i++) {
        if (!lk->symbols[i].resolved) {
            link_error(lk, "unresolved external: '%s' (type=%d)",
                      lk->symbols[i].name, lk->symbols[i].type);
            unresolved++;
        }
    }
    /* Dump symbol table stats */
    int entries = 0, externs = 0, resolved = 0;
    for (int i = 0; i < lk->num_symbols; i++) {
        if (lk->symbols[i].type == LSYM_ENTRY) entries++;
        else externs++;
        if (lk->symbols[i].resolved) resolved++;
    }
    fprintf(stderr, "Linker: %d symbols total (%d entries, %d externs), %d resolved, %d unresolved\n",
            lk->num_symbols, entries, externs, resolved, unresolved);
    /* Search for specific symbols to debug */
    const char *debug_syms[] = {"extractkey", "EXTRACTKEY", "EXTRACTK", "retry_compact", "prefix_name", "ENTER_SCHEDULER", "ENTER_SC", "HAllocate", "SchedEnable", "SCHEDENA", NULL};
    for (int d = 0; debug_syms[d]; d++) {
        int idx = find_global_symbol(lk, debug_syms[d]);
        if (idx >= 0) {
            fprintf(stderr, "  DEBUG: '%s' found as '%s' type=%d resolved=%d val=$%X mod=%d\n",
                    debug_syms[d], lk->symbols[idx].name, lk->symbols[idx].type,
                    lk->symbols[idx].resolved, lk->symbols[idx].value, lk->symbols[idx].module_idx);
        } else {
            fprintf(stderr, "  DEBUG: '%s' NOT FOUND in symbol table\n", debug_syms[d]);
        }
    }

    return lk->num_errors == 0;
}

/* ========================================================================
 * Output
 * ======================================================================== */

bool linker_write_binary(linker_t *lk, const char *filename) {
    if (!lk->output || lk->output_size == 0) {
        link_error(lk, "no output to write");
        return false;
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        link_error(lk, "cannot create '%s'", filename);
        return false;
    }

    fwrite(lk->output, 1, lk->output_size, f);
    fclose(f);

    printf("Linked: %s (%u bytes, %d modules, %d symbols)\n",
           filename, lk->output_size, lk->num_modules, lk->num_symbols);
    return true;
}

bool linker_write_lib(linker_t *lk, const char *filename) {
    /* Intrinsic library format: header + directory + segments
     * For now, write the same format as a binary with a library header */
    if (!lk->output) return false;

    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    /* Write a simple library header */
    fwrite("LLIB", 1, 4, f);  /* Lisa LIBrary magic */
    uint32_t version = 1;
    fwrite(&version, 4, 1, f);
    uint32_t num_entries = 0;
    for (int i = 0; i < lk->num_symbols; i++) {
        if (lk->symbols[i].type == LSYM_ENTRY) num_entries++;
    }
    fwrite(&num_entries, 4, 1, f);
    fwrite(&lk->output_size, 4, 1, f);

    /* Write entry directory */
    for (int i = 0; i < lk->num_symbols; i++) {
        if (lk->symbols[i].type == LSYM_ENTRY) {
            uint8_t namelen = (uint8_t)strlen(lk->symbols[i].name);
            fwrite(&namelen, 1, 1, f);
            fwrite(lk->symbols[i].name, 1, namelen, f);
            fwrite(&lk->symbols[i].value, 4, 1, f);
        }
    }

    /* Write code */
    fwrite(lk->output, 1, lk->output_size, f);
    fclose(f);

    printf("Library: %s (%u bytes, %u entries)\n",
           filename, lk->output_size, num_entries);
    return true;
}

const uint8_t *linker_get_output(linker_t *lk, uint32_t *size) {
    if (size) *size = lk->output_size;
    return lk->output;
}

int linker_get_error_count(linker_t *lk) {
    return lk->num_errors;
}

void linker_print_linkmap(linker_t *lk) {
    printf("=== Link Map ===\n");
    printf("Input summary:\n");
    printf("  %d Modules\n", lk->num_modules);
    printf("  %d Symbols\n", lk->num_symbols);
    printf("\n");

    for (int i = 0; i < lk->num_modules; i++) {
        printf("Module: %-20s  base=%06X  size=%u\n",
               lk->modules[i]->name, lk->modules[i]->base_addr,
               lk->modules[i]->code_size);
    }
    printf("\n");

    int entries = 0;
    for (int i = 0; i < lk->num_symbols; i++) {
        if (lk->symbols[i].type == LSYM_ENTRY && lk->symbols[i].resolved) {
            printf("  %-8s : %-16s at %08X\n",
                   lk->modules[lk->symbols[i].module_idx]->name,
                   lk->symbols[i].name,
                   (uint32_t)lk->symbols[i].value);
            entries++;
        }
    }
    printf("\nTotal: %u bytes code, %d entry points\n", lk->output_size, entries);
}

/* ========================================================================
 * Init / Free
 * ======================================================================== */

void linker_init(linker_t *lk) {
    memset(lk, 0, sizeof(linker_t));

    /* Register Pascal builtin symbols as resolved intrinsics.
     * These are compiled inline by the real Lisa Pascal compiler,
     * but our codegen emits calls that need resolution.
     * Value 0 = stub (the codegen already generated inline code). */
    static const char *builtins[] = {
        "ABS", "ORD", "ORD4", "CHR", "ODD", "SUCC", "PRED",
        "SIZEOF", "POINTER", "EXIT", "HALT",
        "LENGTH", "COPY", "CONCAT", "POS", "DELETE", "INSERT",
        "ROUND", "TRUNC", "SQR", "SQRT",
        "WRITE", "WRITELN", "READ", "READLN",
        "NEW", "DISPOSE", "MARK", "RELEASE",
        "MOVELEFT", "MOVERIGHT", "FILLCHAR", "SCANEQ", "SCANNE",
        "BLOCKREAD", "BLOCKWRITE", "UNITREAD", "UNITWRITE",
        "IORESULT", "UNITSTATUS", "UNITCLEAR", "UNITBUSY",
        "TIME", "KEYPRESS",
        "TpLONGINT", "LIntMulInt", "LIntDivLInt", "DistToLDist",
        "LRectToRect", "LPtToPt", "LPatToPat",
        /* QuickDraw basics */
        "SetPt", "SetRect", "EqualRect", "EmptyRect",
        "PenNormal", "PenMode", "PenSize", "PenPat",
        "MoveTo", "LineTo", "Move", "Line",
        "FrameRect", "PaintRect", "EraseRect", "InvertRect",
        "FrameRgn", "PaintRgn", "EraseRgn", "InvertRgn",
        "RectRgn", "DiffRgn", "SectRgn", "UnionRgn", "OffsetRgn",
        "ClipRect", "SetClip", "GetClip",
        "OpenPort", "ClosePort", "GetPort", "SetPort",
        "DrawString", "StringWidth", "TextFont", "TextFace", "TextSize",
        "GetFontInfo", "CharWidth",
        "NewRgn", "DisposeRgn", "CopyRgn",
        "SetPen", "GetPen",
        /* Toolkit */
        "InsFirst", "InsLast", "Scan", "ClipFurtherTo",
        "NewBreakLocation", "DistinguishScreenFeedback", "DoOnAPage",
        "StrUpperCased",
        NULL
    };
    for (int i = 0; builtins[i]; i++) {
        add_global_symbol(lk, builtins[i], LSYM_ENTRY, 0, -1);
    }
}

void linker_free(linker_t *lk) {
    for (int i = 0; i < lk->num_modules; i++) {
        if (lk->modules[i]) {
            free(lk->modules[i]->code);
            free(lk->modules[i]);
        }
    }
    free(lk->output);
    memset(lk, 0, sizeof(linker_t));
}
