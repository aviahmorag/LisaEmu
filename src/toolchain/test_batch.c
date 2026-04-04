/*
 * Batch test — runs parser on a file, reports only error count.
 * Usage: test_batch <file>
 */

#include "pascal_parser.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_batch <file>\n");
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
    (void)ast;

    printf("%d\t%s\n", parser.num_errors, filename);

    parser_free(&parser);
    free(buf);
    return parser.num_errors > 0 ? 1 : 0;
}
