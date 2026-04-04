/*
 * Batch codegen test — runs parser + codegen on a file, reports error counts.
 * Usage: test_batch_cg <file>
 */

#include "pascal_parser.h"
#include "pascal_codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_batch_cg <file>\n");
        return 1;
    }

    const char *filename = argv[1];
    FILE *f = fopen(filename, "r");
    if (!f) {
        printf("CANT_OPEN\t%s\n", filename);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    parser_t parser;
    parser_init(&parser, buf, filename);
    ast_node_t *ast = parser_parse(&parser);

    if (parser.num_errors > 0) {
        printf("P%d\t%s\n", parser.num_errors, filename);
        parser_free(&parser);
        free(buf);
        return 1;
    }

    codegen_t *cg = calloc(1, sizeof(codegen_t));
    codegen_init(cg);
    strncpy(cg->current_file, filename, sizeof(cg->current_file) - 1);
    codegen_generate(cg, ast);

    printf("%d\t%u\t%s\n", cg->num_errors, cg->code_size, filename);

    codegen_free(cg);
    free(cg);
    parser_free(&parser);
    free(buf);
    return cg->num_errors > 0 ? 1 : 0;
}
