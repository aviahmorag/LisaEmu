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
    if (strcasecmp(name, "LIBHW") == 0 && !strcasestr(name, "LIBS")) return true; /* duplicate LIBHW outside LIBS */
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
    if (strcasestr(name, "68K.TEXT") != NULL && !strcasestr(name, "ASM")) return true; /* 68k stubs, not asm */
    if (strcasestr(name, "68k.text") != NULL && !strcasestr(name, "asm")) return true;
    if (strcasestr(name, "COMP.TEXT") != NULL) return true; /* compile scripts */
    if (strcasestr(name, "INSTALL.TEXT") != NULL) return true;
    if (strcasestr(name, "DOC.TEXT") != NULL) return true;
    if (strcasestr(name, "REL.TEXT") != NULL && strncasecmp(name, "libhw-REL", 9) == 0) return true;
    if (strcasestr(name, "LEGENDS") != NULL) return true;
    if (strcasestr(name, "APPENDIX") != NULL) return true;
    if (strcasestr(name, "INSTRUCT") != NULL) return true;
    if (strcasestr(name, "RELEASE") != NULL) return true;
    /* LIBFP: only NEWFPSUB should be assembled (it includes all others) */
    if (strcasestr(name, "libfp-") != NULL && !strcasestr(name, "NEWFPSUB")) return true;
    /* Assembly files misidentified as Pascal */
    if (strcasestr(name, "QSORT") != NULL) return true;
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
    if (strcasestr(name, "STRETCH") != NULL && strcasestr(name, "libqd") != NULL) return true;
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
    if (strcasestr(name, "MENUS.TEXT") != NULL) return true;
    if (strcasestr(name, "DBOX.TEXT") != NULL) return true;
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
            bool is_asm = (strcasestr(entry->d_name, "ASM") != NULL);
            if (!is_asm) {
                /* Check first non-empty line: assembly starts with ; or . or tab+instruction */
                FILE *probe = fopen(path, "r");
                if (probe) {
                    char line[256];
                    while (fgets(line, sizeof(line), probe)) {
                        /* Skip empty lines */
                        const char *p = line;
                        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                        if (*p == '\0') continue;
                        /* Assembly indicators */
                        if (*p == ';') { is_asm = true; break; }
                        if (*p == '.' && (p[1] == 'I' || p[1] == 'i' || p[1] == 'P' || p[1] == 'p' ||
                                          p[1] == 'D' || p[1] == 'd' || p[1] == 'T' || p[1] == 't' ||
                                          p[1] == 'M' || p[1] == 'm' || p[1] == 'S' || p[1] == 's' ||
                                          p[1] == 'E' || p[1] == 'e' || p[1] == 'R' || p[1] == 'r' ||
                                          p[1] == 'F' || p[1] == 'f' || p[1] == 'B' || p[1] == 'b')) {
                            is_asm = true; break;
                        }
                        break; /* First non-empty line wasn't assembly */
                    }
                    fclose(probe);
                }
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

static bool compile_pascal_file(const char *path, linker_t *lk) {
    char *source = read_file(path);
    if (!source) return false;

    parser_t parser;
    parser_init(&parser, source, path);
    ast_node_t *ast = parser_parse(&parser);

    if (parser.num_errors > 0) {
        parser_free(&parser);
        free(source);
        return false;
    }

    codegen_t *cg = calloc(1, sizeof(codegen_t));
    if (!cg) {
        parser_free(&parser);
        free(source);
        return false;
    }
    codegen_init(cg);
    strncpy(cg->current_file, path, sizeof(cg->current_file) - 1);
    codegen_generate(cg, ast);

    bool ok = true;
    if (cg->code_size > 0) {
        ok = linker_load_codegen(lk, path,
                                  cg->code, cg->code_size,
                                  cg->globals, cg->num_globals,
                                  cg->relocs, cg->num_relocs);
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
        linker_load_codegen(lk, path,
                            asm68k_get_output(as, NULL), as->output_size,
                            NULL, 0, NULL, 0);
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
    if (progress) progress("Found source files", 1, 100);

    /* Create linker */
    linker_t *lk = calloc(1, sizeof(linker_t));
    linker_init(lk);

    /* Phase 1: Compile Pascal files */
    int pascal_count = 0, pascal_ok = 0;
    for (int i = 0; i < num_files; i++) {
        if (files[i].is_assembly) continue;
        pascal_count++;
        if (compile_pascal_file(files[i].path, lk)) pascal_ok++;
        if (progress && pascal_count % 20 == 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Compiling Pascal: %d/%d", pascal_ok, pascal_count);
            progress(msg, 10 + (pascal_count * 30 / num_files), 100);
        }
    }
    result.files_compiled = pascal_ok;

    /* Phase 2: Assemble assembly files */
    int asm_count = 0, asm_ok = 0;
    for (int i = 0; i < num_files; i++) {
        if (!files[i].is_assembly) continue;
        asm_count++;
        if (assemble_file(files[i].path, lk, source_dir)) asm_ok++;
    }
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
