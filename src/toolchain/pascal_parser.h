/*
 * LisaEm Toolchain — Lisa Pascal Parser
 *
 * Recursive descent parser for Lisa Pascal.
 * Builds an AST (Abstract Syntax Tree) from tokens.
 *
 * Supports:
 *   UNIT / PROGRAM declarations
 *   INTERFACE / IMPLEMENTATION sections
 *   USES clauses with {$U} directives
 *   CONST, TYPE, VAR, LABEL declarations
 *   PROCEDURE / FUNCTION (including EXTERNAL, FORWARD)
 *   Full statement set: assignment, if/then/else, while, repeat,
 *     for, case, with, goto, compound (begin/end)
 *   Full expression set: arithmetic, relational, boolean, set, string
 *   Types: simple, subrange, array, record, set, file, pointer, string
 *   Compiler directives: {$S segment}, {$IFC}, {$ENDC}, {$R-}, etc.
 */

#ifndef PASCAL_PARSER_H
#define PASCAL_PARSER_H

#include "pascal_lexer.h"
#include <stdint.h>
#include <stdbool.h>

/* AST node types */
typedef enum {
    /* Top-level */
    AST_PROGRAM,
    AST_UNIT,

    /* Sections */
    AST_INTERFACE,
    AST_IMPLEMENTATION,
    AST_USES,
    AST_USES_ITEM,

    /* Declarations */
    AST_CONST_DECL,
    AST_TYPE_DECL,
    AST_VAR_DECL,
    AST_LABEL_DECL,
    AST_PROC_DECL,
    AST_FUNC_DECL,
    AST_PARAM_LIST,
    AST_PARAM,

    /* Types */
    AST_TYPE_IDENT,
    AST_TYPE_SUBRANGE,
    AST_TYPE_ARRAY,
    AST_TYPE_RECORD,
    AST_TYPE_SET,
    AST_TYPE_FILE,
    AST_TYPE_POINTER,
    AST_TYPE_STRING,
    AST_TYPE_PACKED,
    AST_TYPE_ENUM,
    AST_FIELD_LIST,
    AST_FIELD,
    AST_VARIANT,

    /* Statements */
    AST_BLOCK,         /* begin..end */
    AST_ASSIGN,
    AST_CALL,
    AST_IF,
    AST_WHILE,
    AST_REPEAT,
    AST_FOR,
    AST_CASE,
    AST_WITH,
    AST_GOTO,
    AST_EMPTY,

    /* Expressions */
    AST_BINARY_OP,
    AST_UNARY_OP,
    AST_INT_LITERAL,
    AST_REAL_LITERAL,
    AST_STRING_LITERAL,
    AST_IDENT_EXPR,
    AST_FUNC_CALL,
    AST_ARRAY_ACCESS,
    AST_FIELD_ACCESS,
    AST_DEREF,
    AST_ADDR_OF,
    AST_SET_EXPR,
    AST_NIL_EXPR,
    AST_TYPE_CAST,

    /* Compiler directive */
    AST_DIRECTIVE,
} ast_type_t;

/* AST node */
typedef struct ast_node {
    ast_type_t type;
    int line;

    /* Depending on node type */
    char name[256];          /* Identifier name */
    int64_t int_val;         /* Integer value */
    double real_val;         /* Real value */
    char str_val[1024];      /* String value */
    token_type_t op;         /* Operator for binary/unary */

    /* Children */
    struct ast_node **children;
    int num_children;
    int children_capacity;
} ast_node_t;

/* Parser state */
typedef struct {
    lexer_t lex;
    ast_node_t *root;
    int num_errors;
    char errors[100][256];
} parser_t;

/* Public API */
void parser_init(parser_t *p, const char *source, const char *filename);
ast_node_t *parser_parse(parser_t *p);
void parser_free(parser_t *p);

/* AST utilities */
ast_node_t *ast_new(ast_type_t type, int line);
void ast_add_child(ast_node_t *parent, ast_node_t *child);
void ast_free(ast_node_t *node);
void ast_print(ast_node_t *node, int indent);

const char *ast_type_name(ast_type_t type);

#endif /* PASCAL_PARSER_H */
