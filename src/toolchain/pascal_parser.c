/*
 * LisaEm Toolchain — Lisa Pascal Parser
 * Recursive descent parser producing an AST.
 *
 * Handles:
 *   - Full UNIT and PROGRAM files
 *   - Code fragments (files starting with procedures, METHODS OF, etc.)
 *   - Lisa Pascal extensions: write format specifiers (expr:width),
 *     comment-wrapped params, SUBCLASS OF, METHODS OF, OVERRIDE
 *   - Conditional compilation directives ({$IFC}/{$ENDC})
 */

#include "pascal_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* Maximum errors before bail-out to prevent infinite loops */
#define MAX_ERRORS_BAILOUT 200

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
        case AST_FRAGMENT: return "FRAGMENT";
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
        case AST_TYPE_CLASS: return "CLASS";
        case AST_FIELD_LIST: return "FIELDS";
        case AST_FIELD: return "FIELD";
        case AST_VARIANT: return "VARIANT";
        case AST_METHODS: return "METHODS";
        case AST_BLOCK: return "BLOCK";
        case AST_ASSIGN: return "ASSIGN";
        case AST_CALL: return "CALL";
        case AST_IF: return "IF";
        case AST_WHILE: return "WHILE";
        case AST_REPEAT: return "REPEAT";
        case AST_FOR: return "FOR";
        case AST_CASE: return "CASE";
        case AST_CASE_LABELS: return "CASE_LABELS";
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
        case AST_TYPE_CAST: return "CAST";
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
#define BAILED(p) ((p)->num_errors >= MAX_ERRORS_BAILOUT)

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

/* Case-insensitive string comparison */
static bool str_eq_upper(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

/* Match a specific identifier (case-insensitive) */
static bool match_ident(parser_t *p, const char *name) {
    if (CURTYPE(p) == TOK_IDENT && str_eq_upper(CUR(p).str_val, name)) {
        advance(p);
        return true;
    }
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

/* Synchronize after an error — skip tokens until a likely recovery point */
static void synchronize(parser_t *p) {
    while (!check(p, TOK_EOF)) {
        /* Stop at statement/declaration boundaries */
        if (check(p, TOK_SEMICOLON)) { advance(p); return; }
        if (check(p, TOK_END)) return;
        if (check(p, TOK_BEGIN)) return;
        if (check(p, TOK_PROCEDURE)) return;
        if (check(p, TOK_FUNCTION)) return;
        if (check(p, TOK_CONST)) return;
        if (check(p, TOK_TYPE)) return;
        if (check(p, TOK_VAR)) return;
        if (check(p, TOK_IF)) return;
        if (check(p, TOK_WHILE)) return;
        if (check(p, TOK_FOR)) return;
        if (check(p, TOK_REPEAT)) return;
        if (check(p, TOK_CASE)) return;
        if (check(p, TOK_WITH)) return;
        if (check(p, TOK_IMPLEMENTATION)) return;
        if (check(p, TOK_INTERFACE)) return;
        if (check(p, TOK_METHODS)) return;
        advance(p);
    }
}


/* ========================================================================
 * Forward declarations
 * ======================================================================== */

static ast_node_t *parse_expression(parser_t *p);
static ast_node_t *parse_statement(parser_t *p);
static ast_node_t *parse_type(parser_t *p);
static void parse_declarations(parser_t *p, ast_node_t *parent);
static ast_node_t *parse_compound_statement(parser_t *p);

/* ========================================================================
 * Expression parser (precedence climbing)
 * ======================================================================== */

static ast_node_t *parse_factor(parser_t *p) {
    if (BAILED(p)) return ast_new(AST_EMPTY, p->lex.line);
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

    /* Identifier (variable, function call, type name used as value, etc.) */
    if (check(p, TOK_IDENT) || check(p, TOK_INTEGER_KW) || check(p, TOK_BOOLEAN) ||
        check(p, TOK_CHAR) || check(p, TOK_LONGINT) || check(p, TOK_REAL_KW) ||
        check(p, TOK_TEXT) || check(p, TOK_STRING_KW)) {
        ast_node_t *n = ast_new(AST_IDENT_EXPR, p->lex.line);
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);

        /* Postfix: function call, array access, field access, dereference */
        while (1) {
            if (BAILED(p)) break;
            if (match(p, TOK_LPAREN)) {
                /* Function/procedure call */
                ast_node_t *call = ast_new(AST_FUNC_CALL, n->line);
                strncpy(call->name, n->name, sizeof(call->name) - 1);
                ast_free(n);
                if (!check(p, TOK_RPAREN)) {
                    do {
                        skip_directives(p, NULL);
                        ast_add_child(call, parse_expression(p));
                        /* Handle write format specifiers: expr:width[:decimals] */
                        while (match(p, TOK_COLON)) {
                            parse_expression(p); /* discard width/decimals */
                        }
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

    parser_error(p, "unexpected token in expression: %s '%s'",
                 token_type_name(CURTYPE(p)), CUR(p).str_val);
    advance(p); /* skip bad token */
    return ast_new(AST_EMPTY, p->lex.line);
}

static ast_node_t *parse_term(parser_t *p) {
    ast_node_t *left = parse_factor(p);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_DIV) ||
           check(p, TOK_MOD) || check(p, TOK_AND) || check(p, TOK_SHL) || check(p, TOK_SHR)) {
        if (BAILED(p)) break;
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
        if (BAILED(p)) break;
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
    if (BAILED(p)) return ast_new(AST_EMPTY, p->lex.line);
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
    while (!check(p, TOK_END) && !check(p, TOK_EOF) && !BAILED(p)) {
        const char *prev_pos = p->lex.pos;
        skip_directives(p, block);
        if (check(p, TOK_END)) break;
        ast_add_child(block, parse_statement(p));
        match(p, TOK_SEMICOLON);
        skip_directives(p, block);
        if (p->lex.pos == prev_pos && !check(p, TOK_END) && !check(p, TOK_EOF)) {
            parser_error(p, "stuck at '%s', skipping", token_type_name(CURTYPE(p)));
            advance(p);
        }
    }
    expect(p, TOK_END);
    return block;
}

static ast_node_t *parse_statement(parser_t *p) {
    if (BAILED(p)) return ast_new(AST_EMPTY, p->lex.line);
    skip_directives(p, NULL);

    /* Numeric label prefix: 123: statement */
    if (check(p, TOK_INTEGER) && p->lex.pos[0] != '\0') {
        /* Look ahead to see if this is a label */
        token_t saved = CUR(p);
        lexer_t saved_lex = p->lex;
        int label_num = (int)CUR(p).int_val;
        advance(p);
        if (check(p, TOK_COLON)) {
            advance(p); /* consume colon */
            /* Emit an AST_LABEL_DECL node carrying the label number, followed
             * by the labeled statement as its child. Codegen records the
             * label's emission address and patches pending forward-GOTOs. */
            ast_node_t *lbl = ast_new(AST_LABEL_DECL, p->lex.line);
            lbl->int_val = label_num;
            ast_add_child(lbl, parse_statement(p));
            return lbl;
        }
        /* Not a label — restore and fall through to expression parsing */
        p->lex = saved_lex;
        p->lex.current = saved;
    }

    /* Compound statement */
    if (check(p, TOK_BEGIN))
        return parse_compound_statement(p);

    /* IF */
    if (match(p, TOK_IF)) {
        ast_node_t *n = ast_new(AST_IF, p->lex.line);
        ast_add_child(n, parse_expression(p));
        skip_directives(p, NULL);
        expect(p, TOK_THEN);
        skip_directives(p, NULL);
        ast_add_child(n, parse_statement(p));
        skip_directives(p, NULL);
        if (match(p, TOK_ELSE)) {
            skip_directives(p, NULL);
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
        while (!check(p, TOK_UNTIL) && !check(p, TOK_EOF) && !BAILED(p)) {
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
        while (!check(p, TOK_END) && !check(p, TOK_OTHERWISE) && !check(p, TOK_EOF) && !BAILED(p)) {
            skip_directives(p, n);
            if (check(p, TOK_END) || check(p, TOK_OTHERWISE)) break;
            /* Handle empty case alternative (comment-only line followed by ;) */
            if (check(p, TOK_SEMICOLON)) {
                match(p, TOK_SEMICOLON);
                continue;
            }
            /* case label(s) : statement
             * Multiple comma-separated labels share one body. Wrap them
             * in an AST_CASE_LABELS node so codegen can emit one body
             * reached via multiple comparisons (avoids body-dup bugs). */
            ast_node_t *first_label = parse_expression(p);
            if (check(p, TOK_COMMA)) {
                ast_node_t *labels_group = ast_new(AST_CASE_LABELS, p->lex.line);
                ast_add_child(labels_group, first_label);
                while (match(p, TOK_COMMA)) {
                    ast_add_child(labels_group, parse_expression(p));
                }
                if (!expect(p, TOK_COLON)) { synchronize(p); continue; }
                ast_node_t *stmt = parse_statement(p);
                ast_add_child(n, labels_group);
                ast_add_child(n, stmt);
            } else {
                if (!expect(p, TOK_COLON)) { synchronize(p); continue; }
                ast_node_t *stmt = parse_statement(p);
                ast_add_child(n, first_label);
                ast_add_child(n, stmt);
            }
            match(p, TOK_SEMICOLON);
        }
        if (match(p, TOK_OTHERWISE)) {
            /* OTHERWISE can have multiple statements */
            while (!check(p, TOK_END) && !check(p, TOK_EOF) && !BAILED(p)) {
                ast_add_child(n, parse_statement(p));
                if (!match(p, TOK_SEMICOLON)) break;
            }
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

    /* INLINE (assembly inline) — skip the data words */
    if (match(p, TOK_INLINE)) {
        ast_node_t *n = ast_new(AST_EMPTY, p->lex.line);
        /* INLINE can appear as: INLINE expr, expr, ... or INLINE(data) */
        if (match(p, TOK_LPAREN)) {
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF) && !BAILED(p)) {
                advance(p);
            }
            match(p, TOK_RPAREN);
        } else {
            /* INLINE followed by data words separated by / */
            /* Actually Lisa Pascal INLINE is: INLINE $1234/$5678/... */
            /* Just skip tokens until semicolon or statement boundary */
            while (!check(p, TOK_SEMICOLON) && !check(p, TOK_END) &&
                   !check(p, TOK_EOF) && !BAILED(p)) {
                advance(p);
            }
        }
        return n;
    }

    /* Assignment or procedure call */
    if (check(p, TOK_IDENT) || check(p, TOK_INTEGER_KW) || check(p, TOK_BOOLEAN) ||
        check(p, TOK_CHAR) || check(p, TOK_LONGINT) || check(p, TOK_TEXT) ||
        check(p, TOK_REAL_KW) || check(p, TOK_STRING_KW)) {
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

    /* Unknown token — skip it and return empty statement */
    if (!check(p, TOK_SEMICOLON) && !check(p, TOK_END) && !check(p, TOK_EOF) &&
        !check(p, TOK_ELSE) && !check(p, TOK_UNTIL)) {
        /* Don't report error for truly empty statements (just a semicolon) */
        if (!check(p, TOK_DIRECTIVE)) {
            parser_error(p, "unexpected token in statement: %s", token_type_name(CURTYPE(p)));
            advance(p);
        }
    }
    return ast_new(AST_EMPTY, p->lex.line);
}

/* ========================================================================
 * Type parser
 * ======================================================================== */

static ast_node_t *parse_type(parser_t *p) {
    if (BAILED(p)) return ast_new(AST_TYPE_IDENT, p->lex.line);
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
            /* Accept both ".." and ":" as range separator (Lisa Pascal uses both) */
            if (match(p, TOK_DOTDOT) || match(p, TOK_COLON))
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
        while (!check(p, TOK_END) && !check(p, TOK_CASE) && !check(p, TOK_EOF) && !BAILED(p)) {
            skip_directives(p, n);
            if (check(p, TOK_END) || check(p, TOK_CASE)) break;

            /* field: name[, name] : type */
            ast_node_t *field = ast_new(AST_FIELD, p->lex.line);
            strncpy(field->name, CUR(p).str_val, sizeof(field->name) - 1);
            advance(p);
            while (match(p, TOK_COMMA)) {
                strncat(field->name, ",", sizeof(field->name) - strlen(field->name) - 1);
                strncat(field->name, CUR(p).str_val, sizeof(field->name) - strlen(field->name) - 1);
                advance(p);
            }
            expect(p, TOK_COLON);
            ast_add_child(field, parse_type(p));
            ast_add_child(n, field);
            match(p, TOK_SEMICOLON);
        }
        /* Variant part (CASE tag OF ...) */
        if (match(p, TOK_CASE)) {
            /* CASE [tag :] type OF variant_list
             * Parse variant fields so the record gets correct sizing.
             * We collect all fields from all variants — this may overcount
             * (variants overlap in memory) but never undercounts, which is
             * critical: undercounting causes LINK A6,#0 → stack corruption.
             *
             * P82b: emit the TAG field as a regular fixed field before the
             * variant region. Without it, kernel code like
             * `if next_sdb^.sdbtype = free` compiles to a field lookup that
             * misses (codesdb has no "sdbtype" field in our record) and
             * field_off defaults to 0 — silently comparing memchain.fwd_link
             * bytes against the enum ordinal and sending CLEAR_SPACE's
             * inner while loop into infinite spin. */
            if (CURTYPE(p) == TOK_IDENT) {
                /* Peek one token: tag form is `IDENT COLON TYPE OF`, untagged
                 * is `TYPE OF` (one IDENT then OF). lexer_peek fills lookahead
                 * without consuming the current token. */
                token_t peek = lexer_peek(&p->lex);
                if (peek.type == TOK_COLON) {
                    ast_node_t *tag_field = ast_new(AST_FIELD, p->lex.line);
                    strncpy(tag_field->name, CUR(p).str_val, sizeof(tag_field->name) - 1);
                    advance(p); /* consume tag ident */
                    advance(p); /* consume ':' */
                    ast_add_child(tag_field, parse_type(p));
                    ast_add_child(n, tag_field);
                }
            }
            /* Skip remaining discriminant type tokens up to OF */
            while (!check(p, TOK_OF) && !check(p, TOK_END) && !check(p, TOK_EOF) && !BAILED(p))
                advance(p);
            match(p, TOK_OF);

            /* Emit a sentinel marker before the variant fields so the type
             * builder knows the variant region starts here (and can lay out
             * arms with overlapping offsets). */
            ast_node_t *vbeg = ast_new(AST_FIELD, p->lex.line);
            strncpy(vbeg->name, "__variant_begin__", sizeof(vbeg->name) - 1);
            ast_add_child(n, vbeg);

            /* Collect fields from ALL variant arms and add them to the
             * record. Different arms overlap in memory at the same starting
             * offset (the "variant offset" = sum of fixed fields). Type
             * descriptors downstream don't model overlap, but the codegen
             * needs to find every variant field by name — otherwise code
             * like `with rec do begin freechain.fwd_link := ... end`
             * silently drops stores when `freechain` is in a smaller arm.
             * Correct Pascal layout sets the overlap offset per arm; our
             * compromise is to mark variant fields with a flag so the
             * offset-assignment pass can reset them to the variant offset.
             * For now we just emit them all and rely on the first arm
             * providing the most relevant layout. */
            ast_node_t *all_variant_fields[256];
            int num_variant_fields = 0;
            int arm_start_indices[64]; /* index in all_variant_fields where each arm begins */
            int num_arms = 0;

            while (!check(p, TOK_END) && !check(p, TOK_EOF) && !BAILED(p)) {
                /* Skip variant labels: const [, const]... : */
                while (!check(p, TOK_COLON) && !check(p, TOK_END) && !check(p, TOK_EOF) && !BAILED(p))
                    advance(p);
                if (!match(p, TOK_COLON)) break;

                /* Variant arm fields in parentheses: (field1: type1; ...) */
                if (match(p, TOK_LPAREN)) {
                    if (num_arms < 64)
                        arm_start_indices[num_arms++] = num_variant_fields;
                    while (!check(p, TOK_RPAREN) && !check(p, TOK_CASE) &&
                           !check(p, TOK_END) && !check(p, TOK_EOF) && !BAILED(p)) {
                        /* Parse field: name[, name] : type */
                        if (CURTYPE(p) == TOK_IDENT) {
                            ast_node_t *field = ast_new(AST_FIELD, p->lex.line);
                            strncpy(field->name, CUR(p).str_val, sizeof(field->name) - 1);
                            advance(p);
                            while (match(p, TOK_COMMA)) {
                                strncat(field->name, ",", sizeof(field->name) - strlen(field->name) - 1);
                                if (CURTYPE(p) == TOK_IDENT) {
                                    strncat(field->name, CUR(p).str_val, sizeof(field->name) - strlen(field->name) - 1);
                                    advance(p);
                                }
                            }
                            if (match(p, TOK_COLON)) {
                                ast_add_child(field, parse_type(p));
                            }
                            if (num_variant_fields < 256)
                                all_variant_fields[num_variant_fields++] = field;
                            match(p, TOK_SEMICOLON);
                        } else {
                            advance(p); /* skip unexpected tokens */
                        }
                    }
                    match(p, TOK_RPAREN);
                } else {
                    /* No parentheses — skip to next variant or END */
                }
                match(p, TOK_SEMICOLON);
            }

            /* Add variant-arm fields, with __variant_arm__ sentinels
             * between arms so the type builder can reset offset to the
             * variant start at each arm boundary (proper Pascal variant
             * record layout — arms overlap in memory). */
            for (int ai = 0; ai < num_arms; ai++) {
                int arm_begin = arm_start_indices[ai];
                int arm_end = (ai + 1 < num_arms) ? arm_start_indices[ai + 1] : num_variant_fields;
                if (ai > 0) {
                    ast_node_t *vsep = ast_new(AST_FIELD, p->lex.line);
                    strncpy(vsep->name, "__variant_arm__", sizeof(vsep->name) - 1);
                    ast_add_child(n, vsep);
                }
                for (int fi = arm_begin; fi < arm_end; fi++)
                    ast_add_child(n, all_variant_fields[fi]);
            }
            ast_node_t *vend = ast_new(AST_FIELD, p->lex.line);
            strncpy(vend->name, "__variant_end__", sizeof(vend->name) - 1);
            ast_add_child(n, vend);
        }
        expect(p, TOK_END);
        return n;
    }

    /* SUBCLASS OF parentclass ... END */
    if (match(p, TOK_SUBCLASS)) {
        ast_node_t *n = ast_new(AST_TYPE_CLASS, p->lex.line);
        expect(p, TOK_OF);
        /* Parent class name (could be NIL for root) */
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);

        /* Class body: fields, methods, until END */
        while (!check(p, TOK_END) && !check(p, TOK_EOF) && !BAILED(p)) {
            skip_directives(p, n);
            if (check(p, TOK_END)) break;

            if (check(p, TOK_PROCEDURE) || check(p, TOK_FUNCTION)) {
                /* Method declaration in class */
                bool is_func = check(p, TOK_FUNCTION);
                advance(p);
                ast_node_t *m = ast_new(is_func ? AST_FUNC_DECL : AST_PROC_DECL, p->lex.line);
                /* Method name: ClassName.MethodName */
                strncpy(m->name, CUR(p).str_val, sizeof(m->name) - 1);
                advance(p);

                /* Parameters */
                if (check(p, TOK_LPAREN)) {
                    /* Skip params for class declaration (they're just signatures) */
                    int depth = 1;
                    advance(p);
                    while (depth > 0 && !check(p, TOK_EOF)) {
                        if (check(p, TOK_LPAREN)) depth++;
                        if (check(p, TOK_RPAREN)) depth--;
                        if (depth > 0) advance(p);
                    }
                    match(p, TOK_RPAREN);
                }

                /* Return type for functions */
                if (is_func && match(p, TOK_COLON)) {
                    advance(p); /* skip return type name */
                }

                match(p, TOK_SEMICOLON);
                /* OVERRIDE keyword */
                match_ident(p, "OVERRIDE");
                match(p, TOK_SEMICOLON);

                ast_add_child(n, m);
            } else if (check(p, TOK_IDENT)) {
                /* Field declaration */
                ast_node_t *field = ast_new(AST_FIELD, p->lex.line);
                strncpy(field->name, CUR(p).str_val, sizeof(field->name) - 1);
                advance(p);
                while (match(p, TOK_COMMA)) {
                    strncat(field->name, ",", sizeof(field->name) - strlen(field->name) - 1);
                    strncat(field->name, CUR(p).str_val, sizeof(field->name) - strlen(field->name) - 1);
                    advance(p);
                }
                if (match(p, TOK_COLON)) {
                    ast_add_child(field, parse_type(p));
                }
                match(p, TOK_SEMICOLON);
                ast_add_child(n, field);
            } else {
                /* Skip unknown tokens (comments turned into tokens, etc.) */
                advance(p);
            }
        }
        /* Note: don't consume END here — let the caller's type_decl handling get the semicolon */
        /* Actually, we do need END since this is the type definition */
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
            if (CUR(p).type == TOK_INTEGER) {
                n->int_val = CUR(p).int_val;
            } else if (CUR(p).type == TOK_IDENT) {
                /* CONST identifier for string length — store name for codegen lookup */
                strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
                n->int_val = 0;
            }
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

    /* Simple type name, possibly followed by ".." for subrange */
    if (check(p, TOK_IDENT) || check(p, TOK_INTEGER_KW) || check(p, TOK_BOOLEAN) ||
        check(p, TOK_CHAR) || check(p, TOK_LONGINT) || check(p, TOK_REAL_KW) || check(p, TOK_TEXT)) {
        char name[256];
        strncpy(name, CUR(p).str_val, sizeof(name) - 1);
        name[255] = '\0';
        int line = p->lex.line;
        advance(p);

        /* Check for identifier-based subrange: ident..ident */
        if (check(p, TOK_DOTDOT)) {
            advance(p); /* consume .. */
            ast_node_t *n = ast_new(AST_TYPE_SUBRANGE, line);
            ast_node_t *lo = ast_new(AST_IDENT_EXPR, line);
            strncpy(lo->name, name, sizeof(lo->name) - 1);
            ast_add_child(n, lo);
            ast_add_child(n, parse_expression(p));
            return n;
        }

        ast_node_t *n = ast_new(AST_TYPE_IDENT, line);
        strncpy(n->name, name, sizeof(n->name) - 1);
        return n;
    }

    /* Subrange starting with a constant (e.g., 0..255) or negative (-128..127) */
    if (check(p, TOK_INTEGER) || check(p, TOK_MINUS)) {
        ast_node_t *n = ast_new(AST_TYPE_SUBRANGE, p->lex.line);
        ast_add_child(n, parse_expression(p));
        expect(p, TOK_DOTDOT);
        ast_add_child(n, parse_expression(p));
        return n;
    }

    /* String constant as subrange low bound (e.g., 'A'..'Z') */
    if (check(p, TOK_STRING)) {
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
        if (check(p, TOK_SEMICOLON) || check(p, TOK_EOF)) break;
        ast_node_t *item = ast_new(AST_USES_ITEM, p->lex.line);
        strncpy(item->name, CUR(p).str_val, sizeof(item->name) - 1);
        advance(p);
        /* Handle path-style unit names: LibHW/hwint */
        if (match(p, TOK_SLASH)) {
            strncat(item->name, "/", sizeof(item->name) - strlen(item->name) - 1);
            strncat(item->name, CUR(p).str_val, sizeof(item->name) - strlen(item->name) - 1);
            advance(p);
        }
        ast_add_child(uses, item);
    } while (match(p, TOK_COMMA));
    expect(p, TOK_SEMICOLON);
    ast_add_child(parent, uses);
}

static void parse_const_section(parser_t *p, ast_node_t *parent) {
    while ((check(p, TOK_IDENT) || check(p, TOK_DIRECTIVE)) && !BAILED(p)) {
        skip_directives(p, parent);
        if (!check(p, TOK_IDENT)) break;

        ast_node_t *n = ast_new(AST_CONST_DECL, p->lex.line);
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);
        expect(p, TOK_EQ);
        ast_add_child(n, parse_expression(p));
        /* Semicolon optional before section keywords (Lisa Pascal quirk) */
        if (!match(p, TOK_SEMICOLON)) {
            if (!(check(p, TOK_PROCEDURE) || check(p, TOK_FUNCTION) ||
                  check(p, TOK_VAR) || check(p, TOK_TYPE) || check(p, TOK_CONST) ||
                  check(p, TOK_BEGIN) || check(p, TOK_IMPLEMENTATION))) {
                expect(p, TOK_SEMICOLON);
            }
        }
        ast_add_child(parent, n);
    }
}

static void parse_type_section(parser_t *p, ast_node_t *parent) {
    while ((check(p, TOK_IDENT) || check(p, TOK_DIRECTIVE)) && !BAILED(p)) {
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
    while ((check(p, TOK_IDENT) || check(p, TOK_DIRECTIVE)) && !BAILED(p)) {
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
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF) && !BAILED(p)) {
        skip_directives(p, NULL);
        ast_node_t *param = ast_new(AST_PARAM, p->lex.line);

        /* FUNCTION/PROCEDURE parameter (passing a routine) */
        if (check(p, TOK_FUNCTION) || check(p, TOK_PROCEDURE)) {
            bool is_func = check(p, TOK_FUNCTION);
            advance(p);
            strncpy(param->name, CUR(p).str_val, sizeof(param->name) - 1);
            advance(p);
            /* Skip any param list and return type */
            if (check(p, TOK_LPAREN)) {
                int depth = 1;
                advance(p);
                while (depth > 0 && !check(p, TOK_EOF)) {
                    if (check(p, TOK_LPAREN)) depth++;
                    if (check(p, TOK_RPAREN)) depth--;
                    if (depth > 0) advance(p);
                }
                match(p, TOK_RPAREN);
            }
            if (is_func && match(p, TOK_COLON)) {
                advance(p); /* return type name */
            }
            strncpy(param->str_val, is_func ? "FUNCPARAM" : "PROCPARAM", sizeof(param->str_val));
            ast_add_child(params, param);
            match(p, TOK_SEMICOLON); /* optional after proc/func params */
            continue;
        }

        /* VAR parameter */
        bool is_var = match(p, TOK_VAR);
        if (is_var) strncpy(param->str_val, "VAR", sizeof(param->str_val));

        /* Parameter name(s) — "a, b, c: type" creates separate params.
         * Collect names, parse type, then expand into individual params. */
        {
            char all_names[64][64];
            int name_count = 0;
            strncpy(all_names[name_count++], CUR(p).str_val, 63);
            advance(p);
            while (match(p, TOK_COMMA) && name_count < 64) {
                strncpy(all_names[name_count++], CUR(p).str_val, 63);
                advance(p);
            }

            expect(p, TOK_COLON);
            ast_node_t *type_node = parse_type(p);

            /* First param reuses the already-allocated 'param' node */
            strncpy(param->name, all_names[0], sizeof(param->name) - 1);
            ast_add_child(param, type_node);
            ast_add_child(params, param);

            /* Additional params get new nodes with a TYPE_IDENT referencing
             * the same type by name. Can't share the type_node (double-free). */
            for (int ni = 1; ni < name_count; ni++) {
                ast_node_t *extra = ast_new(AST_PARAM, p->lex.line);
                strncpy(extra->name, all_names[ni], sizeof(extra->name) - 1);
                if (is_var) strncpy(extra->str_val, "VAR", sizeof(extra->str_val));
                /* Create a reference to the same type */
                ast_node_t *tref = ast_new(type_node->type, p->lex.line);
                *tref = *type_node;  /* shallow copy */
                tref->children = NULL;  /* don't share children */
                tref->num_children = 0;
                tref->children_capacity = 0;
                ast_add_child(extra, tref);
                ast_add_child(params, extra);
            }
        }

        if (!match(p, TOK_SEMICOLON)) break;
    }
    expect(p, TOK_RPAREN);
    return params;
}

static void parse_proc_or_func(parser_t *p, ast_node_t *parent, bool is_func) {
    ast_node_t *n = ast_new(is_func ? AST_FUNC_DECL : AST_PROC_DECL, p->lex.line);

    /* Name — may include class prefix: ClassName.MethodName */
    if (check(p, TOK_IDENT)) {
        strncpy(n->name, CUR(p).str_val, sizeof(n->name) - 1);
        advance(p);
        /* Check for ClassName.MethodName */
        if (match(p, TOK_DOT)) {
            strncat(n->name, ".", sizeof(n->name) - strlen(n->name) - 1);
            strncat(n->name, CUR(p).str_val, sizeof(n->name) - strlen(n->name) - 1);
            advance(p);
        }
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

    /* In INTERFACE section, all procs/funcs are implicitly forward */
    if (p->in_interface) {
        strncpy(n->str_val, "FORWARD", sizeof(n->str_val));
    }
    /* OVERRIDE (Clascal) */
    else if (match_ident(p, "OVERRIDE")) {
        strncpy(n->str_val, "OVERRIDE", sizeof(n->str_val));
        match(p, TOK_SEMICOLON);
    }
    /* EXTERNAL or FORWARD */
    else if (match(p, TOK_EXTERNAL)) {
        strncpy(n->str_val, "EXTERNAL", sizeof(n->str_val));
        match(p, TOK_SEMICOLON);
    } else if (match(p, TOK_FORWARD)) {
        strncpy(n->str_val, "FORWARD", sizeof(n->str_val));
        match(p, TOK_SEMICOLON);
    } else {
        /* Full body: declarations + compound statement */
        int errors_before = p->num_errors;
        parse_declarations(p, n);
        if (check(p, TOK_BEGIN)) {
            ast_add_child(n, parse_compound_statement(p));
            match(p, TOK_SEMICOLON);
        }
        /* If parsing the body caused errors, recover by scanning forward
         * to the next FUNCTION/PROCEDURE/END at the top level. This prevents
         * one bad function from destroying all subsequent functions. */
        if (p->num_errors > errors_before) {
            while (!check(p, TOK_PROCEDURE) && !check(p, TOK_FUNCTION) &&
                   !check(p, TOK_END) && !check(p, TOK_EOF) && !BAILED(p)) {
                advance(p);
            }
        }
    }

    ast_add_child(parent, n);
}

static void parse_declarations(parser_t *p, ast_node_t *parent) {
    while (!BAILED(p)) {
        skip_directives(p, parent);

        if (match(p, TOK_LABEL)) {
            /* LABEL section: label1, label2, ... ; */
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

/* Parse METHODS OF ClassName; section (Clascal OOP) */
static void parse_methods_section(parser_t *p, ast_node_t *parent) {
    /* METHODS keyword already consumed */
    expect(p, TOK_OF);
    ast_node_t *methods = ast_new(AST_METHODS, p->lex.line);
    strncpy(methods->name, CUR(p).str_val, sizeof(methods->name) - 1);
    advance(p); /* class name */
    match(p, TOK_SEMICOLON); /* optional — some files omit it */

    /* Parse method implementations until next METHODS OF or end of file */
    while (!check(p, TOK_EOF) && !check(p, TOK_METHODS) && !check(p, TOK_END) && !BAILED(p)) {
        skip_directives(p, methods);
        if (check(p, TOK_EOF) || check(p, TOK_METHODS) || check(p, TOK_END)) break;

        if (check(p, TOK_PROCEDURE)) {
            advance(p);
            parse_proc_or_func(p, methods, false);
        } else if (check(p, TOK_FUNCTION)) {
            advance(p);
            parse_proc_or_func(p, methods, true);
        } else {
            /* Might be stray directives or other declarations */
            advance(p);
        }
    }
    ast_add_child(parent, methods);
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

    /* Lisa Pascal INTRINSIC units: skip the keyword, optional qualifier, and semicolon.
     * Syntax: INTRINSIC [SHARED|RESIDENT|...] ; */
    if (CURTYPE(p) == TOK_IDENT &&
        strncasecmp(CUR(p).str_val, "INTRINSIC", 9) == 0) {
        advance(p);
        /* Skip optional qualifier (SHARED, RESIDENT, etc.) */
        while (CURTYPE(p) == TOK_IDENT) advance(p);
        match(p, TOK_SEMICOLON);
    }

    skip_directives(p, unit);

    /* INTERFACE section — all procedure/function declarations are implicitly FORWARD */
    if (match(p, TOK_INTERFACE)) {
        ast_node_t *iface = ast_new(AST_INTERFACE, p->lex.line);
        p->in_interface = true;
        skip_directives(p, iface);
        if (match(p, TOK_USES)) parse_uses(p, iface);
        parse_declarations(p, iface);
        p->in_interface = false;
        ast_add_child(unit, iface);
    }

    /* IMPLEMENTATION section */
    if (match(p, TOK_IMPLEMENTATION)) {
        ast_node_t *impl = ast_new(AST_IMPLEMENTATION, p->lex.line);
        skip_directives(p, impl);
        if (match(p, TOK_USES)) parse_uses(p, impl);
        parse_declarations(p, impl);

        /* Handle METHODS OF sections (Clascal) */
        while (check(p, TOK_METHODS) && !BAILED(p)) {
            advance(p); /* consume METHODS */
            parse_methods_section(p, impl);
        }

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

/* Parse a code fragment (file that doesn't start with UNIT/PROGRAM) */
static ast_node_t *parse_fragment(parser_t *p) {
    ast_node_t *frag = ast_new(AST_FRAGMENT, p->lex.line);
    strncpy(frag->name, p->lex.filename, sizeof(frag->name) - 1);

    while (!check(p, TOK_EOF) && !BAILED(p)) {
        const char *prev_pos = p->lex.pos;
        skip_directives(p, frag);
        if (check(p, TOK_EOF)) break;

        if (check(p, TOK_PROCEDURE)) {
            advance(p);
            parse_proc_or_func(p, frag, false);
        } else if (check(p, TOK_FUNCTION)) {
            advance(p);
            parse_proc_or_func(p, frag, true);
        } else if (match(p, TOK_METHODS)) {
            parse_methods_section(p, frag);
        } else if (match(p, TOK_CONST)) {
            parse_const_section(p, frag);
        } else if (match(p, TOK_TYPE)) {
            parse_type_section(p, frag);
        } else if (match(p, TOK_VAR)) {
            parse_var_section(p, frag);
        } else if (match(p, TOK_LABEL)) {
            do { advance(p); } while (match(p, TOK_COMMA));
            expect(p, TOK_SEMICOLON);
        } else if (check(p, TOK_BEGIN)) {
            ast_add_child(frag, parse_compound_statement(p));
            match(p, TOK_SEMICOLON);
        } else if (match(p, TOK_USES)) {
            parse_uses(p, frag);
        } else if (check(p, TOK_END)) {
            advance(p);
            match(p, TOK_DOT);
        } else {
            /* Skip unknown token */
            advance(p);
        }

        /* Infinite loop guard: if we haven't advanced, force skip */
        if (p->lex.pos == prev_pos && !check(p, TOK_EOF)) {
            advance(p);
        }
    }

    return frag;
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

    if (check(p, TOK_EOF)) {
        /* Empty file (e.g., only {$SETC} directives) — not an error */
        p->root = ast_new(AST_FRAGMENT, p->lex.line);
    } else if (match(p, TOK_UNIT)) {
        p->root = parse_unit(p);
    } else if (match(p, TOK_PROGRAM)) {
        p->root = parse_program(p);
    } else if (check(p, TOK_PROCEDURE) || check(p, TOK_FUNCTION) ||
               check(p, TOK_CONST) || check(p, TOK_TYPE) || check(p, TOK_VAR) ||
               check(p, TOK_BEGIN) || check(p, TOK_LABEL) ||
               check(p, TOK_METHODS) || check(p, TOK_USES) ||
               check(p, TOK_IDENT)) {
        /* Code fragment — not a full unit/program */
        p->root = parse_fragment(p);
    } else {
        parser_error(p, "expected UNIT, PROGRAM, or code fragment");
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
