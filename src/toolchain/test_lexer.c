/*
 * Test the Lisa Pascal lexer against actual Lisa source
 */

#include "pascal_lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    const char *source;
    char *buf = NULL;

    if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { fprintf(stderr, "Cannot open: %s\n", argv[1]); return 1; }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = malloc(size + 1);
        fread(buf, 1, size, f);
        buf[size] = '\0';
        fclose(f);
        source = buf;
    } else {
        source =
            "unit Sched;\n"
            "  { Copyright 1983, Apple Computer Inc. }\n"
            "interface\n"
            "uses\n"
            "  (*$U object/sysglobal.obj *) GLOBALDATA;\n"
            "\n"
            "const\n"
            "  maxDomain = 3;\n"
            "  baseAddr = $FC0000;\n"
            "\n"
            "type\n"
            "  pathname = string[255];\n"
            "  ptr_pathname = ^pathname;\n"
            "  domainRange = 0..3;\n"
            "\n"
            "var\n"
            "  counter : integer;\n"
            "  buffer : packed array[0..511] of char;\n"
            "\n"
            "procedure Scheduler;\n"
            "procedure SchedDisable;\n"
            "\n"
            "implementation\n"
            "\n"
            "procedure Scheduler;\n"
            "  var x : integer;\n"
            "  begin\n"
            "    x := 42;\n"
            "    if x > 0 then\n"
            "      x := x - 1\n"
            "    else\n"
            "      x := 0\n"
            "  end;\n"
            "\n"
            "end.\n"
            ;
    }

    printf("=== Lisa Pascal Lexer Test ===\n");
    if (argc > 1)
        printf("File: %s\n\n", argv[1]);
    else
        printf("Built-in test source\n\n");

    lexer_t lex;
    lexer_init(&lex, source, argc > 1 ? argv[1] : "test.pas");

    int count = 0;
    int errors = 0;
    token_t tok;

    do {
        tok = lexer_next(&lex);
        count++;

        if (tok.type == TOK_ERROR) {
            printf("  ERROR at %d:%d: %s\n", tok.line, tok.col, tok.str_val);
            errors++;
        }

        /* Print first 50 tokens in detail */
        if (count <= 50) {
            printf("  %3d  %-15s", count, token_type_name(tok.type));
            switch (tok.type) {
                case TOK_INTEGER:
                    printf("  %lld ($%llX)", tok.int_val, tok.int_val);
                    break;
                case TOK_REAL:
                    printf("  %g", tok.real_val);
                    break;
                case TOK_STRING:
                case TOK_IDENT:
                case TOK_DIRECTIVE:
                    printf("  \"%s\"", tok.str_val);
                    break;
                default:
                    break;
            }
            printf("\n");
        }
    } while (tok.type != TOK_EOF);

    printf("\n--- %d tokens, %d errors ---\n", count, errors);

    if (buf) free(buf);
    return errors > 0 ? 1 : 0;
}
