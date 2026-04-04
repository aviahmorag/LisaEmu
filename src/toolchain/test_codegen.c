/*
 * Test the full Pascal compilation pipeline: lex -> parse -> codegen
 */

#include "pascal_parser.h"
#include "pascal_codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    const char *source;
    const char *filename;
    char *buf = NULL;

    if (argc > 1) {
        filename = argv[1];
        FILE *f = fopen(filename, "r");
        if (!f) { fprintf(stderr, "Cannot open: %s\n", filename); return 1; }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = malloc(size + 1);
        fread(buf, 1, size, f);
        buf[size] = '\0';
        fclose(f);
        source = buf;
    } else {
        filename = "test.pas";
        source =
            "unit TestUnit;\n"
            "interface\n"
            "const\n"
            "  maxItems = 100;\n"
            "type\n"
            "  ItemRange = 0..99;\n"
            "var\n"
            "  count : integer;\n"
            "  total : longint;\n"
            "\n"
            "procedure InitCount;\n"
            "function AddItem(value : integer) : integer;\n"
            "\n"
            "implementation\n"
            "\n"
            "procedure InitCount;\n"
            "  begin\n"
            "    count := 0;\n"
            "    total := 0\n"
            "  end;\n"
            "\n"
            "function AddItem(value : integer) : integer;\n"
            "  var temp : integer;\n"
            "  begin\n"
            "    temp := count + 1;\n"
            "    if temp < maxItems then\n"
            "      begin\n"
            "        count := temp;\n"
            "        total := total + value;\n"
            "        AddItem := temp\n"
            "      end\n"
            "    else\n"
            "      AddItem := -1\n"
            "  end;\n"
            "\n"
            "end.\n"
            ;
    }

    printf("=== Lisa Pascal Full Pipeline Test ===\n");
    printf("File: %s\n\n", filename);

    /* Step 1: Parse */
    printf("--- Parsing ---\n");
    parser_t parser;
    parser_init(&parser, source, filename);
    ast_node_t *ast = parser_parse(&parser);
    printf("  Parse: %d errors\n", parser.num_errors);

    if (parser.num_errors > 0 && argc <= 1) {
        printf("  Parse failed on built-in test\n");
        parser_free(&parser);
        if (buf) free(buf);
        return 1;
    }

    /* Step 2: Generate code */
    printf("\n--- Code Generation ---\n");
    codegen_t *cg = calloc(1, sizeof(codegen_t));
    codegen_init(cg);
    strncpy(cg->current_file, filename, sizeof(cg->current_file) - 1);

    bool ok = codegen_generate(cg, ast);
    printf("  Codegen: %d errors, %u bytes generated\n", cg->num_errors, cg->code_size);

    if (cg->code_size > 0) {
        /* Hex dump first 128 bytes */
        printf("\n--- Generated 68000 Code ---\n");
        uint32_t dump_size = cg->code_size < 128 ? cg->code_size : 128;
        for (uint32_t i = 0; i < dump_size; i++) {
            if (i % 16 == 0) printf("  %04X: ", i);
            printf("%02X ", cg->code[i]);
            if (i % 16 == 15 || i == dump_size - 1) printf("\n");
        }
        if (cg->code_size > 128) printf("  ... (%u more bytes)\n", cg->code_size - 128);

        /* Show symbols */
        printf("\n--- Symbols ---\n");
        for (int i = 0; i < cg->num_globals; i++) {
            if (cg->globals[i].name[0]) {
                printf("  %s: offset=%d%s%s\n", cg->globals[i].name, cg->globals[i].offset,
                       cg->globals[i].is_external ? " EXTERNAL" : "",
                       cg->globals[i].is_forward ? " FORWARD" : "");
            }
        }

        /* Show relocations */
        if (cg->num_relocs > 0) {
            printf("\n--- Relocations ---\n");
            for (int i = 0; i < cg->num_relocs; i++) {
                printf("  @%04X -> %s (%d bytes)\n", cg->relocs[i].offset,
                       cg->relocs[i].symbol, cg->relocs[i].size);
            }
        }

        /* Write object file */
        if (argc > 1) {
            char objname[256];
            snprintf(objname, sizeof(objname), "build/%s.obj", filename);
            /* Simplify: just use a flat name */
            codegen_write_obj(cg, "build/output.obj");
        }
    }

    printf("\n--- Result: %s ---\n", (ok && parser.num_errors == 0) ? "SUCCESS" : "ERRORS");

    codegen_free(cg);
    free(cg);
    parser_free(&parser);
    if (buf) free(buf);
    return (ok && parser.num_errors == 0) ? 0 : 1;
}
