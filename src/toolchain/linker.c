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
    /* P128k: dot-mangled names are private to the module that emitted
     * them (nested Pascal procedures use `Parent.Child` mangling to
     * disambiguate same-named nested procs across different parents,
     * e.g. MAKE_DATASEG.Recover vs Get_Resources.Recover). Such names
     * must only match via EXACT strcasecmp — never via the 8-char
     * prefix fallback below, which would prefix-match them against
     * the parent's unmangled entry (e.g. "Get_Resources.Recover" would
     * LCP-match "Get_Resources" and steal the resolution). If exact
     * match above failed for a dot-mangled name, the reference is
     * genuinely unresolvable; return -1. */
    if (strchr(name, '.') != NULL) return -1;

    /* 8-char prefix match — Lisa assembler truncates symbols to 8 significant
     * characters. Apple's rule: two identifiers are the same iff their
     * first-8-char truncations are equal. Short names (< 8 chars) are
     * only equal to other names via exact strcasecmp — never to a longer
     * name's first-N-char prefix.
     *
     * P102b: when multiple ENTRY symbols share the first 8 chars with the
     * reference (e.g. STARTUP calls INIT_TWIGGGLOB which 8-prefix-matches
     * BOTH INIT_TWIGGLOB and INIT_TWIG_TABLE), pick the one whose full
     * name has the longest case-insensitive common prefix with the
     * reference. INIT_TWIGGGLOB shares 10 chars with INIT_TWIGGLOB but
     * only 9 with INIT_TWIG_TABLE, so the Pascal proc wins over the asm.
     *
     * P102c: require BOTH the reference and the candidate to be >= 8
     * chars before the prefix match applies. Without this, a WITH-field
     * identifier like `Dimcont` (7 chars, appearing in SET_PREFERENCES'
     * `SETDIMCONTRAST(DimConert(Dimcont))`) LCP-matches `DimContrast`
     * (11 chars, a LIBHW global) — which injects a spurious JSR into
     * the compiled call site and corrupts the sysglobal free list
     * downstream. `Dimcont` is NOT the 8-char-significance sibling of
     * `DimContrast` (7 vs 8 significant chars — Apple would treat them
     * as distinct identifiers). */
    size_t len = strlen(name);
    if (len >= 8) {
        int best_i = -1;
        size_t best_lcp = 0;
        for (int i = 0; i < lk->num_symbols; i++) {
            if (lk->symbols[i].type != LSYM_ENTRY) continue;
            if (strlen(lk->symbols[i].name) < 8) continue;
            if (strncasecmp(lk->symbols[i].name, name, 8) != 0) continue;
            const char *a = lk->symbols[i].name;
            const char *b = name;
            size_t lcp = 0;
            while (a[lcp] && b[lcp] &&
                   toupper((unsigned char)a[lcp]) == toupper((unsigned char)b[lcp]))
                lcp++;
            if (best_i < 0 || lcp > best_lcp) { best_i = i; best_lcp = lcp; }
        }
        if (best_i >= 0) return best_i;
    }
    /* Lisa Pascal compiler intrinsic name mappings.
     * The compiler emits human-readable names but the runtime uses
     * mangled %_ prefixed names. */
    static const struct { const char *from; const char *to; } name_map[] = {
        /* Compiler intrinsics → runtime */
        {"InClass",   "%_InObCN"},
        {"HALT",      "%_HALT"},
        {"WRITELN",   "%_WriteLn"},
        {"READLN",    "%_ReadLn"},
        {"NEW",       "%_New"},
        {"DISPOSE",   "%_Dispose"},
        /* FPLIB 3-operand extended wrappers → NEWFPSUB 2-operand */
        {"fpaddx",    "%f_ADD"},
        {"fpsubx",    "%f_SUB"},
        {"fpmulx",    "%f_MUL"},
        {"fpdivx",    "%f_DIV"},
        {"fpcomx",    "%f_EQ"},
        {"fpmovex",   "FP68K"},     /* move extended via FP dispatch */
        {"fpnegx",    "%f_neg"},
        {"fpabsx",    "%f_abs"},
        {"fpremx",    "FP68K"},     /* remainder via FP dispatch */
        {"fpintx",    "FP68K"},     /* integral part via FP dispatch */
        {"fpsqrtx",   "%_SQRT"},
        /* FPLIB register-based ops (no args, operate on fpcb) */
        {"fpadd",     "%f_ADD"},
        {"fpmul",     "%f_MUL"},
        {"fpdiv",     "%f_DIV"},
        {"fpint",     "FP68K"},
        {"fpsqrt",    "%_SQRT"},
        /* FPLIB move/convert functions */
        {"xmovefp",   "FP68K"},     /* extended → FP register */
        {"xmovefp1",  "FP68K"},     /* extended → FP1 register */
        {"smovex",    "FP68K"},     /* single → extended */
        {"wmovex",    "%I_FLT"},    /* integer → extended (float int) */
        {"dmovex",    "FP68K"},     /* double → extended */
        {"cmovefp",   "FP68K"},     /* BCD → FP register */
        {"xmoves",    "FP68K"},     /* extended → single */
        {"xmovew",    "%_trunc"},   /* extended → integer (truncate) */
        {"xmoved",    "FP68K"},     /* extended → double */
        {"xmovel",    "%_trunc"},   /* extended → longint */
        {"wmovefp1",  "FP68K"},     /* integer → FP1 */
        {"fpmoved",   "FP68K"},     /* FP0 → double */
        /* FPLIB internal helpers */
        {"fp%normalize",  "FP68K"},
        {"fp%cnormalize", "FP68K"},
        {"fp%execute",    "FP68K"},
        {"fp%hex",        "FP68K"},
        {"fp%i64",        "FP68K"},
        {"i64neg",        "%f_neg"},
        {"f32_minus",     "%f_neg"},
        {"f32_kind",      "FP68K"},
        {"f32_integral",  "FP68K"},
        {"f32_fraction",  "FP68K"},
        {"x80_break",     "FP68K"},
        {"x80_integral",  "FP68K"},
        {"x80%paddr",     "FP68K"},
        {"x80%todec",     "FP68K"},
        {"x48%todec",     "FP68K"},
        {"str2dec",       "FP68K"},
        /* X48 internal arithmetic (48-bit packed decimal) */
        {"x%pot",     "FP68K"},
        {"x%int",     "FP68K"},
        {"x%tos",     "FP68K"},
        {"x%sto",     "FP68K"},
        {"x%mul",     "FP68K"},
        {"x%div",     "FP68K"},
        {"x%dec",     "FP68K"},
        {"x%comp",    "FP68K"},
        /* MATHLIB fetch/store helpers */
        {"afetch",    "FP68K"},
        {"bfetch",    "FP68K"},
        {"xfetch",    "FP68K"},
        {"xstore",    "FP68K"},
        {"bstore",    "FP68K"},
        {"rstore",    "FP68K"},
        {"payment",   "FP68K"},     /* financial function */
        /* Math functions */
        {"SINx",      "%_SIN"},
        {"COSx",      "%_COS"},
        {"EXPx",      "%_EXP"},
        {"LNx",       "%_LN"},
        {"ATANx",     "%_ATAN"},
        {NULL, NULL}
    };
    for (int m = 0; name_map[m].from; m++) {
        if (strcasecmp(name, name_map[m].from) == 0) {
            int mapped = find_global_symbol(lk, name_map[m].to);
            if (mapped >= 0 && lk->symbols[mapped].type == LSYM_ENTRY)
                return mapped;
        }
    }

    /* Clascal method match: "AddImage" → "TDialogImage.AddImage"
     * When a method call uses just the method name, try to find an ENTRY
     * whose name ends with ".MethodName" */
    if (len >= 3) {
        char suffix[70];
        snprintf(suffix, sizeof(suffix), ".%s", name);
        size_t slen = strlen(suffix);
        int method_match = -1;
        for (int i = 0; i < lk->num_symbols; i++) {
            if (lk->symbols[i].type != LSYM_ENTRY) continue;
            size_t symlen = strlen(lk->symbols[i].name);
            if (symlen > slen &&
                strncasecmp(lk->symbols[i].name + symlen - slen, suffix, slen) == 0) {
                method_match = i;
                break;  /* Take first match — imperfect but functional */
            }
        }
        if (method_match >= 0) return method_match;
    }
    return extern_match;  /* Fall back to EXTERN if no ENTRY found */
}

/* Strict lookup: exact case-insensitive match only. Used for add_global_symbol
 * to avoid the 8-char prefix / method suffix heuristics destroying symbols. */
static int find_global_symbol_exact(linker_t *lk, const char *name) {
    int extern_match = -1;
    for (int i = 0; i < lk->num_symbols; i++) {
        if (str_eq_nocase(lk->symbols[i].name, name)) {
            if (lk->symbols[i].type == LSYM_ENTRY)
                return i;
            if (extern_match < 0)
                extern_match = i;
        }
    }
    return extern_match;
}

static int add_global_symbol(linker_t *lk, const char *name, link_sym_type_t type,
                              int32_t value, int module_idx) {
    int existing = find_global_symbol_exact(lk, name);
    if (existing >= 0) {
        /* Duplicate symbol handling: first ENTRY wins.
         * DRIVERASM exports thunks (GETSPACE, SYSTEM_ERROR, etc.) that
         * read the driver jump table at $210 and indirect through it.
         * The real implementations are in Pascal units compiled earlier.
         * If we let DRIVERASM's thunks overwrite the real implementations,
         * any call before INIT_JTDRIVER populates $210 will crash. */
        if (type == LSYM_ENTRY) {
            if (lk->symbols[existing].type != LSYM_ENTRY) {
                /* Promote EXTERN to ENTRY */
                lk->symbols[existing].type = LSYM_ENTRY;
                lk->symbols[existing].value = value;
                lk->symbols[existing].module_idx = module_idx;
            } else {
                /* Both ENTRY.
                 *
                 * Priority rules:
                 *
                 * 1. A built-in stub (module_idx == -1) is a fallback for
                 *    symbols that have no real implementation in source.
                 *    It must ALWAYS yield to a real module providing the
                 *    symbol — otherwise QuickDraw's MoveTo / SetPort /
                 *    DrawString etc. all stay stuck on the stub and every
                 *    caller reports unresolved in Phase 3 (the stub has
                 *    no module to take a base address from in Phase 2).
                 *
                 * 2. DRIVERASM thunks (GETSPACE, INTSOFF, ...) indirect
                 *    through the driver jump table at $210. Real Pascal
                 *    implementations compiled earlier must not be shadowed
                 *    by these thunks, so any non-DRIVERASM entry replaces
                 *    a DRIVERASM one.
                 *
                 * 3. LOADER.TEXT is the bootloader. It defines helper
                 *    routines (SETMMU, TERMINATE, RANGEERR, BGETSPACE…)
                 *    with signatures that differ from the kernel's.
                 *    STARTUP.TEXT re-declares SETMMU with the kernel's
                 *    5-param signature, and kernel callers pass 5 args.
                 *    We skip LOADER's BOOTINIT entirely in emulation, so
                 *    LOADER's routines should never win collision — let
                 *    any non-LOADER entry replace a LOADER one.
                 *
                 * 4. Otherwise: first non-DRIVERASM, non-stub, non-LOADER
                 *    ENTRY wins.
                 */
                int ex_mod = lk->symbols[existing].module_idx;
                bool existing_is_stub = (ex_mod < 0);
                bool new_is_real      = (module_idx >= 0);
                bool existing_is_driverasm = false;
                bool new_is_driverasm = false;
                bool existing_is_loader = false;
                bool new_is_loader = false;
                if (ex_mod >= 0 && ex_mod < lk->num_modules) {
                    existing_is_driverasm = (strcasestr(lk->modules[ex_mod]->filename, "DRIVERASM") != NULL);
                    existing_is_loader    = (strcasestr(lk->modules[ex_mod]->filename, "LOADER")    != NULL);
                }
                if (module_idx >= 0 && module_idx < lk->num_modules) {
                    new_is_driverasm = (strcasestr(lk->modules[module_idx]->filename, "DRIVERASM") != NULL);
                    new_is_loader    = (strcasestr(lk->modules[module_idx]->filename, "LOADER")    != NULL);
                }

                if (existing_is_stub && new_is_real) {
                    lk->symbols[existing].value = value;
                    lk->symbols[existing].module_idx = module_idx;
                } else if (existing_is_driverasm && !new_is_driverasm) {
                    lk->symbols[existing].value = value;
                    lk->symbols[existing].module_idx = module_idx;
                } else if (existing_is_loader && !new_is_loader) {
                    lk->symbols[existing].value = value;
                    lk->symbols[existing].module_idx = module_idx;
                }
                /* else: keep existing. */
            }
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
    lk->symbols[idx].resolved = false;  /* Phase 2 resolves kernel module symbols */
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
    for (uint32_t i = 0; i < num_syms && mod->num_symbols < 16384; i++) {
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
    for (uint32_t i = 0; i < num_relocs && mod->num_relocs < 16384; i++) {
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
        int gidx = add_global_symbol(lk, mod->symbols[i].name, st, mod->symbols[i].value, mod_idx);
        if (gidx >= 0 && mod->symbols[i].is_const)
            lk->symbols[gidx].is_const = true;
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

    mod->is_kernel = true;  /* Default: include in output. Caller can override. */
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
    for (int i = 0; i < num_symbols && mod->num_symbols < 16384; i++) {
        if (!cg_syms[i].name[0]) continue;
        int si = mod->num_symbols++;
        strncpy(mod->symbols[si].name, cg_syms[i].name, 63);
        mod->symbols[si].type = cg_syms[i].is_external ? 1 : 0;
        mod->symbols[si].is_const = cg_syms[i].is_const ? 1 : 0;
        mod->symbols[si].value = cg_syms[i].offset;
    }

    /* Copy relocations from codegen format */
    const cg_reloc_t *cg_rels = (const cg_reloc_t *)rel_data;
    for (int i = 0; i < num_relocs && mod->num_relocs < 16384; i++) {
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
        int gidx = add_global_symbol(lk, mod->symbols[i].name, st, mod->symbols[i].value, mod_idx);
        if (gidx >= 0 && mod->symbols[i].is_const)
            lk->symbols[gidx].is_const = true;
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
        if (!mod->is_kernel) {
            mod->base_addr = 0;  /* Non-kernel modules don't get addresses */
            continue;
        }
        /* Word-align */
        if (current_addr & 1) current_addr++;
        mod->base_addr = current_addr;
        current_addr += mod->code_size;
    }

    lk->segments[0].size = current_addr;

    /* Log module map for debugging */
    fprintf(stderr, "=== MODULE MAP ===\n");
    for (int i = 0; i < lk->num_modules; i++) {
        link_module_t *mod = lk->modules[i];
        if (!mod->is_kernel) continue;
        fprintf(stderr, "  $%06X-$%06X (%6u bytes) %s\n",
                mod->base_addr, mod->base_addr + mod->code_size - 1,
                mod->code_size, mod->name);
    }
    fprintf(stderr, "=== END MODULE MAP (total $%X = %u bytes) ===\n",
            current_addr, current_addr);

    /* Phase 2: Resolve symbols — update entry point addresses with base offsets.
     * Only resolve symbols from kernel modules (which are placed in the output).
     * Non-kernel modules have base_addr=0, so their raw offsets would collide
     * with kernel code. Those symbols go to the stub in Phase 4 instead.
     *
     * P86: A5-relative global VARs (data symbols declared in Pascal UNITS and
     * pinned via PASCALDEFS — b_syslocal_ptr at -24785, c_pcb_ptr at -24617,
     * etc.) are represented as NEGATIVE offsets. Code entry-point addresses
     * are non-negative (module offsets). Only add the module base_addr to
     * the positive (code) symbols — A5-relative data offsets must stay at
     * their pinned value so hand-coded asm (Launch, SCHDTRAP, etc.) reads
     * them at the hardwired PASCALDEFS offsets. Before this fix, Launch
     * read b_syslocal_ptr at -24785 from A5 but the compiler had relocated
     * the VAR's storage to (-24785 + SYSGLOBAL.base_addr) = -953; the slot
     * at -24785 was uninitialized ($FFFFFFFF) and Launch dereferenced
     * garbage. */
    for (int i = 0; i < lk->num_symbols; i++) {
        link_symbol_t *sym = &lk->symbols[i];
        if (sym->type == LSYM_ENTRY && sym->module_idx >= 0 &&
            sym->module_idx < lk->num_modules) {
            link_module_t *mod = lk->modules[sym->module_idx];
            if (mod->is_kernel) {
                /* P88: Pascal CONSTs carry their literal value, not a
                 * module-relative offset — skip base_addr relocation.
                 * Without this, e.g. `logrealmem = $AA0000` got bumped
                 * to $AA0000 + module_base, making MAKE_FREE's self-
                 * descriptive SDB pointer (maddr*512 + logrealmem) land
                 * at a realmemmmu alias that clobbered kernel code at
                 * phys $019400 — the ILLEGAL after SYS_PROC_INIT. */
                if (sym->value >= 0 && !sym->is_const) {
                    sym->value += mod->base_addr;
                }
                sym->resolved = true;
            }
            /* Non-kernel symbols remain unresolved → stub in Phase 4 */
        }
    }

    /* Phase 3: Apply relocations */
    for (int m = 0; m < lk->num_modules; m++) {
        link_module_t *mod = lk->modules[m];
        for (int r = 0; r < mod->num_relocs; r++) {
            const char *sym_name = mod->relocs[r].symbol;
            uint32_t offset = mod->relocs[r].offset;
            int size = mod->relocs[r].size;

            /* $SELF relocation: handled in Phase 4 only (not Phase 3) */
            if (strcmp(sym_name, "$SELF") == 0) continue;

            int sym_idx = find_global_symbol(lk, sym_name);
            if (sym_idx < 0 || !lk->symbols[sym_idx].resolved) {
                /* Phase 4 re-applies all relocations and routes anything
                 * still unresolved to the stub at $3F0 (CLR.L D0; RTS).
                 * Don't raise link_error here — it'd double-report and
                 * mask the real link outcome. Phase 4's final sweep is
                 * the authoritative unresolved check. */
                continue;
            }

            int32_t target = lk->symbols[sym_idx].value;

            /* Patch the code */
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

    /* Phase 4: Build output — modules are placed starting at $400.
     * Output buffer must be large enough for base_addr + code. */
    /* Place stub at $3F0 (unused vector table area) instead of after code.
     * The end-of-code area gets overwritten by OS init (sysglobal, etc.),
     * but the vector table area at $0-$3FF is preserved.
     *
     * Two stubs:
     *   $3F0: CLR.L D0; RTS  — for JSR relocations (4-byte return frame)
     *   $3F8: RTE             — for exception vector fills (6-byte frame)
     * Using RTS for an exception vector corrupts PC because RTS pops 4 bytes
     * but the exception pushed 6 (SR+PC), so PC is read as SR:PC_hi. */
    uint32_t stub_addr = 0x3F0;
    uint32_t rte_stub_addr = 0x3F8;
    uint32_t total_size = current_addr;
    if (total_size & 1) total_size++;

    lk->output = malloc(total_size);
    if (!lk->output) {
        link_error(lk, "cannot allocate %u bytes for output", total_size);
        return false;
    }
    lk->output_size = total_size;
    lk->output_capacity = total_size;

    memset(lk->output, 0, total_size);

    /* Function stub at $3F0: CLR.L D0; RTS */
    lk->output[stub_addr]     = 0x42; /* CLR.L D0 = $4280 */
    lk->output[stub_addr + 1] = 0x80;
    lk->output[stub_addr + 2] = 0x4E; /* RTS = $4E75 */
    lk->output[stub_addr + 3] = 0x75;

    /* Exception stub at $3F8: RTE */
    lk->output[rte_stub_addr]     = 0x4E; /* RTE = $4E73 */
    lk->output[rte_stub_addr + 1] = 0x73;

    /* Resolve all unresolved externals to point to the stub */
    for (int i = 0; i < lk->num_symbols; i++) {
        if (lk->symbols[i].type == LSYM_EXTERN && !lk->symbols[i].resolved) {
            lk->symbols[i].value = stub_addr;
            lk->symbols[i].resolved = true;
        }
    }

    /* Re-apply ALL relocations — resolved symbols get their address,
     * unresolved ones get the stub address (never leave JSR $000000) */
    int relocs_to_stub = 0;
    /* Track which symbols go to stub */
    typedef struct { char name[64]; int count; } stub_sym_t;
    stub_sym_t *stub_syms = calloc(4096, sizeof(stub_sym_t));
    int num_stub_syms = 0;

    for (int m = 0; m < lk->num_modules; m++) {
        link_module_t *mod = lk->modules[m];
        for (int r = 0; r < mod->num_relocs; r++) {
            /* $SELF relocation: add module base_addr to existing value */
            if (strcmp(mod->relocs[r].symbol, "$SELF") == 0) {
                uint32_t offset = mod->relocs[r].offset;
                if (offset + 3 < mod->code_size) {
                    int32_t existing = ((int32_t)mod->code[offset] << 24) |
                                       ((int32_t)mod->code[offset+1] << 16) |
                                       ((int32_t)mod->code[offset+2] << 8) |
                                       (int32_t)mod->code[offset+3];
                    int32_t fixed = existing + (int32_t)mod->base_addr;
                    mod->code[offset]     = (fixed >> 24) & 0xFF;
                    mod->code[offset + 1] = (fixed >> 16) & 0xFF;
                    mod->code[offset + 2] = (fixed >> 8)  & 0xFF;
                    mod->code[offset + 3] = fixed & 0xFF;
                }
                continue;
            }
            int sym_idx = find_global_symbol(lk, mod->relocs[r].symbol);
            int32_t target;
            if (sym_idx >= 0 && lk->symbols[sym_idx].resolved &&
                lk->symbols[sym_idx].value != (int32_t)stub_addr) {
                target = lk->symbols[sym_idx].value;
            } else {
                target = stub_addr;  /* Unknown symbol → stub, never 0 */
                relocs_to_stub++;
                /* Track stub symbol */
                bool found = false;
                for (int s = 0; s < num_stub_syms; s++) {
                    if (strcasecmp(stub_syms[s].name, mod->relocs[r].symbol) == 0) {
                        stub_syms[s].count++;
                        found = true;
                        break;
                    }
                }
                if (!found && num_stub_syms < 4096) {
                    strncpy(stub_syms[num_stub_syms].name, mod->relocs[r].symbol, 63);
                    stub_syms[num_stub_syms].count = 1;
                    num_stub_syms++;
                }
            }
            uint32_t offset = mod->relocs[r].offset;
            int size = mod->relocs[r].size;
            /* Log STARTUP INTSOFF in re-apply */
            if (m == 0 && offset >= 0x4750 && offset <= 0x4760) {
                fprintf(stderr, "  REAPPLY STARTUP@$%X: sym='%s' → $%X (sym_idx=%d type=%d)\n",
                        offset, mod->relocs[r].symbol, target, sym_idx,
                        sym_idx >= 0 ? lk->symbols[sym_idx].type : -1);
            }
            if (strcasecmp(mod->relocs[r].symbol, "TRAP1") == 0) {
                fprintf(stderr, "  LINKER PATCH: TRAP1 target=$%08X at module offset %u (code_size=%u, base=$%X)\n",
                        target, offset, mod->code_size, mod->base_addr);
            }
            if (strcasecmp(mod->relocs[r].symbol, "INIT_TRAPV") == 0 ||
                strcasecmp(mod->relocs[r].symbol, "INIT_NMI_TRAPV") == 0) {
                fprintf(stderr, "  LINKER PATCH: %s target=$%08X in module '%s' at offset %u\n",
                        mod->relocs[r].symbol, target, mod->name, offset);
            }
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
    for (int i = 0; i < lk->num_modules; i++) {
        link_module_t *mod = lk->modules[i];
        if (mod->is_kernel && mod->code && mod->code_size > 0) {
            memcpy(lk->output + mod->base_addr, mod->code, mod->code_size);
        }
    }

    /* Phase 5: Install exception vector table in $0-$3FF.
     * INIT_TRAPV normally does this at runtime, but INTSOFF uses TRAP #7
     * before INIT_TRAPV runs. Pre-install vectors so early boot code works.
     * Vector addresses match SOURCE-INITRAP.TEXT EQU definitions. */
    {
        /* Helper to write a vector from a symbol name */
        #define INSTALL_VEC(vec_offset, sym_name) do { \
            int _idx = find_global_symbol(lk, sym_name); \
            if (_idx >= 0 && lk->symbols[_idx].resolved && \
                lk->symbols[_idx].value != (int32_t)stub_addr) { \
                uint32_t _addr = lk->symbols[_idx].value; \
                lk->output[vec_offset]     = (_addr >> 24) & 0xFF; \
                lk->output[vec_offset + 1] = (_addr >> 16) & 0xFF; \
                lk->output[vec_offset + 2] = (_addr >> 8)  & 0xFF; \
                lk->output[vec_offset + 3] = _addr & 0xFF; \
                vec_installed++; \
            } \
        } while(0)

        int vec_installed = 0;

        /* Vector 0: SSP, Vector 1: PC — set by loader, not here */

        /* Exception vectors (matching INITRAP EQUs) */
        INSTALL_VEC(0x08, "BUS_ERR");            /* Bus error */
        INSTALL_VEC(0x0C, "ADDRERROR_TRAP");     /* Address error */
        INSTALL_VEC(0x10, "ILLGINST_TRAP");      /* Illegal instruction */
        INSTALL_VEC(0x14, "DIVZERO_TRAP");       /* Zero divide */
        INSTALL_VEC(0x18, "VALUEOOB_TRAP");      /* CHK instruction */
        INSTALL_VEC(0x1C, "OVERFLOW_TRAP");      /* TRAPV instruction */
        INSTALL_VEC(0x20, "PRIVIOLATION_TRAP");  /* Privilege violation */
        /* LINE1111_TRAP: NOT pre-installed. The OS handler (line1111_trap
         * in EXCEPASM) calls system_error, which isn't ready during early
         * boot. The bootrom's Line-F handler at $FE0310 safely skips the
         * opcode. INIT_TRAPV installs the real handler when ready. */

        /* Spurious + autovector interrupts */
        INSTALL_VEC(0x60, "SPURINTR_TRAP");      /* Spurious interrupt */
        INSTALL_VEC(0x6C, "LVL3INT");            /* Level 3 interrupt */
        INSTALL_VEC(0x70, "LVL4INT");            /* Level 4 interrupt */
        INSTALL_VEC(0x74, "LVL5INT");            /* Level 5 interrupt */
        INSTALL_VEC(0x78, "RSINT");              /* Level 6 interrupt */

        /* TRAP vectors */
        INSTALL_VEC(0x84, "TRAP1");              /* TRAP #1 (OS syscall) */
        INSTALL_VEC(0x88, "SCHDTRAP");           /* TRAP #2 (scheduler) */
        /* TRAP #6 (do_an_mmu, MMU programming) is NOT pre-installed.
         * ENTEROP (in source-LDASM.TEXT) copies do_an_mmu to a specific
         * physical PAGE at runtime and writes that page's address into
         * vector $98 itself. Pre-installing a bogus address would be
         * overwritten anyway — or, worse, fire before the real handler
         * is ready. Leave it at the RTE stub until the OS installs it. */
        INSTALL_VEC(0x9C, "TRAP7");              /* TRAP #7 (SR change) */
        INSTALL_VEC(0xB8, "trapEhandler");       /* TRAP #14 */

        /* Driver jump table: DRIVERASM base at $210.
         * CALLDRIVER reads this and dispatches through it. */
        /* DRIVRJT at $210: NOT pre-installed. The OS's INIT_CONFIG
         * allocates driver JT space in sysglobal and writes it to $210.
         * Pre-installing DRIVERASM's address causes a jump loop. */

        /* Fill unset vectors ($0-$FC) with the RTE stub. Exception frames
         * are 6 bytes (SR+PC), so the handler MUST use RTE — using the
         * $3F0 RTS stub here would corrupt PC on return. */
        for (int v = 2; v < 64; v++) {
            int off = v * 4;
            uint32_t cur = ((uint32_t)lk->output[off] << 24) |
                           ((uint32_t)lk->output[off+1] << 16) |
                           ((uint32_t)lk->output[off+2] << 8) |
                           (uint32_t)lk->output[off+3];
            if (cur == 0) {
                lk->output[off]     = (rte_stub_addr >> 24) & 0xFF;
                lk->output[off + 1] = (rte_stub_addr >> 16) & 0xFF;
                lk->output[off + 2] = (rte_stub_addr >> 8)  & 0xFF;
                lk->output[off + 3] = rte_stub_addr & 0xFF;
            }
        }

        fprintf(stderr, "Linker: installed %d exception vectors in $0-$FF\n", vec_installed);
        #undef INSTALL_VEC
    }

    /* Dump stub symbol report */
    if (num_stub_syms > 0) {
        /* Sort by count descending */
        for (int i = 0; i < num_stub_syms - 1; i++)
            for (int j = i + 1; j < num_stub_syms; j++)
                if (stub_syms[j].count > stub_syms[i].count) {
                    stub_sym_t tmp = stub_syms[i];
                    stub_syms[i] = stub_syms[j];
                    stub_syms[j] = tmp;
                }
        fprintf(stderr, "Linker: %d relocations to stub (%d unique symbols):\n",
                relocs_to_stub, num_stub_syms);
        int show = num_stub_syms < 300 ? num_stub_syms : 300;
        for (int i = 0; i < show; i++)
            fprintf(stderr, "  %4d x  %s\n", stub_syms[i].count, stub_syms[i].name);
    }
    free(stub_syms);

    /* Final safety: scan output for JSR $000000 patterns and patch to stub.
     * This catches any relocations that were dropped due to limits. */
    int patched_zeros = 0;
    for (uint32_t i = 0x400; i + 5 < lk->output_size; i += 2) {
        if (lk->output[i] == 0x4E && lk->output[i+1] == 0xB9 &&
            lk->output[i+2] == 0x00 && lk->output[i+3] == 0x00 &&
            lk->output[i+4] == 0x00 && lk->output[i+5] == 0x00) {
            /* JSR $000000 — patch to stub */
            lk->output[i+2] = (stub_addr >> 24) & 0xFF;
            lk->output[i+3] = (stub_addr >> 16) & 0xFF;
            lk->output[i+4] = (stub_addr >> 8)  & 0xFF;
            lk->output[i+5] = stub_addr & 0xFF;
            patched_zeros++;
        }
    }
    if (patched_zeros > 0) {
        fprintf(stderr, "Linker: patched %d JSR $000000 to stub at $%X\n",
                patched_zeros, stub_addr);
    }

    /* Check for unresolved externals after Phase 4 stubbing. Remaining
     * unresolved entries are LSYM_ENTRY stubs pre-registered by
     * linker_init (module_idx == -1) that no real module provided.
     * Those are deliberately stubbed; they're not a link failure —
     * they're symbols that would be loaded on-demand by the Lisa OS
     * segment loader at runtime. Count them as warnings, not errors. */
    int unresolved = 0;
    int unresolved_stubs = 0;
    for (int i = 0; i < lk->num_symbols; i++) {
        if (lk->symbols[i].resolved) continue;
        unresolved++;
        if (lk->symbols[i].type == LSYM_ENTRY && lk->symbols[i].module_idx < 0) {
            unresolved_stubs++;
            /* Resolve to the stub and mark as such. */
            lk->symbols[i].value = stub_addr;
            lk->symbols[i].resolved = true;
            continue;
        }
        link_error(lk, "unresolved external: '%s' (type=%d)",
                  lk->symbols[i].name, lk->symbols[i].type);
    }
    if (unresolved_stubs > 0) {
        fprintf(stderr, "Linker: %d symbols resolved to deferred-load stub "
                        "(quickdraw/print/alert/etc. runtime-loaded)\n",
                unresolved_stubs);
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
    /* Dump output bytes at $4EC to verify BRA displacement */
    if (lk->output_size > 0x500) {
        fprintf(stderr, "Linker output at $4E8-$4FF: ");
        for (uint32_t i = 0x4E8; i < 0x500; i++)
            fprintf(stderr, "%02X ", lk->output[i]);
        fprintf(stderr, "\n");
    }
    /* Find nearest symbol to the escape address $4AF14 */
    {
        uint32_t target = 0x4AF14;
        const char *nearest = NULL;
        int32_t nearest_dist = 0x7FFFFFFF;
        for (int i = 0; i < lk->num_symbols; i++) {
            if (lk->symbols[i].type == LSYM_ENTRY && lk->symbols[i].resolved) {
                int32_t dist = (int32_t)target - lk->symbols[i].value;
                if (dist >= 0 && dist < nearest_dist) {
                    nearest_dist = dist;
                    nearest = lk->symbols[i].name;
                }
            }
        }
        if (nearest)
            fprintf(stderr, "  NEAREST to $%X: '%s' at $%X (+%d)\n",
                    target, nearest, target - nearest_dist, nearest_dist);
    }
    /* Search for specific symbols to debug */
    const char *debug_syms[] = {"INIT_TRAPV", "INTSOFF", "INITSYS", "TRAP1", "TRAP7", "BUS_ERR", "SCHDTRAP", "INIT_NMI_TRAPV", "INTSON", "PASCALINIT", "GETLDMAP", "REG_TO_MAPPED", "SYSTEM_ERROR", "POOL_INIT", "INIT_SCTAB", "SCTAB1", "BAD_SCALL", "MMU_BASE", "INIT_PE", "INIT_FREEPOOL", "GETSPACE", "GETFREE", "MAKE_FREE", "INSERTSDB", "AVAIL_INIT", "BLDPGLEN", "GET_BOOTSPACE", "INIT_CONFIG", "MAKE_REGION", "TAKE_FREE", NULL};
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

    /* Also write a sorted symbol map: build/<filename>.map */
    char mapname[512];
    snprintf(mapname, sizeof(mapname), "%s.map", filename);
    FILE *mf = fopen(mapname, "w");
    if (mf) {
        int *order = (int*)malloc(sizeof(int) * lk->num_symbols);
        int n = 0;
        for (int i = 0; i < lk->num_symbols; i++) {
            if (lk->symbols[i].type == LSYM_ENTRY) order[n++] = i;
        }
        /* sort by value ascending */
        for (int i = 1; i < n; i++) {
            int key = order[i];
            uint32_t kv = lk->symbols[key].value;
            int j = i - 1;
            while (j >= 0 && lk->symbols[order[j]].value > kv) {
                order[j+1] = order[j]; j--;
            }
            order[j+1] = key;
        }
        for (int i = 0; i < n; i++) {
            fprintf(mf, "$%06X  %s\n",
                    lk->symbols[order[i]].value,
                    lk->symbols[order[i]].name);
        }
        fclose(mf);
        free(order);
        printf("Symbol map: %s (%d entries)\n", mapname, n);
    }

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
        /* Alert Manager / Font Catalog asm helper */
        "CallProc",
        /* Database recovery (LIBDB) */
        "MarksRevenge",
        /* MathLib sort callback */
        "sorted",
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
