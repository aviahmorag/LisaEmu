/*
 * Test the Lisa Pascal parser against actual Lisa source
 */

#include "pascal_parser.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    char *buf = NULL;
    const char *source;
    const char *filename;

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
            "uses\n"
            "  (*$U object/sysglobal.obj *) GLOBALDATA;\n"
            "\n"
            "const\n"
            "  maxItems = 100;\n"
            "  baseAddr = $FC0000;\n"
            "\n"
            "type\n"
            "  ItemRange = 0..99;\n"
            "  Buffer = packed array[0..511] of char;\n"
            "  PBuffer = ^Buffer;\n"
            "  ItemRec = record\n"
            "    name : string[32];\n"
            "    value : integer;\n"
            "    next : ^ItemRec;\n"
            "  end;\n"
            "\n"
            "var\n"
            "  count : integer;\n"
            "  items : array[ItemRange] of ItemRec;\n"
            "\n"
            "procedure InitItems;\n"
            "function GetItem(index : integer) : PBuffer;\n"
            "\n"
            "implementation\n"
            "\n"
            "procedure InitItems;\n"
            "  var i : integer;\n"
            "  begin\n"
            "    count := 0;\n"
            "    for i := 0 to maxItems - 1 do\n"
            "      begin\n"
            "        items[i].name := '';\n"
            "        items[i].value := 0;\n"
            "        items[i].next := nil\n"
            "      end\n"
            "  end;\n"
            "\n"
            "function GetItem(index : integer) : PBuffer;\n"
            "  begin\n"
            "    if (index >= 0) and (index < maxItems) then\n"
            "      GetItem := @items[index]\n"
            "    else\n"
            "      GetItem := nil\n"
            "  end;\n"
            "\n"
            "end.\n"
            ;
    }

    printf("=== Lisa Pascal Parser Test ===\n");
    printf("File: %s\n\n", filename);

    parser_t parser;
    parser_init(&parser, source, filename);
    ast_node_t *ast = parser_parse(&parser);

    if (parser.num_errors == 0) {
        printf("SUCCESS: parsed with 0 errors\n\n");
    } else {
        printf("Parsed with %d errors\n\n", parser.num_errors);
    }

    /* Print AST (first 60 lines) */
    printf("=== AST ===\n");

    /* Redirect stdout to count lines */
    ast_print(ast, 0);

    parser_free(&parser);
    if (buf) free(buf);
    return parser.num_errors > 0 ? 1 : 0;
}
