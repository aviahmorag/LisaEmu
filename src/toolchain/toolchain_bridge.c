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
    /* Skip build scripts, linkmaps, docs, exec files */
    if (strcasecmp(name, "BUILD") == 0) return true;
    if (strstr(name, "Linkmaps") != NULL) return true;
    if (strstr(name, "exec") != NULL) return true;
    if (strcasecmp(name, "DICT") == 0) return true;
    if (strcasecmp(name, "FONTS") == 0) return true; /* binary fonts, not source */
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
    /* Skip files that start with P/X and are in TK dirs (phrase/exec config files) */
    if ((name[0] == 'P' || name[0] == 'X') && isupper(name[1]) &&
        !strstr(name, "PROC") && !strstr(name, "proc") &&
        strlen(name) < 20) {
        /* Likely a TK config file like PBOXER, XBOXER, PCLOCK, etc. */
        /* But only if it's short and all-caps before the extension */
    }
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

            /* Determine if assembly or Pascal */
            bool is_asm = (strcasestr(entry->d_name, "ASM") != NULL);

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

static bool assemble_file(const char *path, linker_t *lk) {
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

    int num_files = find_source_files(source_dir, files, MAX_SOURCE_FILES);
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
        if (assemble_file(files[i].path, lk)) asm_ok++;
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

    /* Add linked output as system.os */
    uint32_t link_size;
    const uint8_t *link_data = linker_get_output(lk, &link_size);
    if (link_data && link_size > 0) {
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

    /* Write disk image */
    char image_path[512];
    snprintf(image_path, sizeof(image_path), "%s/lisa_profile.image", output_dir);

    /* Create output directory if needed */
    mkdir(output_dir, 0755);

    disk_write_image(db, image_path);
    strncpy(result.output_path, image_path, sizeof(result.output_path) - 1);

    disk_free(db);
    free(db);
    linker_free(lk);
    free(lk);
    free(files);

    result.success = (result.files_compiled > 0);
    result.errors = 0; /* TODO: accumulate errors */

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
