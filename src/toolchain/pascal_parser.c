/*
 * LisaEm Toolchain — Lisa Pascal Parser
 * Recursive descent parser producing an AST.
 */

#include "pascal_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* ========================================================================
 * AST node management
 * ======================================================================== */

ast_node_t *ast_new(ast_type_t type, int line) {
    ast_node_t *n = calloc(1, sizeof(ast_node_t));
    n->type = type;
    n->line = line;
    return n;
}

void ast_add_child(ast_node_t *parent, ast_node_t *child) {
    if (!parent || !child) return;
    if (parent->num_children >= parent->children_capacity) {
        parent->children_capacity = parent->children_capacity ? parent->children_capacity * 2 : 4;
        parent->children = realloc(parent->children, sizeof(ast_node_t *) * parent->children_capacity);
    }
    parent->children[parent->num_children++] = child;
}

void ast_free(ast_node_t *node) {
    if (!node) return;
    for (int i = 0; i < node->num_children; i++)
        ast_free(node->children[i]);
    free(node->children);
    free(node);
}

const char *ast_type_name(ast_type_t type) {
    switch (type) {
        case AST_PROGRAM: return "PROGRAM";
        case AST_UNIT: return "UNIT";
        case AST_INTERFACE: return "INTERFACE";
        case AST_IMPLEMENTATION: return "IMPLEMENTATION";
        case AST_USES: return "USES";
        case AST_USES_ITEM: return "USES_ITEM";
        case AST_CONST_DECL: return "CONST";
        case AST_TYPE_DECL: return "TYPE";
        case AST_VAR_DECL: return "VAR";
        case AST_LABEL_DECL: return "LABEL";
        case AST_PROC_DECL: return "PROCEDURE";
        case AST_FUNC_DECL: return "FUNCTION";
        case AST_PARAM_LIST: return "PARAMS";
        case AST_PARAM: return "PARAM";
        case AST_TYPE_IDENT: return "TYPE_ID";
        case AST_TYPE_SUBRANGE: return "SUBRANGE";
        case AST_TYPE_ARRAY: return "ARRAY";
        case AST_TYPE_RECORD: return "RECORD";
        case AST_TYPE_SET: return "SET";
        case AST_TYPE_FILE: return "FILE";
        case AST_TYPE_POINTER: return "POINTER";
        case AST_TYPE_STRING: return "STRING_TYPE";
        case AST_TYPE_PACKED: return "PACKED";
        case AST_TYPE_ENUM: return "ENUM";
        case AST_FIELD_LIST: return "FIELDS";
        case AST_FIELD: return "FIELD";
        case AST_BLOCK: return "BLOCK";
        case AST_ASSIGN: return "ASSIGN";
        case AST_CALL: return "CALL";
        case AST_IF: return "IF";
        case AST_WHILE: return "WHILE";
        case AST_REPEAT: return "REPEAT";
        case AST_FOR: return "FOR";
        case AST_CASE: return "CASE";
        case AST_WITH: return "WITH";
        case AST_GOTO: return "GOTO";
        case AST_EMPTY: return "EMPTY";
        case AST_BINARY_OP: return "BINOP";
        case AST_UNARY_OP: return "UNOP";
        case AST_INT_LITERAL: return "INT";
        case AST_REAL_LITERAL: return "REAL";
        case AST_STRING_LITERAL: return "STR";
        case AST_IDENT_EXPR: return "IDENT";
        case AST_FUNC_CALL: return "CALL";
        case AST_ARRAY_ACCESS: return "INDEX";
        case AST_FIELD_ACCESS: return "FIELD";
        case AST_DEREF: return "DEREF";
        case AST_ADDR_OF: return "ADDR";
        case AST_SET_EXPR: return "SET";
        case AST_NIL_EXPR: return "NIL";
        case AST_DIRECTIVE: return "DIRECTIVE";
        default: return "???";
    }
}

void ast_print(ast_node_t *node, int indent) {
    if (!node) return;
    for (int i = 0; i < indent; i++) printf("  ");
    printf("%s", ast_type_name(node->type));
    if (node->name[0]) printf(" name=\"%s\"", node->name);
    if (node->type == AST_INT_LITERAL) printf(" val=%lld", (long long)node->int_val);
    if (node->type == AST_STRING_LITERAL) printf(" val=\"%s\"", node->str_val);
    if (node->type == AST_DIRECTIVE) printf(" \"%s\"", node->str_val);
    printf("\n");
    for (int i = 0; i < node->num_children; i++)
        ast_print(node->children[i], indent + 1);
}

/* ========================================================================
 * Parser helpers
 * ======================================================================== */

#define CUR(p)    ((p)->lex.current)
#define CURTYPE(p) ((p)->lex.current.type)

static void parser_error(parser_t *p, const char *fmt, ...) {
    if (p->num_errors >= 100) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(p->errors[p->num_errors], 256, fmt, args);
    va_end(args);
    fprintf(stderr, "%s:%d: error: %s\n", p->lex.filename, p->lex.line, p->errors[p->num_errors]);
    p->num_errors++;
}

static token_t advance(parser_t *p) {
    return lexer_next(&p->lex);
}

static bool check(parser_t *p, token_type_t type) {
    return CURTYPE(p) == type;
}

static bool match(parser_t *p, token_type_t type) {
    if (CURTYPE(p) == type) {
        advance(p);
        return true;
    }
    return false;
}

static bool expect(parser_t *p, token_type_t type) {
    if (CURTYPE(p) == type) {
        advance(p);
        return true;
    }
    parser_error(p, "expected %s, got %s", token_type_name(type), token_type_name(CURTYPE(p)));
    return false;
}

/* Skip compiler directives, collecting them as nodes */
static ast_node_t *try_directive(parser_t *p) {
    if (CURTYPE(p) == TOK_DIRECTIVE) {
        ast_node_t *n = ast_new(AST_DIRECTIVE, p->lex.line);
        strncpy(n->str_val, CUR(p).str_val, sizeof(n->str_val) - 1);
        advance(p);
        return n;
    }
    return NULL;
}

/* Skip all consecutive directives */
static void skip_directives(parser_t *p, ast_node_t *parent) {
    while (CURTYPE(p) == TOK_DIRECTIVE) {
        ast_node_t *d = try_directive(p);
        if (d && parent) ast_add_child(parent, d);
    }
}

/* ========================================================================
 * Forward declarations
 * ======================================================================== */

static ast_node_t *parse_expression(parser_t *p);
static ast_node_t *parse_statement(parser_t *p);
static ast_node_t *parse_type(parser_t *p);
static void parse_declarations(parser_t *p, ast_node_t *parent);
static ast_node_t *parse_block(parser_t *p);

/* ========================================================================
 * Expression parser (precedence climbing)
 * ======================================================================== */

static ast_node_t *parse_factor(parser_t *p) {
    skip_directives(p, NULL);

    /* Integer literal */
    if (check(p, TOK_INTEGER)) {
        ast_node_t *n = ast_new(AST_INT_LITERAL, p->lex.line);
        n->int_val = CUR(p).int_val;
        advance(p);
        return n;
    }

    /* Real literal */
    if (check(p, TOK_REAL)) {
        ast_node_t *n = ast_new(AST_REAL_LITERAL, p->lex.line);
        n->real_val = CUR(p).real_val;
        advance(p);
        return n;
    }

    /* String literal */
    if (check(p, TOK_STRING)) {
        ast_node_t *n = ast_new(AST_STRING_LITERAL, p->lex.line);
        strncpy(n->str_val, CUR(p).str_val, sizeof(n->str_val) - 1);
        advance(p);
        return n;
    }

    /* NIL */
    if (match(p, TOK_NIL)) {
        return ast_new(AST_NIL_EXPR, p->lex.line);
    }

    /* NOT expr */
    if (match(p, TOK_NOT)) {
        ast_node_t *n = ast_new(AST_UNARY_OP, p->lex.line);
        n->op = TOK_NOT;
        ast_add_child(n, parse_factor(p));
        return n;
    }

    /* Unary minus */
    if (check(p, TOK_MINUS)) {
        advance(p);
        ast_node_t *n = ast_new(AST_UNARY_OP, p->lex.line);
        n->op = TOK_MINUS;
        ast_add_child(n, parse_factor(p));
        return n;
    }

    /* Unary plus */
    if (check(p, TOK_PLUS)) {
        advance(p);
        return parse_factor(p);
    }

    /* @ (address-of) */
    if (match(p, TOK_AT)) {
        ast_node_t *n = ast_new(AST_ADDR_OF, p->lex.line);
        ast_add_child(n, parse_factor(p));
        return n;
    }

    /* Parenthesized expression */
    if (match(p, TOK_LPAREN)) {
        ast_node_t *n = parse_expression(p);
        expect(p, TOK_RPAREN);
        return n;
    }

    /* Set constructor [a, b, c..d] */
    if (match(p, TOK_LBRACKET)) {
        ast_node_t *n = ast_new(AST_SET_EXPR, p->lex.line);
        if (!check(p, TOK_RBRACKET)) {
            do {
                ast_add_child(n, parse_expression(p));
                if (match(p, TOK_DOTDOT)) {
                    ast_add_child(n, parse_expression(p));
                }
            } while (match(p, TOK_COMMA));
        }
        expect(p, TOK_RBRACKET);
        return n;
    }

    /* Identifier (variable, function call, etc.) */
    if (check(p, TOK_IDENT) || check(p, TOK_INTEGER_KW) || check(p, TOK_BOOLEAN) ||
        check(p, TOK_CHAR) || check(p, TOK_LONGINT) || check(p, TOK_REAL_KW) ||
        check(p, TOK_TEXT)) {
        ast_node_t *n = ast_new(AST_IDENT_EXPR, p->lex.line);
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);

        /* Postfix: function call, array access, field access, dereference */
        while (1) {
            if (match(p, TOK_LPAREN)) {
                /* Function/procedure call */
                ast_node_t *call = ast_new(AST_FUNC_CALL, n->line);
                strncpy(call->name, n->name, sizeof(call->name) - 1);
                ast_free(n);
                if (!check(p, TOK_RPAREN)) {
                    do {
                        skip_directives(p, NULL);
                        ast_add_child(call, parse_expression(p));
                    } while (match(p, TOK_COMMA));
                }
                expect(p, TOK_RPAREN);
                n = call;
            } else if (match(p, TOK_LBRACKET)) {
                /* Array access */
                ast_node_t *idx = ast_new(AST_ARRAY_ACCESS, n->line);
                ast_add_child(idx, n);
                do {
                    ast_add_child(idx, parse_expression(p));
                } while (match(p, TOK_COMMA));
                expect(p, TOK_RBRACKET);
                n = idx;
            } else if (match(p, TOK_DOT)) {
                /* Field access */
                ast_node_t *field = ast_new(AST_FIELD_ACCESS, n->line);
                ast_add_child(field, n);
                strncpy(field->name, CUR(p).str_val, sizeof(field->name) - 1);
                advance(p);
                n = field;
            } else if (match(p, TOK_CARET)) {
                /* Pointer dereference */
                ast_node_t *deref = ast_new(AST_DEREF, n->line);
                ast_add_child(deref, n);
                n = deref;
            } else {
                break;
            }
        }
        return n;
    }

    parser_error(p, "unexpected token in expression: %s", token_type_name(CURTYPE(p)));
    advance(p); /* skip bad token */
    return ast_new(AST_EMPTY, p->lex.line);
}

static ast_node_t *parse_term(parser_t *p) {
    ast_node_t *left = parse_factor(p);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_DIV) ||
           check(p, TOK_MOD) || check(p, TOK_AND) || check(p, TOK_SHL) || check(p, TOK_SHR)) {
        token_type_t op = CURTYPE(p);
        advance(p);
        ast_node_t *n = ast_new(AST_BINARY_OP, p->lex.line);
        n->op = op;
        ast_add_child(n, left);
        ast_add_child(n, parse_factor(p));
        left = n;
    }
    return left;
}

static ast_node_t *parse_simple_expr(parser_t *p) {
    ast_node_t *left = parse_term(p);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS) || check(p, TOK_OR) || check(p, TOK_XOR)) {
        token_type_t op = CURTYPE(p);
        advance(p);
        ast_node_t *n = ast_new(AST_BINARY_OP, p->lex.line);
        n->op = op;
        ast_add_child(n, left);
        ast_add_child(n, parse_term(p));
        left = n;
    }
    return left;
}

static ast_node_t *parse_expression(parser_t *p) {
    ast_node_t *left = parse_simple_expr(p);
    if (check(p, TOK_EQ) || check(p, TOK_NE) || check(p, TOK_LT) ||
        check(p, TOK_LE) || check(p, TOK_GT) || check(p, TOK_GE) || check(p, TOK_IN)) {
        token_type_t op = CURTYPE(p);
        advance(p);
        ast_node_t *n = ast_new(AST_BINARY_OP, p->lex.line);
        n->op = op;
        ast_add_child(n, left);
        ast_add_child(n, parse_simple_expr(p));
        left = n;
    }
    return left;
}

/* ========================================================================
 * Statement parser
 * ======================================================================== */

static ast_node_t *parse_compound_statement(parser_t *p) {
    ast_node_t *block = ast_new(AST_BLOCK, p->lex.line);
    expect(p, TOK_BEGIN);
    while (!check(p, TOK_END) && !check(p, TOK_EOF)) {
        skip_directives(p, block);
        if (check(p, TOK_END)) break;
        ast_add_child(block, parse_statement(p));
        match(p, TOK_SEMICOLON); /* optional between statements */
        skip_directives(p, block);
    }
    expect(p, TOK_END);
    return block;
}

static ast_node_t *parse_statement(parser_t *p) {
    skip_directives(p, NULL);

    /* Compound statement */
    if (check(p, TOK_BEGIN))
        return parse_compound_statement(p);

    /* IF */
    if (match(p, TOK_IF)) {
        ast_node_t *n = ast_new(AST_IF, p->lex.line);
        ast_add_child(n, parse_expression(p));
        expect(p, TOK_THEN);
        ast_add_child(n, parse_statement(p));
        if (match(p, TOK_ELSE)) {
            ast_add_child(n, parse_statement(p));
        }
        return n;
    }

    /* WHILE */
    if (match(p, TOK_WHILE)) {
        ast_node_t *n = ast_new(AST_WHILE, p->lex.line);
        ast_add_child(n, parse_expression(p));
        expect(p, TOK_DO);
        ast_add_child(n, parse_statement(p));
        return n;
    }

    /* REPEAT */
    if (match(p, TOK_REPEAT)) {
        ast_node_t *n = ast_new(AST_REPEAT, p->lex.line);
        while (!check(p, TOK_UNTIL) && !check(p, TOK_EOF)) {
            ast_add_child(n, parse_statement(p));
            match(p, TOK_SEMICOLON);
        }
        expect(p, TOK_UNTIL);
        ast_add_child(n, parse_expression(p));
        return n;
    }

    /* FOR */
    if (match(p, TOK_FOR)) {
        ast_node_t *n = ast_new(AST_FOR, p->lex.line);
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);
        expect(p, TOK_ASSIGN);
        ast_add_child(n, parse_expression(p));
        if (match(p, TOK_TO)) n->int_val = 1;
        else if (match(p, TOK_DOWNTO)) n->int_val = -1;
        else expect(p, TOK_TO);
        ast_add_child(n, parse_expression(p));
        expect(p, TOK_DO);
        ast_add_child(n, parse_statement(p));
        return n;
    }

    /* CASE */
    if (match(p, TOK_CASE)) {
        ast_node_t *n = ast_new(AST_CASE, p->lex.line);
        ast_add_child(n, parse_expression(p));
        expect(p, TOK_OF);
        while (!check(p, TOK_END) && !check(p, TOK_OTHERWISE) && !check(p, TOK_EOF)) {
            skip_directives(p, n);
            /* case label(s) : statement */
            ast_node_t *label_node = parse_expression(p);
            while (match(p, TOK_COMMA)) {
                /* multiple labels - just parse and discard extras for now */
                parse_expression(p);
            }
            expect(p, TOK_COLON);
            ast_node_t *stmt = parse_statement(p);
            ast_add_child(n, label_node);
            ast_add_child(n, stmt);
            match(p, TOK_SEMICOLON);
        }
        if (match(p, TOK_OTHERWISE)) {
            ast_add_child(n, parse_statement(p));
            match(p, TOK_SEMICOLON);
        }
        expect(p, TOK_END);
        return n;
    }

    /* WITH */
    if (match(p, TOK_WITH)) {
        ast_node_t *n = ast_new(AST_WITH, p->lex.line);
        do {
            ast_add_child(n, parse_expression(p));
        } while (match(p, TOK_COMMA));
        expect(p, TOK_DO);
        ast_add_child(n, parse_statement(p));
        return n;
    }

    /* GOTO */
    if (match(p, TOK_GOTO)) {
        ast_node_t *n = ast_new(AST_GOTO, p->lex.line);
        n->int_val = CUR(p).int_val;
        advance(p);
        return n;
    }

    /* Assignment or procedure call */
    if (check(p, TOK_IDENT) || check(p, TOK_INTEGER_KW) || check(p, TOK_BOOLEAN) ||
        check(p, TOK_CHAR) || check(p, TOK_LONGINT)) {
        ast_node_t *lhs = parse_factor(p);

        if (match(p, TOK_ASSIGN)) {
            ast_node_t *n = ast_new(AST_ASSIGN, p->lex.line);
            ast_add_child(n, lhs);
            ast_add_child(n, parse_expression(p));
            return n;
        }

        /* It's a procedure call (already parsed in parse_factor) */
        if (lhs->type == AST_FUNC_CALL) return lhs;

        /* Bare identifier = procedure call with no args */
        ast_node_t *call = ast_new(AST_CALL, lhs->line);
        strncpy(call->name, lhs->name, sizeof(call->name) - 1);
        ast_free(lhs);
        return call;
    }

    /* Empty statement */
    return ast_new(AST_EMPTY, p->lex.line);
}

/* ========================================================================
 * Type parser
 * ======================================================================== */

static ast_node_t *parse_type(parser_t *p) {
    skip_directives(p, NULL);

    /* PACKED prefix */
    if (match(p, TOK_PACKED)) {
        ast_node_t *packed = ast_new(AST_TYPE_PACKED, p->lex.line);
        ast_add_child(packed, parse_type(p));
        return packed;
    }

    /* ARRAY [range] OF type */
    if (match(p, TOK_ARRAY)) {
        ast_node_t *n = ast_new(AST_TYPE_ARRAY, p->lex.line);
        expect(p, TOK_LBRACKET);
        do {
            ast_add_child(n, parse_expression(p));
            if (match(p, TOK_DOTDOT))
                ast_add_child(n, parse_expression(p));
        } while (match(p, TOK_COMMA));
        expect(p, TOK_RBRACKET);
        expect(p, TOK_OF);
        ast_add_child(n, parse_type(p));
        return n;
    }

    /* RECORD ... END */
    if (match(p, TOK_RECORD)) {
        ast_node_t *n = ast_new(AST_TYPE_RECORD, p->lex.line);
        while (!check(p, TOK_END) && !check(p, TOK_CASE) && !check(p, TOK_EOF)) {
            skip_directives(p, n);
            if (check(p, TOK_END) || check(p, TOK_CASE)) break;

            /* field: name[, name] : type */
            ast_node_t *field = ast_new(AST_FIELD, p->lex.line);
            strncpy(field->name, CUR(p).str_val, sizeof(field->name) - 1);
            advance(p);
            while (match(p, TOK_COMMA)) {
                /* Additional field names - append to name */
                strncat(field->name, ",", sizeof(field->name) - strlen(field->name) - 1);
                strncat(field->name, CUR(p).str_val, sizeof(field->name) - strlen(field->name) - 1);
                advance(p);
            }
            expect(p, TOK_COLON);
            ast_add_child(field, parse_type(p));
            ast_add_child(n, field);
            match(p, TOK_SEMICOLON);
        }
        /* Variant part (CASE) - skip for now */
        if (match(p, TOK_CASE)) {
            /* Skip variant part until END */
            int depth = 1;
            while (depth > 0 && !check(p, TOK_EOF)) {
                if (check(p, TOK_RECORD) || check(p, TOK_CASE)) depth++;
                if (check(p, TOK_END)) depth--;
                if (depth > 0) advance(p);
            }
        }
        expect(p, TOK_END);
        return n;
    }

    /* SET OF type */
    if (match(p, TOK_SET)) {
        ast_node_t *n = ast_new(AST_TYPE_SET, p->lex.line);
        expect(p, TOK_OF);
        ast_add_child(n, parse_type(p));
        return n;
    }

    /* FILE OF type */
    if (match(p, TOK_FILE)) {
        ast_node_t *n = ast_new(AST_TYPE_FILE, p->lex.line);
        if (match(p, TOK_OF)) {
            ast_add_child(n, parse_type(p));
        }
        return n;
    }

    /* ^type (pointer) */
    if (match(p, TOK_CARET)) {
        ast_node_t *n = ast_new(AST_TYPE_POINTER, p->lex.line);
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);
        return n;
    }

    /* STRING[n] */
    if (match(p, TOK_STRING_KW)) {
        ast_node_t *n = ast_new(AST_TYPE_STRING, p->lex.line);
        if (match(p, TOK_LBRACKET)) {
            n->int_val = CUR(p).int_val;
            advance(p);
            expect(p, TOK_RBRACKET);
        } else {
            n->int_val = 255;
        }
        return n;
    }

    /* Enumeration (ident1, ident2, ...) */
    if (match(p, TOK_LPAREN)) {
        ast_node_t *n = ast_new(AST_TYPE_ENUM, p->lex.line);
        do {
            ast_node_t *id = ast_new(AST_IDENT_EXPR, p->lex.line);
            strncpy(id->name, CUR(p).str_val, sizeof(id->name) - 1);
            advance(p);
            ast_add_child(n, id);
        } while (match(p, TOK_COMMA));
        expect(p, TOK_RPAREN);
        return n;
    }

    /* Simple type name or subrange */
    if (check(p, TOK_IDENT) || check(p, TOK_INTEGER_KW) || check(p, TOK_BOOLEAN) ||
        check(p, TOK_CHAR) || check(p, TOK_LONGINT) || check(p, TOK_REAL_KW) || check(p, TOK_TEXT)) {
        ast_node_t *n = ast_new(AST_TYPE_IDENT, p->lex.line);
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);
        return n;
    }

    /* Subrange starting with a constant (e.g., 0..255) */
    if (check(p, TOK_INTEGER) || check(p, TOK_MINUS)) {
        ast_node_t *n = ast_new(AST_TYPE_SUBRANGE, p->lex.line);
        ast_add_child(n, parse_expression(p));
        expect(p, TOK_DOTDOT);
        ast_add_child(n, parse_expression(p));
        return n;
    }

    parser_error(p, "expected type, got %s", token_type_name(CURTYPE(p)));
    return ast_new(AST_TYPE_IDENT, p->lex.line);
}

/* ========================================================================
 * Declaration parser
 * ======================================================================== */

static void parse_uses(parser_t *p, ast_node_t *parent) {
    ast_node_t *uses = ast_new(AST_USES, p->lex.line);
    do {
        skip_directives(p, uses); /* captures {$U path} directives */
        ast_node_t *item = ast_new(AST_USES_ITEM, p->lex.line);
        strncpy(item->name, CUR(p).str_val, sizeof(item->name) - 1);
        advance(p);
        ast_add_child(uses, item);
    } while (match(p, TOK_COMMA));
    expect(p, TOK_SEMICOLON);
    ast_add_child(parent, uses);
}

static void parse_const_section(parser_t *p, ast_node_t *parent) {
    while (check(p, TOK_IDENT) || check(p, TOK_DIRECTIVE)) {
        skip_directives(p, parent);
        if (!check(p, TOK_IDENT)) break;

        ast_node_t *n = ast_new(AST_CONST_DECL, p->lex.line);
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);
        expect(p, TOK_EQ);
        ast_add_child(n, parse_expression(p));
        expect(p, TOK_SEMICOLON);
        ast_add_child(parent, n);
    }
}

static void parse_type_section(parser_t *p, ast_node_t *parent) {
    while (check(p, TOK_IDENT) || check(p, TOK_DIRECTIVE)) {
        skip_directives(p, parent);
        if (!check(p, TOK_IDENT)) break;

        ast_node_t *n = ast_new(AST_TYPE_DECL, p->lex.line);
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);
        expect(p, TOK_EQ);
        ast_add_child(n, parse_type(p));
        expect(p, TOK_SEMICOLON);
        ast_add_child(parent, n);
    }
}

static void parse_var_section(parser_t *p, ast_node_t *parent) {
    while (check(p, TOK_IDENT) || check(p, TOK_DIRECTIVE)) {
        skip_directives(p, parent);
        if (!check(p, TOK_IDENT)) break;

        ast_node_t *n = ast_new(AST_VAR_DECL, p->lex.line);
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);
        while (match(p, TOK_COMMA)) {
            strncat(n->name, ",", sizeof(n->name) - strlen(n->name) - 1);
            strncat(n->name, CUR(p).str_val, sizeof(n->name) - strlen(n->name) - 1);
            advance(p);
        }
        expect(p, TOK_COLON);
        ast_add_child(n, parse_type(p));
        expect(p, TOK_SEMICOLON);
        ast_add_child(parent, n);
    }
}

static ast_node_t *parse_param_list(parser_t *p) {
    ast_node_t *params = ast_new(AST_PARAM_LIST, p->lex.line);
    expect(p, TOK_LPAREN);
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        skip_directives(p, NULL);
        ast_node_t *param = ast_new(AST_PARAM, p->lex.line);

        /* VAR, const, or value parameter */
        bool is_var = match(p, TOK_VAR);
        if (is_var) strncpy(param->str_val, "VAR", sizeof(param->str_val));

        /* Parameter name(s) */
        strncpy(param->name, CUR(p).str_val, sizeof(param->name) - 1);
        advance(p);
        while (match(p, TOK_COMMA)) {
            strncat(param->name, ",", sizeof(param->name) - strlen(param->name) - 1);
            strncat(param->name, CUR(p).str_val, sizeof(param->name) - strlen(param->name) - 1);
            advance(p);
        }

        expect(p, TOK_COLON);
        ast_add_child(param, parse_type(p));
        ast_add_child(params, param);

        if (!match(p, TOK_SEMICOLON)) break;
    }
    expect(p, TOK_RPAREN);
    return params;
}

static void parse_proc_or_func(parser_t *p, ast_node_t *parent, bool is_func) {
    ast_node_t *n = ast_new(is_func ? AST_FUNC_DECL : AST_PROC_DECL, p->lex.line);

    /* Name */
    if (check(p, TOK_IDENT)) {
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);
    }

    /* Parameters */
    if (check(p, TOK_LPAREN)) {
        ast_add_child(n, parse_param_list(p));
    }

    /* Function return type */
    if (is_func && match(p, TOK_COLON)) {
        ast_add_child(n, parse_type(p));
    }

    expect(p, TOK_SEMICOLON);
    skip_directives(p, n);

    /* EXTERNAL or FORWARD */
    if (match(p, TOK_EXTERNAL)) {
        strncpy(n->str_val, "EXTERNAL", sizeof(n->str_val));
        match(p, TOK_SEMICOLON);
    } else if (match(p, TOK_FORWARD)) {
        strncpy(n->str_val, "FORWARD", sizeof(n->str_val));
        match(p, TOK_SEMICOLON);
    } else {
        /* Full body: declarations + compound statement */
        parse_declarations(p, n);
        if (check(p, TOK_BEGIN)) {
            ast_add_child(n, parse_compound_statement(p));
            match(p, TOK_SEMICOLON);
        }
    }

    ast_add_child(parent, n);
}

static void parse_declarations(parser_t *p, ast_node_t *parent) {
    while (1) {
        skip_directives(p, parent);

        if (match(p, TOK_LABEL)) {
            /* LABEL section */
            do { advance(p); } while (match(p, TOK_COMMA));
            expect(p, TOK_SEMICOLON);
        } else if (match(p, TOK_CONST)) {
            parse_const_section(p, parent);
        } else if (match(p, TOK_TYPE)) {
            parse_type_section(p, parent);
        } else if (match(p, TOK_VAR)) {
            parse_var_section(p, parent);
        } else if (match(p, TOK_PROCEDURE)) {
            parse_proc_or_func(p, parent, false);
        } else if (match(p, TOK_FUNCTION)) {
            parse_proc_or_func(p, parent, true);
        } else {
            break;
        }
    }
}

/* ========================================================================
 * Top-level parser
 * ======================================================================== */

static ast_node_t *parse_unit(parser_t *p) {
    ast_node_t *unit = ast_new(AST_UNIT, p->lex.line);
    strncpy(unit->name, CUR(p).str_val, sizeof(unit->name) - 1);
    advance(p);
    expect(p, TOK_SEMICOLON);

    skip_directives(p, unit);

    /* INTERFACE section */
    if (match(p, TOK_INTERFACE)) {
        ast_node_t *iface = ast_new(AST_INTERFACE, p->lex.line);
        skip_directives(p, iface);
        if (match(p, TOK_USES)) parse_uses(p, iface);
        parse_declarations(p, iface);
        ast_add_child(unit, iface);
    }

    /* IMPLEMENTATION section */
    if (match(p, TOK_IMPLEMENTATION)) {
        ast_node_t *impl = ast_new(AST_IMPLEMENTATION, p->lex.line);
        skip_directives(p, impl);
        if (match(p, TOK_USES)) parse_uses(p, impl);
        parse_declarations(p, impl);
        ast_add_child(unit, impl);
    }

    /* Trailing END. */
    match(p, TOK_END);
    match(p, TOK_DOT);

    return unit;
}

static ast_node_t *parse_program(parser_t *p) {
    ast_node_t *prog = ast_new(AST_PROGRAM, p->lex.line);
    strncpy(prog->name, CUR(p).str_val, sizeof(prog->name) - 1);
    advance(p);

    /* Optional parameter list */
    if (check(p, TOK_LPAREN)) {
        advance(p);
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) advance(p);
        expect(p, TOK_RPAREN);
    }
    expect(p, TOK_SEMICOLON);

    skip_directives(p, prog);
    if (match(p, TOK_USES)) parse_uses(p, prog);
    parse_declarations(p, prog);

    if (check(p, TOK_BEGIN)) {
        ast_add_child(prog, parse_compound_statement(p));
    }
    match(p, TOK_DOT);

    return prog;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void parser_init(parser_t *p, const char *source, const char *filename) {
    memset(p, 0, sizeof(parser_t));
    lexer_init(&p->lex, source, filename);
}

ast_node_t *parser_parse(parser_t *p) {
    /* Get first token */
    advance(p);

    skip_directives(p, NULL);

    if (match(p, TOK_UNIT)) {
        p->root = parse_unit(p);
    } else if (match(p, TOK_PROGRAM)) {
        p->root = parse_program(p);
    } else {
        parser_error(p, "expected UNIT or PROGRAM");
        p->root = ast_new(AST_EMPTY, p->lex.line);
    }

    return p->root;
}

void parser_free(parser_t *p) {
    if (p->root) {
        ast_free(p->root);
        p->root = NULL;
    }
}
