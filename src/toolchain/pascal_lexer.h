/*
 * LisaEm Toolchain — Lisa Pascal Lexer
 *
 * Tokenizer for Apple Lisa Pascal dialect.
 * Handles: keywords, identifiers, numbers (decimal/hex/$),
 *          strings, operators, compiler directives {$...}
 */

#ifndef PASCAL_LEXER_H
#define PASCAL_LEXER_H

#include <stdint.h>
#include <stdbool.h>

/* Token types */
typedef enum {
    /* Special */
    TOK_EOF = 0,
    TOK_ERROR,

    /* Literals */
    TOK_INTEGER,       /* 123, $FF */
    TOK_REAL,          /* 3.14, 1.0E5 */
    TOK_STRING,        /* 'hello' */
    TOK_IDENT,         /* Identifier */

    /* Keywords */
    TOK_AND, TOK_ARRAY, TOK_BEGIN, TOK_CASE, TOK_CONST,
    TOK_DIV, TOK_DO, TOK_DOWNTO, TOK_ELSE, TOK_END,
    TOK_FILE, TOK_FOR, TOK_FUNCTION, TOK_GOTO, TOK_IF,
    TOK_IMPLEMENTATION, TOK_IN, TOK_INTERFACE, TOK_LABEL,
    TOK_MOD, TOK_NIL, TOK_NOT, TOK_OF, TOK_OR,
    TOK_OTHERWISE, TOK_PACKED, TOK_PROCEDURE, TOK_PROGRAM,
    TOK_RECORD, TOK_REPEAT, TOK_SET, TOK_SHL, TOK_SHR,
    TOK_STRING_KW, TOK_THEN, TOK_TO, TOK_TYPE, TOK_UNIT,
    TOK_UNTIL, TOK_USES, TOK_VAR, TOK_WHILE, TOK_WITH,
    TOK_XOR, TOK_EXTERNAL, TOK_FORWARD, TOK_INLINE,
    TOK_BOOLEAN, TOK_CHAR, TOK_INTEGER_KW, TOK_LONGINT,
    TOK_REAL_KW, TOK_TEXT,

    /* Operators / punctuation */
    TOK_PLUS,          /* + */
    TOK_MINUS,         /* - */
    TOK_STAR,          /* * */
    TOK_SLASH,         /* / */
    TOK_ASSIGN,        /* := */
    TOK_EQ,            /* = */
    TOK_NE,            /* <> */
    TOK_LT,            /* < */
    TOK_LE,            /* <= */
    TOK_GT,            /* > */
    TOK_GE,            /* >= */
    TOK_LPAREN,        /* ( */
    TOK_RPAREN,        /* ) */
    TOK_LBRACKET,      /* [ */
    TOK_RBRACKET,      /* ] */
    TOK_DOT,           /* . */
    TOK_DOTDOT,        /* .. */
    TOK_COMMA,         /* , */
    TOK_SEMICOLON,     /* ; */
    TOK_COLON,         /* : */
    TOK_CARET,         /* ^ */
    TOK_AT,            /* @ */

    /* Compiler directives */
    TOK_DIRECTIVE,     /* {$...} or (*$...*) */
} token_type_t;

/* Token value */
typedef struct {
    token_type_t type;
    int line;
    int col;

    union {
        int64_t int_val;         /* For TOK_INTEGER */
        double real_val;         /* For TOK_REAL */
        char str_val[1024];      /* For TOK_STRING, TOK_IDENT, TOK_DIRECTIVE */
    };
} token_t;

/* Lexer state */
typedef struct {
    const char *source;
    const char *pos;
    const char *filename;
    int line;
    int col;
    token_t current;
    token_t lookahead;
    bool has_lookahead;
} lexer_t;

/* Public API */
void lexer_init(lexer_t *lex, const char *source, const char *filename);
token_t lexer_next(lexer_t *lex);
token_t lexer_peek(lexer_t *lex);
bool lexer_match(lexer_t *lex, token_type_t type);
bool lexer_expect(lexer_t *lex, token_type_t type);
const char *token_type_name(token_type_t type);

#endif /* PASCAL_LEXER_H */
