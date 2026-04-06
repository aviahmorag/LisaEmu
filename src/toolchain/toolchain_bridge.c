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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* ========================================================================
 * File discovery
 * ======================================================================== */

#define MAX_SOURCE_FILES 2048

typedef struct {
    char path[512];
    bool is_assembly;  /* true = assembly, false = Pascal */
} source_file_t;

/* Check if a directory name should be skipped (non-source directories) */
static bool should_skip_dir(const char *name) {
    if (strcasecmp(name, "BUILD") == 0) return true;
    if (strcasestr(name, "Linkmaps") != NULL) return true;
    if (strcasestr(name, "exec") != NULL) return true;
    if (strcasecmp(name, "DICT") == 0) return true;
    if (strcasecmp(name, "FONTS") == 0) return true;
    if (strcasecmp(name, "APIN") == 0) return true;  /* install scripts */
    if (strcasecmp(name, "GUIDE_APIM") == 0) return true; /* guide/tutorial app */
    if (strcasecmp(name, "TKIN") == 0) return true;  /* toolkit install */
    /* LIBHW directory is in LIBS/ — don't skip it. Only skip if it's a duplicate at the top level. */
    /* (Previously this was broken: name is just "LIBHW" which never contains "LIBS") */
    /* Skip TK3/TK4/TK5 sample app directories (contain build scripts, not compilable units) */
    if (strcasecmp(name, "TK3") == 0 || strcasecmp(name, "TK4") == 0 || strcasecmp(name, "TK5") == 0) return true;

    return false;
}

/* Check if a file is likely a non-Pascal/non-assembly file that should be skipped */
static bool should_skip_file(const char *name) {
    /* Skip build scripts, link lists, alert tables, 68k assembly stubs, docs */
    if (strncasecmp(name, "BUILD-", 6) == 0) return true;
    if (strncasecmp(name, "build-", 6) == 0) return true;
    if (strcasestr(name, "ALERT") != NULL) return true;  /* alert resource files */
    if (strcasestr(name, "LINK.TEXT") != NULL) return true; /* link command files */
    if (strcasestr(name, "linkmap") != NULL) return true;
    /* 68K.TEXT files are valid assembly — no longer skipped */
    if (strcasestr(name, "COMP.TEXT") != NULL) return true; /* compile scripts */
    if (strcasestr(name, "INSTALL.TEXT") != NULL) return true;
    if (strcasestr(name, "DOC.TEXT") != NULL) return true;
    if (strcasestr(name, "REL.TEXT") != NULL && strncasecmp(name, "libhw-REL", 9) == 0) return true;
    if (strcasestr(name, "APPENDIX") != NULL) return true;
    /* LIBHW include fragments — assembled via DRIVERS.TEXT master */
    if (strcasestr(name, "libhw-CURSOR") != NULL) return true;
    if (strcasestr(name, "libhw-KEYBD") != NULL) return true;
    if (strcasestr(name, "libhw-LEGENDS") != NULL) return true;
    if (strcasestr(name, "libhw-MACHINE") != NULL) return true;
    if (strcasestr(name, "libhw-MOUSE") != NULL) return true;
    if (strcasestr(name, "libhw-SPRKEYBD") != NULL) return true;
    if (strcasestr(name, "libhw-TIMERS") != NULL) return true;
    if (strcasestr(name, "INSTRUCT") != NULL) return true;
    if (strcasestr(name, "RELEASE") != NULL) return true;
    /* LIBFP: assembly include fragments are assembled via NEWFPSUB master.
     * Pascal files in LIBFP should be compiled normally.
     * Content detection handles asm vs Pascal classification. */
    /* QSORT is assembly but starts with EQU constants — force assembly detection below */
    /* Documentation / release notes */
    if (strcasestr(name, "relmemo") != NULL) return true;
    /* Non-source data files in APPS */
    if (strcasestr(name, "T5LM") != NULL) return true;
    if (strcasestr(name, "t5dbc") != NULL) return true;
    if (strcasestr(name, "t8dialogs") != NULL) return true;
    if (strcasestr(name, "t10menus") != NULL) return true;
    if (strcasestr(name, "T10DBOX") != NULL) return true;
    if (strcasestr(name, "-TABLES.TEXT") != NULL) return true;
    /* LIBQD include fragments (assembled via DRAWLINE master file) */
    if (strcasestr(name, "FASTLINE") != NULL) return true;
    if (strcasestr(name, "LINE2") != NULL) return true;
    if (strcasestr(name, "GRAFTYPES") != NULL) return true;
    /* LIBQD: STRETCH is standalone assembly, not an include fragment */
    /* Phrase/resource files, documentation, code generator templates */
    if (strcasestr(name, "PABC") != NULL) return true;
    if (strcasestr(name, "PASGEN") != NULL) return true;
    if (strcasestr(name, "phquickport") != NULL) return true;
    if (strcasestr(name, "INITFPFILE") != NULL) return true;
    if (strcasestr(name, "qpsample") != NULL) return true;
    if (strcasestr(name, "qpmake") != NULL) return true;
    if (strcasestr(name, "make_qp") != NULL) return true;
    if (strcasestr(name, "link_qp") != NULL) return true;
    if (strcasestr(name, "lnewFPLIB") != NULL) return true;
    if (strcasestr(name, "buildpref") != NULL) return true;
    if (strcasestr(name, "MAKEHEUR") != NULL) return true;
    /* Data/list files that aren't source code */
    if (strcasestr(name, "-LIST.TEXT") != NULL) return true;
    if (strcasestr(name, "-SIZES.TEXT") != NULL) return true;
    if (strcasestr(name, "-EXEC.TEXT") != NULL) return true;
    if (strcasestr(name, "LETTERCODES") != NULL) return true;
    if (strcasestr(name, "KEYWORDS") != NULL) return true;
    if (strcasestr(name, "FKEYWORDS") != NULL) return true;
    /* Note: MENUS.TEXT and DBOX.TEXT patterns removed — they were catching
     * LIBWM-MENUS.TEXT (real source with GetItem, CheckItem, etc.).
     * App-level menu/dialog data files are already excluded by skip_dir(APPS). */
    if (strcasestr(name, "CNBUILD") != NULL) return true;
    if (strcasestr(name, "CIBUILD") != NULL) return true;
    if (strcasestr(name, "BUILDPR") != NULL) return true;
    if (strcasestr(name, "DWBTN") != NULL) return true;
    if (strcasestr(name, "ciBTN") != NULL) return true;
    if (strcasestr(name, "PARBTN") != NULL) return true;
    if (strcasestr(name, "CNBTN") != NULL) return true;
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
    /* From LisaSourceCompilation ALEX-LINK-SYSTEMOS.TEXT:
     * system.os contains exactly 46 modules from SOURCE/ + HWINTL + PASMATH.
     * NO libraries. Libraries are loaded as intrinsic libs from disk.
     *
     * But we also need LIBPL (Pascal runtime) and LIBHW (hardware interface)
     * because on the real Lisa these are in IOSPASLIB.OBJ and SYSTEM.LLD
     * which are loaded by the boot loader before the OS starts. */

    /* OS source files (the 46 kernel modules) */
    if (strcasestr(path, "/OS/") != NULL) return true;

    /* LIBPL — Pascal runtime (normally IOSPASLIB.OBJ, loaded by boot loader) */
    if (strcasestr(path, "LIBPL") != NULL) return true;

    /* LIBHW — Hardware interface (normally SYSTEM.LLD, loaded before MMU) */
    if (strcasestr(path, "LIBHW") != NULL) return true;

    /* LIBFP — Floating point (normally IOSFPLIB.OBJ, loaded early) */
    if (strcasestr(path, "LIBFP") != NULL) return true;

    /* LIBOS — OS syscall interface (part of kernel interface) */
    if (strcasestr(path, "LIBOS") != NULL) return true;

    /* Everything else (apps, UI libs) → non-kernel */
    return false;
}

/* Shared types accumulated from all previously compiled units */
#define MAX_SHARED_TYPES 8192
static type_desc_t shared_types[MAX_SHARED_TYPES];
static int num_shared_types = 0;

static bool compile_pascal_file(const char *path, linker_t *lk) {
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

    if (is_startup && ast) {
        fprintf(stderr, "STARTUP AST: type=%d name='%s' children=%d errors=%d\n",
                ast->type, ast->name, ast->num_children, parser.num_errors);
        for (int i = 0; i < ast->num_children && i < 40; i++) {
            ast_node_t *c = ast->children[i];
            fprintf(stderr, "  child[%d]: type=%d name='%s' children=%d\n",
                    i, c->type, c->name, c->num_children);
            /* For INITSYS, dump its nested declarations */
            if ((c->type == 11 || c->type == 12) &&
                strcasecmp(c->name, "INITSYS") == 0) {
                for (int j = 0; j < c->num_children && j < 45; j++) {
                    ast_node_t *ic = c->children[j];
                    fprintf(stderr, "    INITSYS child[%d]: type=%d name='%s' children=%d\n",
                            j, ic->type, ic->name, ic->num_children);
                }
            }
        }
    }

    if (is_startup)
        fprintf(stderr, "STARTUP PARSE: %d errors, lexer at line %d\n",
                parser.num_errors, parser.lex.line);
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
        /* Check for key signatures */
        int found_intsoff = 0;
        for (int i = 0; i < num_shared_proc_sigs; i++) {
            if (strcasecmp(shared_proc_sigs[i].name, "INTSOFF") == 0) {
                fprintf(stderr, "  IMPORTED SIG[%d]: %s (%d params) ext=%d\n",
                        i, shared_proc_sigs[i].name, shared_proc_sigs[i].num_params,
                        shared_proc_sigs[i].is_external);
                found_intsoff++;
            }
        }
        if (!found_intsoff) {
            fprintf(stderr, "  WARNING: INTSOFF not in shared_proc_sigs! First 5 sigs:\n");
            for (int i = 0; i < 5 && i < num_shared_proc_sigs; i++)
                fprintf(stderr, "    [%d] %s (%d params)\n", i, shared_proc_sigs[i].name, shared_proc_sigs[i].num_params);
        }
    }

    /* Debug: log AST type for every file to trace symbol issues */
    if (ast && (strcasestr(path, "UNITHZ") || strcasestr(path, "UNITSTD"))) {
        fprintf(stderr, "  DEBUG AST: %s → type=%d (%s) children=%d\n",
                strrchr(path, '/') ? strrchr(path, '/') + 1 : path,
                ast->type, ast->name, ast->num_children);
        for (int i = 0; i < ast->num_children && i < 10; i++) {
            fprintf(stderr, "    child[%d] type=%d name='%s'\n",
                    i, ast->children[i]->type, ast->children[i]->name);
        }
    }

    codegen_generate(cg, ast);

    if (is_startup) {
        /* Dump bytes around offset $EC to debug odd BRA target */
        fprintf(stderr, "STARTUP bytes at $E8-$FF: ");
        for (uint32_t i = 0xE8; i < 0x100 && i < cg->code_size; i++)
            fprintf(stderr, "%02X ", cg->code[i]);
        fprintf(stderr, "\n");
        /* Check for relocations near offset $EC */
        for (int ri = 0; ri < cg->num_relocs; ri++) {
            if (cg->relocs[ri].offset >= 0xE8 && cg->relocs[ri].offset <= 0xF4)
                fprintf(stderr, "  RELOC at $%X: sym='%s' size=%d\n",
                        cg->relocs[ri].offset, cg->relocs[ri].symbol, cg->relocs[ri].size);
        }
        /* Count statements in main body */
        for (int i = 0; i < ast->num_children; i++) {
            if (ast->children[i]->type == 30) { /* AST_BLOCK */
                fprintf(stderr, "STARTUP MAIN BODY: %d statements\n",
                        ast->children[i]->num_children);
            }
        }
        fprintf(stderr, "STARTUP CODEGEN: %u bytes code, %d globals, %d relocs\n",
                cg->code_size, cg->num_globals, cg->num_relocs);
        for (int i = 0; i < cg->num_globals && i < 30; i++) {
            fprintf(stderr, "  global[%d]: '%s' offset=%d ext=%d\n",
                    i, cg->globals[i].name, cg->globals[i].offset,
                    cg->globals[i].is_external);
        }
    }

    /* Export this file's globals to the shared table for cross-unit access */
    for (int i = 0; i < cg->num_globals && num_shared_globals < MAX_SHARED_GLOBALS; i++) {
        if (!cg->globals[i].is_external && !cg->globals[i].is_forward) {
            shared_globals[num_shared_globals++] = cg->globals[i];
        }
    }

    /* Export this file's types to the shared table for cross-unit type casts */
    for (int i = 0; i < cg->num_types && num_shared_types < MAX_SHARED_TYPES; i++) {
        if (cg->types[i].name[0]) {
            shared_types[num_shared_types++] = cg->types[i];
        }
    }

    /* Export this file's procedure signatures to the shared table */
    if (cg->proc_sigs) {
        for (int i = 0; i < cg->num_proc_sigs && num_shared_proc_sigs < MAX_SHARED_PROC_SIGS; i++) {
            shared_proc_sigs[num_shared_proc_sigs++] = cg->proc_sigs[i];
        }
    }

    bool ok = true;
    if (cg->code_size > 0) {
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

    /* Only compile OS kernel and system libraries (LISA_OS/).
     * Apps (APPS/) and Toolkit (Lisa_Toolkit/) are loaded on demand —
     * the kernel must fit in 1MB RAM. */
    int num_files = 0;
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/LISA_OS", source_dir);
    num_files += find_source_files(subdir, files + num_files, MAX_SOURCE_FILES - num_files);

    /* Sort files: base units before numbered fragments (case-insensitive).
     * Ensures types defined in UNIT INTERFACE are available for fragments. */
    for (int a = 0; a < num_files - 1; a++)
        for (int b = a + 1; b < num_files; b++)
            if (strcasecmp(files[a].path, files[b].path) > 0) {
                source_file_t tmp = files[a]; files[a] = files[b]; files[b] = tmp;
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

        /* Dump bytes around $3015C to decode what the CPU gets stuck on */
        if (link_size > 0x30170) {
            fprintf(stderr, "=== LINK OUTPUT BYTES AT $30150-$30170 ===\n");
            fprintf(stderr, "  ");
            for (uint32_t a = 0x30150; a <= 0x30170; a += 2)
                fprintf(stderr, "%04X:%02X%02X ", a, link_data[a], link_data[a+1]);
            fprintf(stderr, "\n");

            /* Also identify which module contains $3015C */
            for (int m = 0; m < lk->num_modules; m++) {
                link_module_t *mod = lk->modules[m];
                if (!mod->is_kernel) continue;
                if (0x3015C >= mod->base_addr &&
                    0x3015C < mod->base_addr + mod->code_size) {
                    fprintf(stderr, "  $3015C is in module '%s' (base=$%X, size=%u, offset=%u)\n",
                            mod->name, mod->base_addr, mod->code_size,
                            0x3015C - mod->base_addr);
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
