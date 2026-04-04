/*
 * Trace test — lexes a file and prints all tokens to understand what the parser sees.
 */

#include "pascal_lexer.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: test_trace <file>\n"); return 1; }

    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    lexer_t lex;
    lexer_init(&lex, buf, argv[1]);

    int count = 0;
    while (1) {
        token_t tok = lexer_next(&lex);
        printf("%d:%d\t%s", tok.line, tok.col, token_type_name(tok.type));
        if (tok.type == TOK_IDENT || tok.type == TOK_DIRECTIVE)
            printf("\t'%s'", tok.str_val);
        else if (tok.type == TOK_INTEGER)
            printf("\t%lld", (long long)tok.int_val);
        else if (tok.type == TOK_STRING)
            printf("\t'%s'", tok.str_val);
        printf("\n");
        if (tok.type == TOK_EOF) break;
        if (++count > 5000) { printf("...(truncated)\n"); break; }
    }

    free(buf);
    return 0;
}
