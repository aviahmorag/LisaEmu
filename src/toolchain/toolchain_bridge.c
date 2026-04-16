/*
 * LisaEm Toolchain — C Bridge API
 *
 * Orchestrates the full build pipeline:
 *   1. Find all source files in Lisa_Source
 *   2. Compile Pascal files to OBJ
 *   3. Assemble assembly files to OBJ
 *   4. Link OBJ files into executables
 *   5. Build a bootable disk image
 */

#include "toolchain_bridge.h"
#include "pascal_lexer.h"
#include "pascal_parser.h"
#include "pascal_codegen.h"
#include "asm68k.h"
#include "linker.h"
#include "diskimage.h"
#include "bootrom.h"
#include "toolchain_fileset.h"
#include "compile_targets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* HLE address globals — populated during build, read by lisa.c after */
uint32_t hle_addr_calldriver = 0;
uint32_t hle_addr_system_error = 0;

/* ========================================================================
 * File discovery
 * ======================================================================== */

#define MAX_SOURCE_FILES 2048

typedef struct {
    char path[512];
    bool is_assembly;  /* true = assembly, false = Pascal */
} source_file_t;

/* Exclusion rules shared with audit_toolchain.c — see toolchain_fileset.c. */
#define should_skip_dir(n)  tc_should_skip_dir(n)
#define should_skip_file(n) tc_should_skip_file(n)

static int find_source_files(const char *dir, source_file_t *files, int max_files) {
    int count = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < max_files) {
        if (entry->d_name[0] == '.') continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (should_skip_dir(entry->d_name)) continue;
            count += find_source_files(path, files + count, max_files - count);
        } else if (S_ISREG(st.st_mode)) {
            /* Must have .TEXT.unix.txt extension */
            const char *ext = strcasestr(entry->d_name, ".TEXT.unix.txt");
            if (!ext) continue;

            /* Skip non-source files */
            if (should_skip_file(entry->d_name)) continue;

            /* Determine if assembly or Pascal by checking file content */
            bool is_asm = (strcasestr(entry->d_name, "ASM") != NULL) ||
                          (strcasestr(entry->d_name, "QSORT") != NULL);
            if (!is_asm) {
                /* Check first few non-empty lines for assembly indicators */
                FILE *probe = fopen(path, "r");
                if (probe) {
                    char line[256];
                    int lines_checked = 0;
                    while (fgets(line, sizeof(line), probe) && lines_checked < 5) {
                        const char *p = line;
                        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                        if (*p == '\0') continue;
                        lines_checked++;
                        if (*p == ';') { is_asm = true; break; }
                        if (*p == '.' && strchr("IiPpDdTtMmSsEeRrFfBb", p[1])) {
                            is_asm = true; break;
                        }
                        /* PAGE directive (common asm formatting) */
                        if (strncasecmp(p, "PAGE", 4) == 0 && (p[4]=='\0'||p[4]==' '||p[4]=='\n'||p[4]=='\r'))
                            continue;
                        break;
                    }
                    fclose(probe);
                }
            }

            /* LIBFP: skip assembly include-fragments (assembled via NEWFPSUB).
             * Keep NEWFPSUB itself and all Pascal files in LIBFP. */
            if (is_asm && strcasestr(entry->d_name, "libfp-") != NULL &&
                !strcasestr(entry->d_name, "NEWFPSUB")) {
                continue;  /* Assembly include fragment — skip */
            }

            strncpy(files[count].path, path, sizeof(files[count].path) - 1);
            files[count].is_assembly = is_asm;
            count++;
        }
    }
    closedir(d);
    return count;
}

/* Scan a Pascal/asm file for $I include directives (either
 * `{$I name}` or `{$Iname}` / `(*$Iname*)` forms). Fills `out` with
 * the included filenames' basenames (in uppercase, without
 * extension) up to max_out entries. Returns count. */
static int scan_file_for_includes(const char *path, char out[][64], int max_out) {
    int n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), f) && n < max_out) {
        for (const char *p = line; *p; p++) {
            /* Match `{$I` or `(*$I` (not followed by 'F' of IFC). */
            if (p[0] != '$' || (p[1] != 'I' && p[1] != 'i')) continue;
            /* Check for preceding '{' or '(*' */
            bool good_prefix = false;
            if (p > line && p[-1] == '{') good_prefix = true;
            if (p >= line + 2 && p[-1] == '*' && p[-2] == '(') good_prefix = true;
            if (!good_prefix) continue;
            /* Exclude $IFC */
            if ((p[2] == 'F' || p[2] == 'f') && (p[3] == 'C' || p[3] == 'c')) continue;
            /* Parse the include filename (can be with or without space after $I). */
            const char *q = p + 2;
            while (*q == ' ' || *q == '\t') q++;
            char inc[256]; int pi = 0;
            while (*q && *q != '}' && *q != '*' && *q != ' ' && pi < 255) {
                inc[pi++] = *q++;
            }
            inc[pi] = '\0';
            if (!inc[0]) continue;
            /* Strip leading dir prefix (e.g. "source/MM1.text" → "MM1.text"). */
            const char *slash = strrchr(inc, '/');
            const char *colon = strrchr(inc, ':');
            const char *sep = slash ? slash : colon;
            const char *base_inc = sep ? sep + 1 : inc;
            /* Strip extension. */
            char bare[64];
            size_t blen = strlen(base_inc);
            if (blen >= sizeof(bare)) blen = sizeof(bare) - 1;
            memcpy(bare, base_inc, blen);
            bare[blen] = '\0';
            char *dot = strchr(bare, '.');
            if (dot) *dot = '\0';
            /* Uppercase for comparison. */
            for (char *c = bare; *c; c++) *c = toupper((unsigned char)*c);
            if (!bare[0]) continue;
            strncpy(out[n], bare, 63);
            out[n][63] = '\0';
            n++;
            if (n >= max_out) break;
        }
    }
    fclose(f);
    return n;
}

/* Build a set of basenames (uppercase, no extension) that are
 * $I-included by OTHER files in the list. These should be skipped
 * from standalone compilation to avoid double-emission. */
static int build_included_set(const source_file_t *files, int num_files,
                              char included[][64], int max_included) {
    int n = 0;
    char tmp[32][64];
    for (int i = 0; i < num_files; i++) {
        int m = scan_file_for_includes(files[i].path, tmp, 32);
        for (int j = 0; j < m && n < max_included; j++) {
            /* Dedupe. */
            bool dup = false;
            for (int k = 0; k < n; k++) {
                if (strcmp(included[k], tmp[j]) == 0) { dup = true; break; }
            }
            if (!dup) {
                strncpy(included[n], tmp[j], 63);
                included[n][63] = '\0';
                n++;
            }
        }
    }
    return n;
}

/* Check if a file's basename matches any entry in the "included" set. */
static bool file_is_included(const char *path, char included[][64], int n_included) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    /* Extract bare basename — strip "source-" prefix and ".TEXT..." extension. */
    char bare[64];
    strncpy(bare, base, sizeof(bare) - 1);
    bare[sizeof(bare) - 1] = '\0';
    /* Skip "source-" prefix. */
    char *b = bare;
    if (strncasecmp(b, "source-", 7) == 0) b += 7;
    /* Strip first '.' */
    char *dot = strchr(b, '.');
    if (dot) *dot = '\0';
    /* Uppercase. */
    for (char *c = b; *c; c++) *c = toupper((unsigned char)*c);
    for (int i = 0; i < n_included; i++) {
        if (strcmp(b, included[i]) == 0) return true;
    }
    return false;
}

/* ========================================================================
 * Compilation
 * ======================================================================== */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* Shared globals accumulated from all previously compiled units */
#define MAX_SHARED_GLOBALS 65536
static cg_symbol_t shared_globals[MAX_SHARED_GLOBALS];
static int num_shared_globals = 0;

/* Shared procedure signatures accumulated from all previously compiled units */
#define MAX_SHARED_PROC_SIGS 8192
static cg_proc_sig_t shared_proc_sigs[MAX_SHARED_PROC_SIGS];
static int num_shared_proc_sigs = 0;

/* All compiled modules are treated as kernel — they all get linked into the
 * output and their symbols are available for cross-module resolution. */
static bool is_kernel_module(const char *path) {
    if (strcasestr(path, "/OS/") != NULL) return true;
    if (strcasestr(path, "LIBPL") != NULL) return true;
    if (strcasestr(path, "LIBHW") != NULL) return true;
    if (strcasestr(path, "LIBFP") != NULL) return true;
    if (strcasestr(path, "LIBOS") != NULL) return true;
    return false;
}

/* Shared types accumulated from all previously compiled units */
#define MAX_SHARED_TYPES 8192
static type_desc_t shared_types[MAX_SHARED_TYPES];
static int num_shared_types = 0;

/* When `types_only_pass` is true, parse the file and export its TYPE/CONST
 * decls to the shared tables so subsequent files can resolve cross-unit
 * named types (e.g. `semaphore` from source-procprims.text.unix.txt
 * referenced by source-MMPRIM.TEXT's mmrb record). Doesn't emit code
 * or push to the linker. Fixes the compile-order issue where a file
 * using another unit's types compiles BEFORE that unit, producing
 * records whose field types have `size=-1` (NULL type ptr), leading
 * to wrong overall record size and miscompiled sentinel stores. */
static bool compile_pascal_file_impl(const char *path, linker_t *lk, bool types_only_pass);

static bool compile_pascal_file(const char *path, linker_t *lk) {
    return compile_pascal_file_impl(path, lk, false);
}

static bool compile_pascal_file_impl(const char *path, linker_t *lk, bool types_only_pass) {
    char *source = read_file(path);
    if (!source) return false;

    bool is_startup = (strcasestr(path, "SOURCE-STARTUP") != NULL);

    parser_t parser;
    parser_init(&parser, source, path);

    /* Set source_root for {$I} include resolution.
     * Derive from path: .../Lisa_Source/LISA_OS/LIBS/FOO/file → .../Lisa_Source */
    {
        const char *marker = strcasestr(path, "/LISA_OS/");
        if (marker) {
            size_t rlen = (size_t)(marker - path);
            if (rlen < sizeof(parser.lex.source_root))  {
                memcpy(parser.lex.source_root, path, rlen);
                parser.lex.source_root[rlen] = '\0';
            }
        }
    }

    ast_node_t *ast = parser_parse(&parser);

    (void)is_startup;
    if (parser.num_errors > 0) {
        /* Log errors but continue — partial code is better than no code.
         * Previously we discarded the entire file, losing all its symbols. */
        const char *basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        fprintf(stderr, "  PARSE: %d errors in %s (line %d) — continuing with partial AST\n",
                parser.num_errors, basename, parser.lex.line);
    }

    codegen_t *cg = calloc(1, sizeof(codegen_t));
    if (!cg) {
        parser_free(&parser);
        free(source);
        return false;
    }
    codegen_init(cg);
    strncpy(cg->current_file, path, sizeof(cg->current_file) - 1);
    cg->imported_globals = shared_globals;
    cg->imported_globals_count = num_shared_globals;
    cg->imported_types = shared_types;
    cg->imported_types_count = num_shared_types;
    cg->imported_proc_sigs = shared_proc_sigs;
    cg->imported_proc_sigs_count = num_shared_proc_sigs;
    if (is_startup) {
        fprintf(stderr, "STARTUP: imported %d proc sigs, %d globals, %d types\n",
                num_shared_proc_sigs, num_shared_globals, num_shared_types);
        /* Check for critical constants */
        {
            const char *check_names[] = {"sysglobmmu", "syslocmmu", "maxpgmmu",
                                          "hmempgsize", "maxmmusize", "mempgsize", NULL};
            for (int ci = 0; check_names[ci]; ci++) {
                int found = 0;
                for (int i = 0; i < num_shared_globals; i++) {
                    if (strcasecmp(shared_globals[i].name, check_names[ci]) == 0) {
                        fprintf(stderr, "  CONST %s=%d\n", check_names[ci], shared_globals[i].offset);
                        found = 1;
                        break;
                    }
                }
                if (!found) fprintf(stderr, "  MISSING CONST: %s\n", check_names[ci]);
            }
        }
    }

    codegen_generate(cg, ast);

    if (is_startup && !types_only_pass) {
        fprintf(stderr, "STARTUP CODEGEN: %u bytes code, %d globals, %d relocs\n",
                cg->code_size, cg->num_globals, cg->num_relocs);
    }

    /* Export this file's globals (including CONSTs) to the shared table.
     * P79: CONSTs must also be exported during the types-only pre-pass so
     * that record types with string[CONST_NAME] fields (e.g. DCB.name using
     * string[max_ename]) resolve correctly. Without this, max_ename isn't
     * found during pre-pass record resolution, string[max_ename] falls back
     * to string[255], and DCB is 344 bytes instead of ~120.
     * On the real pass, only export non-duplicates (check by name). */
    for (int i = 0; i < cg->num_globals && num_shared_globals < MAX_SHARED_GLOBALS; i++) {
        if (!cg->globals[i].is_external && !cg->globals[i].is_forward) {
            /* On real pass, skip if already exported from pre-pass */
            if (!types_only_pass) {
                bool dup = false;
                for (int j = 0; j < num_shared_globals; j++) {
                    if (strcasecmp(shared_globals[j].name, cg->globals[i].name) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
            }
            shared_globals[num_shared_globals++] = cg->globals[i];
        }
    }

    /* Export this file's types to the shared table for cross-unit type casts.
     * On the types-only pre-pass, this is where the whole scheme pays off.
     * On the real (second) pass, types for this file are already in
     * shared_types from the pre-pass — re-exporting would duplicate them
     * and find_type's first-match could pick up the new copy that still
     * has stale NULL pointers from AST nodes destined for reparse. So
     * only export on the pre-pass; the second pass uses the pre-pass's
     * fully-resolved types by reference. */
    int types_base = num_shared_types;  /* index where this file's types start */
    if (!types_only_pass) goto skip_type_export;
    {
        /* Phase 1: copy all types */
        for (int i = 0; i < cg->num_types && num_shared_types < MAX_SHARED_TYPES; i++) {
            shared_types[num_shared_types++] = cg->types[i];
        }
        /* Helper: check if a pointer is within the cg->types array */
        #define IN_CG_TYPES(ptr) ((ptr) >= &cg->types[0] && (ptr) < &cg->types[cg->num_types])
        #define REMAP_TYPE_PTR(ptr) do { \
            if ((ptr) && IN_CG_TYPES(ptr)) { \
                ptrdiff_t _idx = (ptr) - cg->types; \
                (ptr) = &shared_types[types_base + _idx]; \
            } \
            /* If ptr is already in shared_types or elsewhere, leave it alone */ \
        } while(0)

        /* Phase 2: remap internal type pointers within the newly exported types */
        for (int i = types_base; i < num_shared_types; i++) {
            type_desc_t *t = &shared_types[i];
            REMAP_TYPE_PTR(t->element_type);
            REMAP_TYPE_PTR(t->base_type);
            REMAP_TYPE_PTR(t->set_base);
            for (int fi = 0; fi < t->num_fields; fi++) {
                REMAP_TYPE_PTR(t->fields[fi].type);
            }
        }
        /* Phase 3: remap type pointers in globals exported from this file.
         * We need to fix globals that were just added (they point into cg->types). */
        for (int i = 0; i < num_shared_globals; i++) {
            REMAP_TYPE_PTR(shared_globals[i].type);
        }
        #undef IN_CG_TYPES
        #undef REMAP_TYPE_PTR
    }
skip_type_export:;

    /* Export this file's procedure signatures to the shared table.
     * Skip on types-only pass — re-exporting on second pass would
     * duplicate entries.
     *
     * P30-followup: remap sig->param_type pointers from cg->types into
     * shared_types BY NAME. Previously we used `types_base + idx`, but
     * on the real (second) pass types aren't re-copied (skip_type_export
     * above), so types_base points past valid shared_types entries and
     * the remap lands in uninitialized slots — causing proc sigs to
     * report size=0 kind=0 for int2/int4/etc., which defaults param
     * sizes to 4 bytes in RECONST. Name-based lookup is stable across
     * passes. */
    if (cg->proc_sigs && !types_only_pass) {
        for (int i = 0; i < cg->num_proc_sigs && num_shared_proc_sigs < MAX_SHARED_PROC_SIGS; i++) {
            cg_proc_sig_t *sig = &shared_proc_sigs[num_shared_proc_sigs++];
            *sig = cg->proc_sigs[i];
            for (int j = 0; j < sig->num_params; j++) {
                type_desc_t *pt = sig->param_type[j];
                if (pt && pt >= &cg->types[0] && pt < &cg->types[cg->num_types] && pt->name[0]) {
                    /* Look up the same type by name in shared_types */
                    type_desc_t *found = NULL;
                    for (int k = 0; k < num_shared_types; k++) {
                        if (strcasecmp(shared_types[k].name, pt->name) == 0) {
                            found = &shared_types[k];
                            break;
                        }
                    }
                    if (found) sig->param_type[j] = found;
                    /* If not found by name (anonymous type), clear so RECONST's
                     * name-lookup fallback doesn't hit a dangling pointer. */
                    else sig->param_type[j] = NULL;
                } else if (pt && pt >= &cg->types[0] && pt < &cg->types[cg->num_types]) {
                    /* Anonymous type (no name) — can't remap safely, clear. */
                    sig->param_type[j] = NULL;
                }
                /* If already in shared_types (imported), leave as-is */
            }
        }
    }

    bool ok = true;
    if (cg->code_size > 0 && !types_only_pass) {
        ok = linker_load_codegen(lk, path,
                                  cg->code, cg->code_size,
                                  cg->globals, cg->num_globals,
                                  cg->relocs, cg->num_relocs);
        if (ok && lk->num_modules > 0 && !is_kernel_module(path))
            lk->modules[lk->num_modules - 1]->is_kernel = false;
    }

    codegen_free(cg);
    free(cg);
    parser_free(&parser);
    free(source);
    return ok;
}

static bool assemble_file(const char *path, linker_t *lk, const char *source_root) {
    asm68k_t *as = calloc(1, sizeof(asm68k_t));
    if (!as) return false;
    asm68k_init(as);

    /* Set base dir for includes */
    char *slash = strrchr(path, '/');
    if (slash) {
        char dir[256];
        size_t len = slash - path;
        if (len > 255) len = 255;
        strncpy(dir, path, len);
        dir[len] = '\0';
        asm68k_set_base_dir(as, dir);
    }

    /* Add cross-library include paths */
    if (source_root) {
        char inc[512];
        snprintf(inc, sizeof(inc), "%s/LISA_OS/LIBS", source_root);
        asm68k_add_include_path(as, inc);
        snprintf(inc, sizeof(inc), "%s/LISA_OS/OS", source_root);
        asm68k_add_include_path(as, inc);
    }

    bool ok = asm68k_assemble_file(as, path);

    if (ok && as->output_size > 0) {
        /* Convert assembler symbols to codegen format for the linker.
         * Export .PROC, .FUNC, .DEF symbols AND .REF symbols.
         * .PROC/.FUNC are entry points even without a .DEF directive. */
        int num_exported = 0;
        for (int i = 0; i < as->num_symbols; i++) {
            if (as->symbols[i].exported || as->symbols[i].external ||
                as->symbols[i].type == SYM_PROC || as->symbols[i].type == SYM_FUNC)
                num_exported++;
        }

        cg_symbol_t *syms = NULL;
        cg_reloc_t *rels = NULL;
        int nsyms = 0, nrels = 0;

        if (num_exported > 0) {
            syms = calloc(num_exported, sizeof(cg_symbol_t));
            if (syms) {
                for (int i = 0; i < as->num_symbols; i++) {
                    asm_symbol_t *s = &as->symbols[i];
                    if (!s->exported && !s->external &&
                        s->type != SYM_PROC && s->type != SYM_FUNC) continue;
                    strncpy(syms[nsyms].name, s->name, 63);
                    syms[nsyms].offset = s->value;
                    /* .PROC/.FUNC are global entry points even without .DEF */
                    syms[nsyms].is_global = s->exported ||
                        s->type == SYM_PROC || s->type == SYM_FUNC;
                    syms[nsyms].is_external = s->external;
                    nsyms++;
                }
            }
        }

        /* Convert assembler relocations */
        if (as->num_relocs > 0) {
            rels = calloc(as->num_relocs, sizeof(cg_reloc_t));
            if (rels) {
                for (int i = 0; i < as->num_relocs; i++) {
                    rels[nrels].offset = as->relocs[i].offset;
                    rels[nrels].size = as->relocs[i].size;
                    rels[nrels].pc_relative = as->relocs[i].pc_relative;
                    if (as->relocs[i].symbol_idx == -2) {
                        /* Self-relocation: linker adds module base_addr */
                        strncpy(rels[nrels].symbol, "$SELF", 63);
                    } else if (as->relocs[i].symbol_idx >= 0 &&
                        as->relocs[i].symbol_idx < as->num_symbols) {
                        strncpy(rels[nrels].symbol,
                                as->symbols[as->relocs[i].symbol_idx].name, 63);
                    }
                    nrels++;
                }
            }
        }

        if (nsyms > 0 || as->output_size > 0) {
            fprintf(stderr, "  ASM: %s — %u bytes code, %d exported symbols\n",
                    path, as->output_size, nsyms);
        }
        linker_load_codegen(lk, path,
                            asm68k_get_output(as, NULL), as->output_size,
                            syms, nsyms, rels, nrels);
        if (lk->num_modules > 0 && !is_kernel_module(path))
            lk->modules[lk->num_modules - 1]->is_kernel = false;
        free(syms);
        free(rels);
    }

    asm68k_free(as);
    free(as);
    return ok;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

bool toolchain_validate_source(const char *source_dir) {
    char path[512];

    /* Check for expected directories */
    snprintf(path, sizeof(path), "%s/LISA_OS", source_dir);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;

    snprintf(path, sizeof(path), "%s/LISA_OS/OS", source_dir);
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;

    snprintf(path, sizeof(path), "%s/LISA_OS/LIBS", source_dir);
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;

    return true;
}

build_result_t toolchain_build(const char *source_dir,
                                const char *output_dir,
                                build_progress_fn progress) {
    build_result_t result;
    memset(&result, 0, sizeof(result));

    /* Validate source directory */
    if (!toolchain_validate_source(source_dir)) {
        snprintf(result.error_message, sizeof(result.error_message),
                 "Invalid source directory: expected LISA_OS subdirectory");
        return result;
    }

    if (progress) progress("Finding source files...", 0, 100);

    /* Find all source files */
    source_file_t *files = calloc(MAX_SOURCE_FILES, sizeof(source_file_t));
    if (!files) {
        snprintf(result.error_message, sizeof(result.error_message), "Out of memory");
        return result;
    }

    /* P58: use the SYSTEM.OS compile target — explicit module list
     * from the reference project. Current state: the module list
     * matches Apple's ALEX-COMP-SYSTEMOS.TEXT + ALEX-LINK-SYSTEMOS.TEXT
     * (~50 modules). However our toolchain currently depends on some
     * support files that Apple's OS doesn't include in SYSTEM.OS
     * (e.g., $I-expansion differences, cross-module type propagation).
     * For now we walk search_dirs and compile EVERYTHING found — once
     * the toolchain is able to cleanly compile the minimal Apple set,
     * we'll switch to strict module filtering.
     *
     * The module list drives disk-image layout: each target will emit
     * a separate OBJ file at its out_path. Apps and libraries become
     * separate targets (future work). */
    const compile_target_t *target = compile_targets_find("SYSTEM.OS");
    if (!target) {
        snprintf(result.error_message, sizeof(result.error_message),
                 "SYSTEM.OS compile target not found");
        free(files);
        return result;
    }

    int num_files = 0;
    source_file_t *candidates = calloc(MAX_SOURCE_FILES, sizeof(source_file_t));
    int num_cand = 0;
    for (int d = 0; target->search_dirs[d]; d++) {
        char subdir[512];
        snprintf(subdir, sizeof(subdir), "%s/%s", source_dir, target->search_dirs[d]);
        num_cand += find_source_files(subdir, candidates + num_cand,
                                       MAX_SOURCE_FILES - num_cand);
    }

    /* Check env var: LISAEMU_STRICT_MODULES=1 enforces module filter. */
    const char *strict_env = getenv("LISAEMU_STRICT_MODULES");
    bool strict = strict_env && strict_env[0] == '1';

    int modules_requested = 0;
    while (target->modules[modules_requested]) modules_requested++;

    if (strict) {
        /* Filter candidates through the module list. */
        for (int c = 0; c < num_cand; c++) {
            const char *slash = strrchr(candidates[c].path, '/');
            const char *base = slash ? slash + 1 : candidates[c].path;
            bool matched = false;
            const char *matched_mod = NULL;
            for (int m = 0; target->modules[m] && !matched; m++) {
                const char *mod = target->modules[m];
                size_t mlen = strlen(mod);
                for (const char *p = base; *p; p++) {
                    if (strncasecmp(p, mod, mlen) != 0) continue;
                    if (p != base) {
                        char prev = *(p - 1);
                        if (isalnum((unsigned char)prev)) continue;
                    }
                    char next = p[mlen];
                    if (isalnum((unsigned char)next)) continue;
                    matched = true;
                    matched_mod = mod;
                    break;
                }
            }
            if (matched && num_files < MAX_SOURCE_FILES) {
                files[num_files++] = candidates[c];
                fprintf(stderr, "  [keep] %s (matches %s)\n", base, matched_mod);
            }
        }
        fprintf(stderr, "COMPILE TARGET '%s' [STRICT]: %d modules, %d files matched (from %d candidates)\n",
                target->name, modules_requested, num_files, num_cand);
    } else {
        /* P60: skip files that are $I-included by other files on the
         * list. Otherwise they'd be compiled twice (once standalone,
         * once inline via $I-expansion in the parent), causing symbol
         * duplication / placement drift. */
        static char included_set[256][64];
        int n_included = build_included_set(candidates, num_cand,
                                             included_set, 256);
        int skipped = 0;
        for (int c = 0; c < num_cand; c++) {
            if (file_is_included(candidates[c].path, included_set, n_included)) {
                skipped++;
                continue;
            }
            if (num_files < MAX_SOURCE_FILES)
                files[num_files++] = candidates[c];
        }
        fprintf(stderr, "COMPILE TARGET '%s' [LOOSE]: %d modules requested, %d source files (%d skipped as $I-included)\n",
                target->name, modules_requested, num_files, skipped);
    }
    free(candidates);

    /* Sort files: interface/definition files first, then primitives, then
     * alphabetically.  Files containing "GLOBAL", "DEFS", or "SYSCALL"
     * have INTERFACE declarations that must be processed before their
     * implementation files.  Files containing "PRIM" define primitive types
     * (sdb, mmrb, pcb, etc.) used by higher-level units (MM0, DS0, etc.)
     * and must also be compiled early so their types are available. */
    for (int a = 0; a < num_files - 1; a++)
        for (int b = a + 1; b < num_files; b++) {
            /* Priority: 0 = GLOBAL/DEFS/SYSCALL, 1 = PRIM, 2 = everything else */
            int a_pri = (strcasestr(files[a].path, "GLOBAL") ||
                         strcasestr(files[a].path, "DEFS") ||
                         strcasestr(files[a].path, "SYSCALL")) ? 0 :
                        (strcasestr(files[a].path, "PRIM")) ? 1 : 2;
            int b_pri = (strcasestr(files[b].path, "GLOBAL") ||
                         strcasestr(files[b].path, "DEFS") ||
                         strcasestr(files[b].path, "SYSCALL")) ? 0 :
                        (strcasestr(files[b].path, "PRIM")) ? 1 : 2;
            if (a_pri != b_pri ? (a_pri > b_pri) :
                (strcasecmp(files[a].path, files[b].path) > 0)) {
                source_file_t tmp = files[a]; files[a] = files[b]; files[b] = tmp;
            }
        }

    if (progress) progress("Found source files", 1, 100);

    /* Create linker */
    linker_t *lk = calloc(1, sizeof(linker_t));
    linker_init(lk);

    /* Sort files: STARTUP must be COMPILED LAST (so it sees all other units'
     * exported globals via the shared table) but LINKED FIRST (so the linker
     * places it at $400 where the boot ROM jumps to). */
    int startup_idx = -1;
    for (int i = 0; i < num_files; i++) {
        if (strcasestr(files[i].path, "SOURCE-STARTUP.TEXT") != NULL) {
            startup_idx = i;
            fprintf(stderr, "Boot entry: %s (compiled last, linked first at $400)\n",
                    files[i].path);
            break;
        }
    }

    /* Phase 0: types-only pre-pass. Without this, units that reference
     * types declared in later-compiled units (e.g. source-MMPRIM.TEXT's
     * mmrb field `seg_wait_sem: semaphore` — semaphore is in
     * source-procprims.text which compiles later) produce records
     * whose field types are NULL. Record layout degrades: each unknown
     * field occupies only 2 bytes, so hd_sdscb_list's offset is 14
     * bytes short of the real layout and all its sentinel-init stores
     * land in wrong slots. By pre-running every Pascal file in
     * types-only mode, shared_types is fully populated before the
     * real code-emit pass. */
    for (int i = 0; i < num_files; i++) {
        if (files[i].is_assembly) continue;
        if (i == startup_idx) continue;
        compile_pascal_file_impl(files[i].path, lk, true);
    }
    if (startup_idx >= 0) {
        compile_pascal_file_impl(files[startup_idx].path, lk, true);
    }
    /* P79-diag: check key record sizes after pre-pass */
    for (int ti = 0; ti < num_shared_types; ti++) {
        type_desc_t *t = &shared_types[ti];
        if (t->kind == TK_RECORD && t->num_fields > 0 &&
            (strcasecmp(t->name, "addrdisc") == 0 ||
             strcasecmp(t->name, "discaddr") == 0 ||
             strcasecmp(t->name, "Tsdbtype") == 0 ||
             strcasecmp(t->name, "segHandle") == 0)) {
            fprintf(stderr, "  [TYPE] %s: size=%d fields=%d\n", t->name, t->size, t->num_fields);
            for (int f = 0; f < t->num_fields; f++)
                fprintf(stderr, "    @%d %s sz=%d\n", t->fields[f].offset, t->fields[f].name,
                        t->fields[f].type ? t->fields[f].type->size : -1);
        }
    }
    fprintf(stderr, "Types-only pre-pass complete: %d shared types, %d shared globals (constants)\n",
            num_shared_types, num_shared_globals);

    /* Phase 1: Compile Pascal files — STARTUP last */
    int pascal_count = 0, pascal_ok = 0, pascal_fail = 0;
    for (int i = 0; i < num_files; i++) {
        if (files[i].is_assembly) continue;
        if (i == startup_idx) continue;  /* skip STARTUP for now */
        pascal_count++;
        if (compile_pascal_file(files[i].path, lk)) {
            pascal_ok++;
        } else {
            pascal_fail++;
            if (pascal_fail <= 20) /* Limit output */
                fprintf(stderr, "PASCAL FAIL: %s\n", files[i].path);
        }
        if (progress && pascal_count % 20 == 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Compiling Pascal: %d/%d", pascal_ok, pascal_count);
            progress(msg, 10 + (pascal_count * 30 / num_files), 100);
        }
    }
    /* Now compile STARTUP last — it needs globals from all other units */
    if (startup_idx >= 0) {
        pascal_count++;
        /* Remember how many modules are loaded before STARTUP */
        int modules_before_startup = lk->num_modules;
        if (compile_pascal_file(files[startup_idx].path, lk)) {
            pascal_ok++;
            /* Move STARTUP's module to position 0 so linker places it at $400 */
            if (lk->num_modules > modules_before_startup && modules_before_startup > 0) {
                int startup_old_idx = lk->num_modules - 1;
                link_module_t *startup_mod = lk->modules[startup_old_idx];
                for (int j = lk->num_modules - 1; j > 0; j--)
                    lk->modules[j] = lk->modules[j - 1];
                lk->modules[0] = startup_mod;

                /* Fix symbol table module_idx references after swap.
                 * STARTUP moved from last → 0; all others shifted +1. */
                for (int s = 0; s < lk->num_symbols; s++) {
                    if (lk->symbols[s].module_idx == startup_old_idx) {
                        lk->symbols[s].module_idx = 0;
                    } else if (lk->symbols[s].module_idx >= 0 &&
                               lk->symbols[s].module_idx < startup_old_idx) {
                        lk->symbols[s].module_idx++;
                    }
                }
            }
        } else {
            pascal_fail++;
            fprintf(stderr, "PASCAL FAIL: %s\n", files[startup_idx].path);
        }
    }

    fprintf(stderr, "Pascal: %d/%d succeeded, %d failed\n", pascal_ok, pascal_count, pascal_fail);
    result.files_compiled = pascal_ok;

    /* Phase 2: Assemble assembly files */
    int asm_count = 0, asm_ok = 0;
    for (int i = 0; i < num_files; i++) {
        if (!files[i].is_assembly) continue;
        asm_count++;
        bool aok = assemble_file(files[i].path, lk, source_dir);
        if (aok) asm_ok++;
        else fprintf(stderr, "ASM FAIL: %s\n", files[i].path);
    }
    fprintf(stderr, "Assembly: %d/%d succeeded\n", asm_ok, asm_count);
    result.files_assembled = asm_ok;

    if (progress) progress("Linking...", 60, 100);

    /* Phase 3: Link */
    bool link_ok = linker_link(lk);
    if (link_ok) {
        result.files_linked = lk->num_modules;
    }

    /* Export HLE addresses from linker symbol table (even if link had warnings) */
    {
        struct { const char *name; const char *label; } hle_syms[] = {
            {"CALLDRIVER", "CALLDRIVER"},
            {"CALL_HDIS", "CALL_HDISK"},     /* 8-char truncated */
            {"HDISKIO",   "HDISKIO"},
            {"PRODRIVE",  "PRODRIVER"},       /* 8-char truncated */
            {"SYSTEM_E",  "SYSTEM_ERROR"},   /* 8-char truncated */
            {"BADCALL",   "BADCALL"},
            {"PARALLEL",  "PARALLEL"},
            {"USE_HDIS",  "USE_HDISK"},      /* 8-char truncated */
            {NULL, NULL}
        };
        char hle_path[512];
        snprintf(hle_path, sizeof(hle_path), "%s/hle_addrs.txt", output_dir);
        FILE *hle_f = fopen(hle_path, "w");
        if (hle_f) {
            for (int h = 0; hle_syms[h].name; h++) {
                int idx = -1;
                size_t match_len = strlen(hle_syms[h].name);
                if (match_len > 8) match_len = 8;
                for (int s = 0; s < lk->num_symbols; s++) {
                    if (lk->symbols[s].type == LSYM_ENTRY &&
                        lk->symbols[s].resolved &&
                        strncasecmp(lk->symbols[s].name, hle_syms[h].name, match_len) == 0) {
                        idx = s;
                        break;
                    }
                }
                if (idx >= 0) {
                    fprintf(hle_f, "%s 0x%X\n", hle_syms[h].label, lk->symbols[idx].value);
                    fprintf(stderr, "HLE: %s = $%X\n", hle_syms[h].label, lk->symbols[idx].value);
                    /* Store in globals for direct use by lisa.c */
                    if (strcmp(hle_syms[h].label, "CALLDRIVER") == 0)
                        hle_addr_calldriver = lk->symbols[idx].value;
                    else if (strcmp(hle_syms[h].label, "SYSTEM_ERROR") == 0)
                        hle_addr_system_error = lk->symbols[idx].value;
                } else {
                    fprintf(stderr, "HLE: %s NOT FOUND\n", hle_syms[h].name);
                }
            }
            fclose(hle_f);
        }
    }

    if (progress) progress("Building disk image...", 80, 100);

    /* Phase 4: Build disk image */
    disk_builder_t *db = calloc(1, sizeof(disk_builder_t));
    disk_init(db, PROFILE_5MB_BLOCKS);
    disk_set_volume_name(db, "LisaOS");

    /* Add linked output as system.os and write to boot track */
    uint32_t link_size;
    const uint8_t *link_data = linker_get_output(lk, &link_size);
    if (link_data && link_size > 0) {
        disk_write_boot_track(db, link_data, link_size);
        disk_add_file(db, "system.os", FTYPE_CODE, link_data, link_size);

        /* Save raw linked output for offline disassembly */
        {
            char raw_path[512];
            snprintf(raw_path, sizeof(raw_path), "%s/lisa_linked.bin", output_dir);
            FILE *rf = fopen(raw_path, "wb");
            if (rf) {
                fwrite(link_data, 1, link_size, rf);
                fclose(rf);
                fprintf(stderr, "Linked raw: %s (%u bytes)\n", raw_path, link_size);
            }

            /* Sorted symbol map alongside the raw binary */
            char map_path[512];
            snprintf(map_path, sizeof(map_path), "%s/lisa_linked.map", output_dir);
            FILE *mf = fopen(map_path, "w");
            if (mf) {
                int *order = (int*)malloc(sizeof(int) * lk->num_symbols);
                int n = 0;
                for (int i = 0; i < lk->num_symbols; i++)
                    if (lk->symbols[i].type == LSYM_ENTRY) order[n++] = i;
                for (int i = 1; i < n; i++) {
                    int key = order[i];
                    uint32_t kv = lk->symbols[key].value;
                    int j = i - 1;
                    while (j >= 0 && lk->symbols[order[j]].value > kv) {
                        order[j+1] = order[j]; j--;
                    }
                    order[j+1] = key;
                }
                for (int i = 0; i < n; i++)
                    fprintf(mf, "$%06X  %s\n",
                            lk->symbols[order[i]].value,
                            lk->symbols[order[i]].name);
                fclose(mf);
                free(order);
                fprintf(stderr, "Symbol map: %s (%d entries)\n", map_path, n);
            }
        }

        /* Dump bytes around $B14 (where compiled code crashes with Illegal) */
        if (link_size > 0xB40) {
            fprintf(stderr, "=== LINK OUTPUT BYTES AT $B00-$B40 ===\n");
            fprintf(stderr, "  ");
            for (uint32_t a = 0xB00; a <= 0xB40; a += 2)
                fprintf(stderr, "%04X:%02X%02X ", a, link_data[a], link_data[a+1]);
            fprintf(stderr, "\n");

            for (int m = 0; m < lk->num_modules; m++) {
                link_module_t *mod = lk->modules[m];
                if (!mod->is_kernel) continue;
                if (0xB14 >= mod->base_addr &&
                    0xB14 < mod->base_addr + mod->code_size) {
                    fprintf(stderr, "  $B14 is in module '%s' (base=$%X, size=%u, offset=%u)\n",
                            mod->name, mod->base_addr, mod->code_size,
                            0xB14 - mod->base_addr);
                }
            }
            fprintf(stderr, "=== END ===\n");
        }
    }

    /* Add font files if present */
    char font_dir[512];
    snprintf(font_dir, sizeof(font_dir), "%s/LISA_OS/FONTS", source_dir);
    DIR *fd = opendir(font_dir);
    if (fd) {
        struct dirent *fe;
        while ((fe = readdir(fd)) != NULL) {
            if (fe->d_name[0] == '.') continue;
            char font_path[512];
            snprintf(font_path, sizeof(font_path), "%s/%s", font_dir, fe->d_name);
            disk_add_file_from_path(db, fe->d_name, FTYPE_FONT, font_path);
        }
        closedir(fd);
    }

    disk_finalize(db);

    /* Create output directory if needed */
    mkdir(output_dir, 0755);

    /* Write disk image */
    char image_path[512];
    snprintf(image_path, sizeof(image_path), "%s/lisa_profile.image", output_dir);
    disk_write_image(db, image_path);
    strncpy(result.output_path, image_path, sizeof(result.output_path) - 1);

    /* Phase 5: Generate boot ROM */
    if (progress) progress("Generating boot ROM...", 90, 100);
    uint8_t *rom = bootrom_generate();
    if (rom) {
        char rom_path[512];
        snprintf(rom_path, sizeof(rom_path), "%s/lisa_boot.rom", output_dir);
        FILE *rf = fopen(rom_path, "wb");
        if (rf) {
            fwrite(rom, 1, ROM_SIZE, rf);
            fclose(rf);
            fprintf(stderr, "Boot ROM: %s (%d bytes)\n", rom_path, ROM_SIZE);
        }
        free(rom);
    }

    disk_free(db);
    free(db);
    linker_free(lk);
    free(lk);
    free(files);

    result.success = (result.files_compiled > 0);
    result.errors = 0;

    if (progress) progress("Build complete!", 100, 100);

    return result;
}

const char *toolchain_get_artifact(const char *output_dir, const char *artifact) {
    static char path[512];
    if (strcmp(artifact, "profile") == 0) {
        snprintf(path, sizeof(path), "%s/lisa_profile.image", output_dir);
    } else if (strcmp(artifact, "rom") == 0) {
        snprintf(path, sizeof(path), "%s/lisa_rom.bin", output_dir);
    } else if (strcmp(artifact, "floppy") == 0) {
        snprintf(path, sizeof(path), "%s/lisa_floppy.image", output_dir);
    } else {
        path[0] = '\0';
    }
    return path;
}
