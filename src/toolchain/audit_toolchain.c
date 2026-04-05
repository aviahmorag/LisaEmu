/*
 * Toolchain Audit — Unified diagnostic tool
 *
 * Runs the full Lisa OS toolchain pipeline (parse → codegen → assemble → link)
 * and reports detailed per-stage statistics. Use this to measure progress
 * and identify what needs fixing.
 *
 * Usage:
 *   audit_toolchain <Lisa_Source_root>          # Full report (all stages)
 *   audit_toolchain <Lisa_Source_root> parser    # Stage 1 only
 *   audit_toolchain <Lisa_Source_root> codegen   # Stage 2 only
 *   audit_toolchain <Lisa_Source_root> asm       # Stage 3 only
 *   audit_toolchain <Lisa_Source_root> linker    # Stage 4 only (full pipeline)
 *
 * Output: summary to stdout, detailed errors to stderr.
 * Pipe stdout to a file for tracking over time.
 */

#include "pascal_parser.h"
#include "pascal_codegen.h"
#include "asm68k.h"
#include "linker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* Forward declarations */
static void set_parser_source_root(parser_t *p);

/* ========================================================================
 * File discovery (shared across all stages)
 * ======================================================================== */

typedef struct {
    char path[512];
    bool is_assembly;
} source_file_t;

static bool should_skip_dir(const char *name) {
    if (strcasecmp(name, "BUILD") == 0) return true;
    if (strcasecmp(name, "OS exec files") == 0) return true;
    if (strcasecmp(name, "Linkmaps 3.0") == 0) return true;
    if (strcasestr(name, "Linkmaps") != NULL) return true;
    if (strcasecmp(name, "Fonts") == 0) return true;
    if (strcasecmp(name, "APPS") == 0) return true;
    if (strcasecmp(name, "Lisa_Toolkit") == 0) return true;
    return false;
}

static bool should_skip_file(const char *name) {
    /* Build scripts, documentation, non-source data */
    static const char *skip_patterns[] = {
        "INSTRUCT", "RELEASE", "relmemo",
        "T5LM", "t5dbc", "t8dialogs", "t10menus", "T10DBOX",
        "-TABLES.TEXT", "FASTLINE", "LINE2", "GRAFTYPES",
        "PABC", "PASGEN", "phquickport", "INITFPFILE",
        "qpsample", "qpmake", "make_qp", "link_qp",
        "lnewFPLIB", "buildpref", "MAKEHEUR",
        "-LIST.TEXT", "-SIZES.TEXT", "-EXEC.TEXT",
        "LETTERCODES", "KEYWORDS", "FKEYWORDS",
        /* MENUS.TEXT and DBOX.TEXT removed: caught LIBWM-MENUS (real source) */
        "CNBUILD", "CIBUILD", "BUILDPR",
        "DWBTN", "ciBTN", "PARBTN", "CNBTN",
        "PASLIBDOC", "PASLIBCDOC", "linkmap-",
        /* LIBHW include fragments — assembled via DRIVERS.TEXT master */
        "libhw-CURSOR", "libhw-KEYBD", "libhw-LEGENDS",
        "libhw-MACHINE", "libhw-MOUSE", "libhw-SPRKEYBD", "libhw-TIMERS",
        NULL
    };
    for (int i = 0; skip_patterns[i]; i++) {
        if (strcasestr(name, skip_patterns[i]) != NULL) return true;
    }
    /* LIBFP handled in find_source_files after content detection */
    /* LIBQD: STRETCH was incorrectly skipped — it's standalone assembly */
    return false;
}

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
            const char *ext = strcasestr(entry->d_name, ".TEXT.unix.txt");
            if (!ext) continue;
            if (should_skip_file(entry->d_name)) continue;
            /* Detect assembly vs Pascal */
            bool is_asm = (strcasestr(entry->d_name, "ASM") != NULL) ||
                          (strcasestr(entry->d_name, "QSORT") != NULL);
            if (!is_asm) {
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
                        if (strncasecmp(p, "PAGE", 4) == 0 && (p[4] == '\0' || p[4] == ' ' || p[4] == '\n' || p[4] == '\r'))
                            continue;  /* Skip PAGE, check next line */
                        break;  /* Non-asm line, stop */
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

static const char *basename_of(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* ========================================================================
 * AST counting helpers
 * ======================================================================== */

static int count_nodes(ast_node_t *node, ast_type_t type) {
    if (!node) return 0;
    int n = (node->type == type) ? 1 : 0;
    for (int i = 0; i < node->num_children; i++)
        n += count_nodes(node->children[i], type);
    return n;
}

static int count_all_nodes(ast_node_t *node) {
    if (!node) return 0;
    int n = 1;
    for (int i = 0; i < node->num_children; i++)
        n += count_all_nodes(node->children[i]);
    return n;
}

/* ========================================================================
 * Stage 1: Parser Audit
 * ======================================================================== */

static void audit_parser(source_file_t *files, int num_files) {
    printf("========================================\n");
    printf("STAGE 1: PARSER\n");
    printf("========================================\n\n");

    int total = 0, ok = 0, fail = 0, total_errors = 0;
    int units = 0, programs = 0, fragments = 0;
    int total_nodes = 0;

    for (int i = 0; i < num_files; i++) {
        if (files[i].is_assembly) continue;
        total++;

        char *source = read_file(files[i].path);
        if (!source) { fail++; continue; }

        parser_t parser;
        parser_init(&parser, source, files[i].path);
        set_parser_source_root(&parser);
        ast_node_t *ast = parser_parse(&parser);

        if (parser.num_errors == 0) {
            ok++;
            if (ast) {
                total_nodes += count_all_nodes(ast);
                if (ast->type == AST_UNIT) units++;
                else if (ast->type == AST_PROGRAM) programs++;
                else fragments++;
            }
        } else {
            fail++;
            total_errors += parser.num_errors;
            /* Only log errors for files that have code (not empty build scripts) */
            if (ast && count_all_nodes(ast) > 1) {
                fprintf(stderr, "PARSER FAIL: %s (%d errors)\n",
                        basename_of(files[i].path), parser.num_errors);
                int show = parser.num_errors < 5 ? parser.num_errors : 5;
                for (int e = 0; e < show; e++)
                    fprintf(stderr, "  %s\n", parser.errors[e]);
            }
        }

        parser_free(&parser);
        free(source);
    }

    printf("Pascal files:   %d\n", total);
    printf("Parse OK:       %d (%.1f%%)\n", ok, total ? 100.0 * ok / total : 0);
    printf("Parse FAIL:     %d (%.1f%%)\n", fail, total ? 100.0 * fail / total : 0);
    printf("Total errors:   %d\n", total_errors);
    printf("AST nodes:      %d\n", total_nodes);
    printf("Units: %d  Programs: %d  Fragments: %d\n\n", units, programs, fragments);
}

/* ========================================================================
 * Stage 2: Codegen Audit
 * ======================================================================== */

/* Shared globals for cross-unit imports */
#define MAX_SHARED_GLOBALS 32768
static cg_symbol_t shared_globals[MAX_SHARED_GLOBALS];
static int num_shared_globals = 0;

/* Shared types for cross-unit type casts */
#define MAX_SHARED_TYPES 8192
static type_desc_t shared_types[MAX_SHARED_TYPES];
static int num_shared_types = 0;

/* Track relocation targets */
#define MAX_TARGETS 65536
typedef struct { char name[64]; int refs; bool defined; } target_t;
static target_t targets[MAX_TARGETS];
static int num_targets = 0;

static target_t *find_or_add_target(const char *name) {
    for (int i = 0; i < num_targets; i++)
        if (strcasecmp(targets[i].name, name) == 0) return &targets[i];
    if (num_targets >= MAX_TARGETS) return NULL;
    target_t *t = &targets[num_targets++];
    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, sizeof(t->name) - 1);
    return t;
}

static void audit_codegen(source_file_t *files, int num_files) {
    printf("========================================\n");
    printf("STAGE 2: CODEGEN\n");
    printf("========================================\n\n");

    /* Reset shared state */
    num_shared_globals = 0;
    num_targets = 0;

    int startup_idx = -1;
    for (int i = 0; i < num_files; i++) {
        if (!files[i].is_assembly && strcasestr(files[i].path, "SOURCE-STARTUP.TEXT"))
            startup_idx = i;
    }

    int total = 0, total_code = 0, total_globals = 0, total_relocs = 0;

    /* Helper: compile one Pascal file */
    #define COMPILE_PASCAL(idx) do { \
        char *src = read_file(files[idx].path); \
        if (!src) break; \
        parser_t p; parser_init(&p, src, files[idx].path); \
        set_parser_source_root(&p); \
        ast_node_t *ast = parser_parse(&p); \
        codegen_t *cg = calloc(1, sizeof(codegen_t)); \
        codegen_init(cg); \
        strncpy(cg->current_file, files[idx].path, sizeof(cg->current_file)-1); \
        cg->imported_globals = shared_globals; \
        cg->imported_globals_count = num_shared_globals; \
        cg->imported_types = shared_types; \
        cg->imported_types_count = num_shared_types; \
        codegen_generate(cg, ast); \
        total++; total_code += cg->code_size; \
        total_globals += cg->num_globals; total_relocs += cg->num_relocs; \
        for (int g = 0; g < cg->num_globals; g++) { \
            if (!cg->globals[g].is_external && !cg->globals[g].is_forward) { \
                target_t *t = find_or_add_target(cg->globals[g].name); \
                if (t) t->defined = true; \
            } \
        } \
        for (int r = 0; r < cg->num_relocs; r++) { \
            target_t *t = find_or_add_target(cg->relocs[r].symbol); \
            if (t) t->refs++; \
        } \
        for (int g = 0; g < cg->num_globals && num_shared_globals < MAX_SHARED_GLOBALS; g++) { \
            if (!cg->globals[g].is_external && !cg->globals[g].is_forward) \
                shared_globals[num_shared_globals++] = cg->globals[g]; \
        } \
        for (int t2 = 0; t2 < cg->num_types && num_shared_types < MAX_SHARED_TYPES; t2++) { \
            if (cg->types[t2].name[0]) \
                shared_types[num_shared_types++] = cg->types[t2]; \
        } \
        /* Log files with unresolved relocations to stderr */ \
        if (cg->num_relocs > 0) { \
            int unres = 0; \
            for (int r2 = 0; r2 < cg->num_relocs; r2++) { \
                bool found2 = false; \
                for (int g2 = 0; g2 < cg->num_globals; g2++) { \
                    if (strcasecmp(cg->globals[g2].name, cg->relocs[r2].symbol) == 0) \
                        { found2 = true; break; } \
                } \
                if (!found2) unres++; \
            } \
            if (unres > 5) \
                fprintf(stderr, "  CODEGEN: %s — %u bytes, %d globals, %d relocs (%d unresolved)\n", \
                        basename_of(files[idx].path), cg->code_size, cg->num_globals, cg->num_relocs, unres); \
        } \
        codegen_free(cg); free(cg); parser_free(&p); free(src); \
    } while(0)

    for (int i = 0; i < num_files; i++) {
        if (files[i].is_assembly || i == startup_idx) continue;
        COMPILE_PASCAL(i);
    }
    if (startup_idx >= 0) COMPILE_PASCAL(startup_idx);

    int resolved = 0, unresolved = 0, resolved_refs = 0, unresolved_refs = 0;
    for (int i = 0; i < num_targets; i++) {
        if (targets[i].defined) { resolved++; resolved_refs += targets[i].refs; }
        else { unresolved++; unresolved_refs += targets[i].refs; }
    }

    printf("Files compiled:     %d\n", total);
    printf("Total code bytes:   %d (%.1f KB)\n", total_code, total_code / 1024.0);
    printf("Total globals:      %d\n", total_globals);
    printf("Shared globals:     %d\n", num_shared_globals);
    printf("Total relocations:  %d\n", total_relocs);
    printf("Unique targets:     %d\n", num_targets);
    printf("  Resolved:         %d symbols → %d refs\n", resolved, resolved_refs);
    printf("  UNRESOLVED:       %d symbols → %d refs\n", unresolved, unresolved_refs);
    printf("  Resolution rate:  %.1f%% symbols, %.1f%% refs\n",
           num_targets > 0 ? 100.0 * resolved / num_targets : 0,
           total_relocs > 0 ? 100.0 * resolved_refs / total_relocs : 0);

    /* Top 20 unresolved */
    printf("\nTop 20 unresolved symbols:\n");
    bool used[MAX_TARGETS]; memset(used, 0, sizeof(used));
    for (int rank = 0; rank < 20; rank++) {
        int best = -1, best_c = 0;
        for (int i = 0; i < num_targets; i++) {
            if (used[i] || targets[i].defined) continue;
            if (targets[i].refs > best_c) { best = i; best_c = targets[i].refs; }
        }
        if (best < 0) break;
        used[best] = true;
        printf("  %4d refs  %s\n", targets[best].refs, targets[best].name);
    }
    printf("\n");
}

/* ========================================================================
 * Stage 3: Assembler Audit
 * ======================================================================== */

static void audit_asm(source_file_t *files, int num_files, const char *root) {
    printf("========================================\n");
    printf("STAGE 3: ASSEMBLER\n");
    printf("========================================\n\n");

    int total = 0, ok = 0, fail = 0, total_errors = 0;
    int total_code = 0, total_defs = 0, total_procs = 0;

    for (int i = 0; i < num_files; i++) {
        if (!files[i].is_assembly) continue;
        total++;

        asm68k_t *as = calloc(1, sizeof(asm68k_t));
        asm68k_init(as);

        char *slash = strrchr(files[i].path, '/');
        if (slash) {
            char dir[256];
            size_t len = (size_t)(slash - files[i].path);
            if (len >= sizeof(dir)) len = sizeof(dir) - 1;
            memcpy(dir, files[i].path, len); dir[len] = '\0';
            asm68k_set_base_dir(as, dir);
        }
        char inc[512];
        snprintf(inc, sizeof(inc), "%s/LISA_OS/LIBS", root);
        asm68k_add_include_path(as, inc);
        snprintf(inc, sizeof(inc), "%s/LISA_OS/OS", root);
        asm68k_add_include_path(as, inc);

        bool aok = asm68k_assemble_file(as, files[i].path);

        uint32_t code_bytes = 0;
        for (int s = 0; s < as->num_sections; s++)
            code_bytes += as->sections[s].size;

        if (aok) { ok++; total_code += code_bytes; }
        else {
            fail++; total_errors += as->num_errors;
            fprintf(stderr, "ASM FAIL: %s (%d errors)\n",
                    basename_of(files[i].path), as->num_errors);
            int show = as->num_errors < 3 ? as->num_errors : 3;
            for (int e = 0; e < show; e++)
                fprintf(stderr, "  %s\n", as->errors[e].message);
        }

        for (int s = 0; s < as->num_symbols; s++) {
            if (as->symbols[s].exported) total_defs++;
            if (as->symbols[s].type == SYM_PROC || as->symbols[s].type == SYM_FUNC) total_procs++;
        }

        asm68k_free(as); free(as);
    }

    printf("Assembly files:    %d\n", total);
    printf("Assemble OK:       %d (%.1f%%)\n", ok, total ? 100.0 * ok / total : 0);
    printf("Assemble FAIL:     %d (%.1f%%)\n", fail, total ? 100.0 * fail / total : 0);
    printf("Total errors:      %d\n", total_errors);
    printf("Total code bytes:  %d (%.1f KB)\n", total_code, total_code / 1024.0);
    printf("Total .DEF exports: %d\n", total_defs);
    printf("Total .PROC/.FUNC: %d\n\n", total_procs);
}

/* ========================================================================
 * Stage 4: Linker Audit (full pipeline)
 * ======================================================================== */

static void audit_linker(source_file_t *files, int num_files, const char *root) {
    printf("========================================\n");
    printf("STAGE 4: LINKER (full pipeline)\n");
    printf("========================================\n\n");

    /* Reset shared globals */
    num_shared_globals = 0;

    linker_t *lk = calloc(1, sizeof(linker_t));
    linker_init(lk);

    int startup_idx = -1;
    for (int i = 0; i < num_files; i++) {
        if (!files[i].is_assembly && strcasestr(files[i].path, "SOURCE-STARTUP.TEXT"))
            startup_idx = i;
    }

    /* Compile Pascal */
    int pascal_ok = 0, pascal_skip = 0;
    for (int i = 0; i < num_files; i++) {
        if (files[i].is_assembly || i == startup_idx) continue;
        char *src = read_file(files[i].path);
        if (!src) continue;
        parser_t p; parser_init(&p, src, files[i].path);
        set_parser_source_root(&p);
        ast_node_t *ast = parser_parse(&p);
        codegen_t *cg = calloc(1, sizeof(codegen_t));
        codegen_init(cg);
        strncpy(cg->current_file, files[i].path, sizeof(cg->current_file)-1);
        cg->imported_globals = shared_globals;
        cg->imported_globals_count = num_shared_globals;
        cg->imported_types = shared_types;
        cg->imported_types_count = num_shared_types;
        codegen_generate(cg, ast);
        for (int g = 0; g < cg->num_globals && num_shared_globals < MAX_SHARED_GLOBALS; g++) {
            if (!cg->globals[g].is_external && !cg->globals[g].is_forward)
                shared_globals[num_shared_globals++] = cg->globals[g];
        }
        for (int ti = 0; ti < cg->num_types && num_shared_types < MAX_SHARED_TYPES; ti++) {
            if (cg->types[ti].name[0])
                shared_types[num_shared_types++] = cg->types[ti];
        }
        if (cg->code_size > 0) {
            linker_load_codegen(lk, files[i].path, cg->code, cg->code_size,
                                cg->globals, cg->num_globals, cg->relocs, cg->num_relocs);
            pascal_ok++;
        } else { pascal_skip++; }
        codegen_free(cg); free(cg); parser_free(&p); free(src);
    }

    /* STARTUP last */
    if (startup_idx >= 0) {
        char *src = read_file(files[startup_idx].path);
        if (src) {
            parser_t p; parser_init(&p, src, files[startup_idx].path);
            set_parser_source_root(&p);
            ast_node_t *ast = parser_parse(&p);
            codegen_t *cg = calloc(1, sizeof(codegen_t));
            codegen_init(cg);
            strncpy(cg->current_file, files[startup_idx].path, sizeof(cg->current_file)-1);
            cg->imported_globals = shared_globals;
            cg->imported_globals_count = num_shared_globals;
            cg->imported_types = shared_types;
            cg->imported_types_count = num_shared_types;
            codegen_generate(cg, ast);
            int mb = lk->num_modules;
            if (cg->code_size > 0) {
                linker_load_codegen(lk, files[startup_idx].path, cg->code, cg->code_size,
                                    cg->globals, cg->num_globals, cg->relocs, cg->num_relocs);
                pascal_ok++;
                if (lk->num_modules > mb && mb > 0) {
                    link_module_t *sm = lk->modules[lk->num_modules - 1];
                    for (int j = lk->num_modules - 1; j > 0; j--)
                        lk->modules[j] = lk->modules[j - 1];
                    lk->modules[0] = sm;
                }
            }
            codegen_free(cg); free(cg); parser_free(&p); free(src);
        }
    }

    /* Assemble */
    int asm_ok = 0, asm_fail = 0;
    for (int i = 0; i < num_files; i++) {
        if (!files[i].is_assembly) continue;
        asm68k_t *as = calloc(1, sizeof(asm68k_t));
        asm68k_init(as);
        char *slash = strrchr(files[i].path, '/');
        if (slash) {
            char dir[256];
            size_t len = (size_t)(slash - files[i].path);
            if (len >= sizeof(dir)) len = sizeof(dir) - 1;
            memcpy(dir, files[i].path, len); dir[len] = '\0';
            asm68k_set_base_dir(as, dir);
        }
        char inc[512];
        snprintf(inc, sizeof(inc), "%s/LISA_OS/LIBS", root);
        asm68k_add_include_path(as, inc);
        snprintf(inc, sizeof(inc), "%s/LISA_OS/OS", root);
        asm68k_add_include_path(as, inc);
        bool aok = asm68k_assemble_file(as, files[i].path);
        /* Debug traces removed — use diag_linker.c for targeted debugging */
        if (aok && as->output_size > 0) {
            int ne = 0;
            for (int s = 0; s < as->num_symbols; s++)
                if (as->symbols[s].exported || as->symbols[s].external ||
                    as->symbols[s].type == SYM_PROC || as->symbols[s].type == SYM_FUNC) ne++;
            cg_symbol_t *syms = calloc(ne + 1, sizeof(cg_symbol_t));
            cg_reloc_t *rels = calloc(as->num_relocs + 1, sizeof(cg_reloc_t));
            int ns = 0, nr = 0;
            for (int s = 0; s < as->num_symbols; s++) {
                if (!as->symbols[s].exported && !as->symbols[s].external &&
                    as->symbols[s].type != SYM_PROC && as->symbols[s].type != SYM_FUNC) continue;
                strncpy(syms[ns].name, as->symbols[s].name, 63);
                syms[ns].offset = as->symbols[s].value;
                syms[ns].is_global = as->symbols[s].exported ||
                    as->symbols[s].type == SYM_PROC || as->symbols[s].type == SYM_FUNC;
                syms[ns].is_external = as->symbols[s].external;
                ns++;
            }
            for (int r = 0; r < as->num_relocs; r++) {
                rels[nr].offset = as->relocs[r].offset;
                rels[nr].size = as->relocs[r].size;
                rels[nr].pc_relative = as->relocs[r].pc_relative;
                if (as->relocs[r].symbol_idx >= 0 && as->relocs[r].symbol_idx < as->num_symbols)
                    strncpy(rels[nr].symbol, as->symbols[as->relocs[r].symbol_idx].name, 63);
                nr++;
            }
            linker_load_codegen(lk, files[i].path,
                                asm68k_get_output(as, NULL), as->output_size,
                                syms, ns, rels, nr);
            free(syms); free(rels);
            asm_ok++;
        } else { asm_fail++; }
        asm68k_free(as); free(as);
    }

    /* Link */
    int total_relocs = 0;
    for (int m = 0; m < lk->num_modules; m++)
        total_relocs += lk->modules[m]->num_relocs;

    bool link_ok = linker_link(lk);

    int resolved = 0, unresolved = 0;
    for (int i = 0; i < lk->num_symbols; i++) {
        if (lk->symbols[i].resolved) resolved++;
        else unresolved++;
    }

    printf("Modules:     %d (Pascal: %d, Assembly: %d, skip: %d, asm-fail: %d)\n",
           lk->num_modules, pascal_ok, asm_ok, pascal_skip, asm_fail);
    printf("Symbols:     %d total, %d resolved, %d unresolved\n",
           lk->num_symbols, resolved, unresolved);
    printf("Relocations: %d\n", total_relocs);
    printf("Link OK:     %s\n", link_ok ? "YES" : "NO");
    printf("Output size: %u bytes (%.1f KB)\n", lk->output_size, lk->output_size / 1024.0);

    /* JSR analysis */
    if (lk->output && lk->output_size > 0x400) {
        uint32_t last_end = 0x400;
        for (int m = 0; m < lk->num_modules; m++) {
            uint32_t end = lk->modules[m]->base_addr + lk->modules[m]->code_size;
            if (end > last_end) last_end = end;
        }
        if (last_end & 1) last_end++;
        uint32_t stub = last_end;

        int jsr_total = 0, jsr_real = 0, jsr_stub = 0;
        for (uint32_t i = 0x400; i + 5 < lk->output_size; i += 2) {
            if (lk->output[i] == 0x4E && lk->output[i+1] == 0xB9) {
                uint32_t t = ((uint32_t)lk->output[i+2]<<24) | ((uint32_t)lk->output[i+3]<<16) |
                             ((uint32_t)lk->output[i+4]<<8) | lk->output[i+5];
                jsr_total++;
                if (t == stub) jsr_stub++;
                else if (t >= 0x400 && t < lk->output_size) jsr_real++;
            }
        }
        printf("\nJSR analysis:\n");
        printf("  Total JSR abs.L:  %d\n", jsr_total);
        printf("  → real code:      %d (%.1f%%)\n", jsr_real,
               jsr_total > 0 ? 100.0 * jsr_real / jsr_total : 0);
        printf("  → stub (unresolved): %d (%.1f%%)\n", jsr_stub,
               jsr_total > 0 ? 100.0 * jsr_stub / jsr_total : 0);
        printf("  → other:          %d\n", jsr_total - jsr_real - jsr_stub);
    }
    printf("\n");

    linker_free(lk); free(lk);
}

/* Set source_root on parser for {$I} include resolution */
static void set_parser_source_root(parser_t *p) {
    const char *marker = strcasestr(p->lex.filename, "/LISA_OS/");
    if (marker) {
        size_t rlen = (size_t)(marker - p->lex.filename);
        if (rlen < sizeof(p->lex.source_root)) {
            memcpy(p->lex.source_root, p->lex.filename, rlen);
            p->lex.source_root[rlen] = '\0';
        }
    }
}

/* ========================================================================
 * File sorting: units before fragments
 * ======================================================================== */

/* Extract a sort key from a filename that puts base units before fragments.
 * "libtk-UABC.TEXt" → "libtk-uabc" (no digit suffix)
 * "LIBTK-UABC2.TEXT" → "libtk-uabc2" (has digit suffix, sorts after)
 * Case-insensitive. */
static int compare_source_files(const void *a, const void *b) {
    const source_file_t *fa = (const source_file_t *)a;
    const source_file_t *fb = (const source_file_t *)b;
    /* Compare case-insensitive */
    return strcasecmp(fa->path, fb->path);
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: audit_toolchain <Lisa_Source_root> [parser|codegen|asm|linker]\n");
        return 1;
    }

    const char *root = argv[1];
    const char *stage = (argc > 2) ? argv[2] : "all";

    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/LISA_OS", root);

    source_file_t *files = calloc(2048, sizeof(source_file_t));
    int num_files = find_source_files(subdir, files, 2048);

    /* Sort Pascal files: base units before numbered fragments.
     * e.g., libtk-UABC.TEXt before LIBTK-UABC2.TEXT
     * This ensures types defined in UNIT INTERFACE are available
     * when compiling continuation fragments. */
    qsort(files, num_files, sizeof(source_file_t), compare_source_files);

    int n_pascal = 0, n_asm = 0;
    for (int i = 0; i < num_files; i++) {
        if (files[i].is_assembly) n_asm++; else n_pascal++;
    }

    printf("========================================\n");
    printf("TOOLCHAIN AUDIT\n");
    printf("========================================\n");
    printf("Source: %s\n", root);
    printf("Files:  %d total (%d Pascal, %d Assembly)\n\n", num_files, n_pascal, n_asm);

    bool all = (strcmp(stage, "all") == 0);

    if (all || strcmp(stage, "parser") == 0)
        audit_parser(files, num_files);

    if (all || strcmp(stage, "codegen") == 0)
        audit_codegen(files, num_files);

    if (all || strcmp(stage, "asm") == 0)
        audit_asm(files, num_files, root);

    if (all || strcmp(stage, "linker") == 0)
        audit_linker(files, num_files, root);

    free(files);
    return 0;
}
