/*
 * Test the linker — compile multiple Pascal files and link them.
 *
 * Usage: test_linker [file1.pas file2.pas ...]
 *   Without args: runs built-in test with two small units
 *   With args: compiles and links the given files
 */

#include "pascal_lexer.h"
#include "pascal_parser.h"
#include "pascal_codegen.h"
#include "linker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static bool compile_and_load(linker_t *lk, const char *source, const char *name) {
    parser_t parser;
    parser_init(&parser, source, name);
    ast_node_t *ast = parser_parse(&parser);

    if (parser.num_errors > 0) {
        fprintf(stderr, "%s: %d parse errors\n", name, parser.num_errors);
        parser_free(&parser);
        return false;
    }

    codegen_t *cg = calloc(1, sizeof(codegen_t));
    codegen_init(cg);
    strncpy(cg->current_file, name, sizeof(cg->current_file) - 1);
    codegen_generate(cg, ast);

    if (cg->num_errors > 0) {
        fprintf(stderr, "%s: %d codegen errors\n", name, cg->num_errors);
        codegen_free(cg);
        free(cg);
        parser_free(&parser);
        return false;
    }

    bool ok = linker_load_codegen(lk, name,
                                   cg->code, cg->code_size,
                                   cg->globals, cg->num_globals,
                                   cg->relocs, cg->num_relocs);

    printf("  Compiled %-30s → %u bytes code, %d symbols, %d relocs\n",
           name, cg->code_size, cg->num_globals, cg->num_relocs);

    codegen_free(cg);
    free(cg);
    parser_free(&parser);
    return ok;
}

/* Built-in test: two units that reference each other */
static const char *unit_a =
    "unit UnitA;\n"
    "interface\n"
    "  procedure DoSomething(x: integer);\n"
    "  function GetValue: integer;\n"
    "implementation\n"
    "  var counter: integer;\n"
    "  procedure DoSomething(x: integer);\n"
    "  begin\n"
    "    counter := counter + x;\n"
    "  end;\n"
    "  function GetValue: integer;\n"
    "  begin\n"
    "    GetValue := counter;\n"
    "  end;\n"
    "end.\n";

static const char *unit_b =
    "program TestProg;\n"
    "begin\n"
    "  DoSomething(42);\n"
    "  DoSomething(100);\n"
    "end.\n";

int main(int argc, char *argv[]) {
    printf("=== LisaEm Linker Test ===\n\n");

    linker_t *lk = calloc(1, sizeof(linker_t));
    linker_init(lk);

    if (argc > 1) {
        /* Compile and link files from command line */
        printf("Compiling %d files...\n", argc - 1);
        for (int i = 1; i < argc; i++) {
            char *source = read_file(argv[i]);
            if (!source) {
                fprintf(stderr, "Cannot open: %s\n", argv[i]);
                continue;
            }
            compile_and_load(lk, source, argv[i]);
            free(source);
        }
    } else {
        /* Built-in test */
        printf("Compiling built-in test units...\n");
        compile_and_load(lk, unit_a, "UnitA");
        compile_and_load(lk, unit_b, "TestProg");
    }

    printf("\nLinking %d modules...\n", lk->num_modules);
    if (linker_link(lk)) {
        printf("SUCCESS!\n\n");
        linker_print_linkmap(lk);

        /* Write output */
        linker_write_binary(lk, "build/test_linked.bin");
    } else {
        printf("LINK FAILED: %d errors\n", lk->num_errors);
        for (int i = 0; i < lk->num_errors; i++) {
            printf("  %s\n", lk->errors[i]);
        }
    }

    linker_free(lk);
    free(lk);
    return 0;
}
