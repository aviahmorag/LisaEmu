/*
 * LisaEm Toolchain — Lisa Pascal Lexer
 */

#include "pascal_lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Keyword table */
static const struct { const char *word; token_type_t type; } keywords[] = {
    {"AND", TOK_AND}, {"ARRAY", TOK_ARRAY}, {"BEGIN", TOK_BEGIN},
    {"BOOLEAN", TOK_BOOLEAN}, {"CASE", TOK_CASE}, {"CHAR", TOK_CHAR},
    {"CONST", TOK_CONST}, {"DIV", TOK_DIV}, {"DO", TOK_DO},
    {"DOWNTO", TOK_DOWNTO}, {"ELSE", TOK_ELSE}, {"END", TOK_END},
    {"EXTERNAL", TOK_EXTERNAL}, {"FILE", TOK_FILE}, {"FOR", TOK_FOR},
    {"FORWARD", TOK_FORWARD}, {"FUNCTION", TOK_FUNCTION}, {"GOTO", TOK_GOTO},
    {"IF", TOK_IF}, {"IMPLEMENTATION", TOK_IMPLEMENTATION}, {"IN", TOK_IN},
    {"INLINE", TOK_INLINE}, {"INTEGER", TOK_INTEGER_KW},
    {"INTERFACE", TOK_INTERFACE}, {"LABEL", TOK_LABEL}, {"LONGINT", TOK_LONGINT},
    {"MOD", TOK_MOD}, {"NIL", TOK_NIL}, {"NOT", TOK_NOT}, {"OF", TOK_OF},
    {"OR", TOK_OR}, {"OTHERWISE", TOK_OTHERWISE}, {"PACKED", TOK_PACKED},
    {"PROCEDURE", TOK_PROCEDURE}, {"PROGRAM", TOK_PROGRAM},
    {"REAL", TOK_REAL_KW}, {"RECORD", TOK_RECORD}, {"REPEAT", TOK_REPEAT},
    {"SET", TOK_SET}, {"SHL", TOK_SHL}, {"SHR", TOK_SHR},
    {"STRING", TOK_STRING_KW}, {"TEXT", TOK_TEXT}, {"THEN", TOK_THEN},
    {"TO", TOK_TO}, {"TYPE", TOK_TYPE}, {"UNIT", TOK_UNIT},
    {"UNTIL", TOK_UNTIL}, {"USES", TOK_USES}, {"VAR", TOK_VAR},
    {"WHILE", TOK_WHILE}, {"WITH", TOK_WITH}, {"XOR", TOK_XOR},
    {"METHODS", TOK_METHODS}, {"SUBCLASS", TOK_SUBCLASS},
    /* Note: OVERRIDE is NOT a keyword — it's used as a variable name in some files.
     * It's handled contextually in the parser (after method declarations in SUBCLASS). */
    {NULL, TOK_EOF}
};

static bool str_eq_upper(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

static token_type_t lookup_keyword(const char *ident) {
    for (int i = 0; keywords[i].word; i++) {
        if (str_eq_upper(ident, keywords[i].word))
            return keywords[i].type;
    }
    return TOK_IDENT;
}

/* ======================================================================== */

void lexer_init(lexer_t *lex, const char *source, const char *filename) {
    memset(lex, 0, sizeof(lexer_t));
    lex->source = source;
    lex->pos = source;
    lex->filename = filename;
    lex->line = 1;
    lex->col = 1;
}

/* Conditional compilation helpers */

static bool lexer_is_active(lexer_t *lex) {
    /* We're active if all levels of the conditional stack are active */
    for (int i = 0; i < lex->cond_depth; i++) {
        if (!lex->cond_stack[i].active) return false;
    }
    return true;
}

static bool lexer_lookup_symbol(lexer_t *lex, const char *name) {
    for (int i = 0; i < lex->num_symbols; i++) {
        if (str_eq_upper(lex->symbols[i].name, name))
            return lex->symbols[i].value;
    }
    return false; /* Unknown symbols default to FALSE (release build) */
}

static void lexer_set_symbol(lexer_t *lex, const char *name, bool value) {
    for (int i = 0; i < lex->num_symbols; i++) {
        if (str_eq_upper(lex->symbols[i].name, name)) {
            lex->symbols[i].value = value;
            return;
        }
    }
    if (lex->num_symbols < LEX_MAX_SYMBOLS) {
        strncpy(lex->symbols[lex->num_symbols].name, name, 63);
        lex->symbols[lex->num_symbols].value = value;
        lex->num_symbols++;
    }
}

/* Parse a $SETC directive: $SETC name := value */
static void lexer_handle_setc(lexer_t *lex, const char *directive) {
    /* directive looks like: "$SETC name := value" or "$SETC name:=value" */
    const char *p = directive + 5; /* skip "$SETC" */
    while (*p == ' ') p++;

    char name[64];
    int ni = 0;
    while (*p && *p != ':' && *p != '=' && *p != ' ' && ni < 63) {
        name[ni++] = *p++;
    }
    name[ni] = '\0';

    /* Skip := */
    while (*p == ' ' || *p == ':' || *p == '=') p++;

    /* Parse value — TRUE, FALSE, or a symbol name */
    char val_str[64];
    int vi = 0;
    while (*p && *p != ' ' && *p != '}' && vi < 63) {
        val_str[vi++] = *p++;
    }
    val_str[vi] = '\0';

    bool value;
    if (str_eq_upper(val_str, "TRUE")) value = true;
    else if (str_eq_upper(val_str, "FALSE")) value = false;
    else value = lexer_lookup_symbol(lex, val_str); /* $SETC a := b */

    lexer_set_symbol(lex, name, value);
}

/* Parse a $IFC directive: $IFC name [or name] [and name] */
static bool lexer_eval_ifc(lexer_t *lex, const char *directive) {
    const char *p = directive + 4; /* skip "$IFC" */
    while (*p == ' ') p++;

    /* Parse first operand */
    char name[64];
    int ni = 0;
    while (*p && *p != ' ' && *p != '}' && *p != '*' && ni < 63) {
        name[ni++] = *p++;
    }
    name[ni] = '\0';

    bool result;
    if (str_eq_upper(name, "TRUE")) result = true;
    else if (str_eq_upper(name, "FALSE")) result = false;
    else if (str_eq_upper(name, "NOT")) {
        /* $IFC NOT name */
        while (*p == ' ') p++;
        ni = 0;
        while (*p && *p != ' ' && *p != '}' && *p != '*' && ni < 63) {
            name[ni++] = *p++;
        }
        name[ni] = '\0';
        result = !lexer_lookup_symbol(lex, name);
    } else {
        result = lexer_lookup_symbol(lex, name);
    }

    /* Check for AND/OR operators */
    while (*p == ' ') p++;
    while (*p) {
        char op[8];
        ni = 0;
        while (*p && *p != ' ' && ni < 7) { op[ni++] = *p++; }
        op[ni] = '\0';
        while (*p == ' ') p++;

        char operand[64];
        ni = 0;
        while (*p && *p != ' ' && *p != '}' && *p != '*' && ni < 63) {
            operand[ni++] = *p++;
        }
        operand[ni] = '\0';
        if (!operand[0]) break;

        bool val = lexer_lookup_symbol(lex, operand);
        if (str_eq_upper(op, "OR")) result = result || val;
        else if (str_eq_upper(op, "AND")) result = result && val;
        while (*p == ' ') p++;
    }

    return result;
}

/* Process a directive — returns true if the directive was handled (conditional compilation) */
static bool lexer_handle_directive(lexer_t *lex, const char *dir) {
    /* dir starts with '$' */
    if (dir[0] != '$') return false;

    /* $SETC */
    if ((dir[1] == 'S' || dir[1] == 's') &&
        (dir[2] == 'E' || dir[2] == 'e') &&
        (dir[3] == 'T' || dir[3] == 't') &&
        (dir[4] == 'C' || dir[4] == 'c')) {
        if (lexer_is_active(lex)) {
            lexer_handle_setc(lex, dir);
        }
        return true;
    }

    /* $IFC */
    if ((dir[1] == 'I' || dir[1] == 'i') &&
        (dir[2] == 'F' || dir[2] == 'f') &&
        (dir[3] == 'C' || dir[3] == 'c')) {
        bool cond = lexer_is_active(lex) ? lexer_eval_ifc(lex, dir) : false;
        if (lex->cond_depth < LEX_MAX_COND_DEPTH) {
            lex->cond_stack[lex->cond_depth].active = cond;
            lex->cond_stack[lex->cond_depth].had_true = cond;
            lex->cond_depth++;
        }
        return true;
    }

    /* $ELSEC */
    if ((dir[1] == 'E' || dir[1] == 'e') &&
        (dir[2] == 'L' || dir[2] == 'l') &&
        (dir[3] == 'S' || dir[3] == 's') &&
        (dir[4] == 'E' || dir[4] == 'e') &&
        (dir[5] == 'C' || dir[5] == 'c')) {
        if (lex->cond_depth > 0) {
            int top = lex->cond_depth - 1;
            if (!lex->cond_stack[top].had_true) {
                lex->cond_stack[top].active = true;
                lex->cond_stack[top].had_true = true;
            } else {
                lex->cond_stack[top].active = false;
            }
        }
        return true;
    }

    /* $ENDC */
    if ((dir[1] == 'E' || dir[1] == 'e') &&
        (dir[2] == 'N' || dir[2] == 'n') &&
        (dir[3] == 'D' || dir[3] == 'd') &&
        (dir[4] == 'C' || dir[4] == 'c')) {
        if (lex->cond_depth > 0) {
            lex->cond_depth--;
        }
        return true;
    }

    return false;
}

static char peek_char(lexer_t *lex) {
    return *lex->pos;
}

static char next_char(lexer_t *lex) {
    char c = *lex->pos;
    if (c) {
        lex->pos++;
        if (c == '\n') { lex->line++; lex->col = 1; }
        else lex->col++;
    }
    return c;
}

static void skip_whitespace_and_comments(lexer_t *lex) {
    while (1) {
        /* Whitespace */
        while (*lex->pos && isspace((unsigned char)*lex->pos)) {
            next_char(lex);
        }

        /* { } comment (also handles {$ directives elsewhere) */
        if (*lex->pos == '{' && lex->pos[1] != '$') {
            next_char(lex); /* consume { */
            while (*lex->pos && *lex->pos != '}') next_char(lex);
            if (*lex->pos == '}') next_char(lex);
            continue;
        }

        /* (* *) comment (also handles (*$ directives elsewhere) */
        if (*lex->pos == '(' && lex->pos[1] == '*' && lex->pos[2] != '$') {
            next_char(lex); next_char(lex); /* consume (* */
            while (*lex->pos) {
                if (*lex->pos == '*' && lex->pos[1] == ')') {
                    next_char(lex); next_char(lex);
                    break;
                }
                next_char(lex);
            }
            continue;
        }

        break;
    }
}

static token_t make_token(lexer_t *lex, token_type_t type) {
    token_t tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = type;
    tok.line = lex->line;
    tok.col = lex->col;
    return tok;
}

/* Lex a single raw token (no conditional compilation processing) */
static token_t lexer_raw_next(lexer_t *lex) {
    while (1) {
    skip_whitespace_and_comments(lex);

    if (!*lex->pos) {
        return make_token(lex, TOK_EOF);
    }

    token_t tok = make_token(lex, TOK_ERROR);
    char c = peek_char(lex);

    /* Compiler directive: {$...} */
    if (c == '{' && lex->pos[1] == '$') {
        /* Distinguish hex comments {$80} {$A0} from directives {$S seg} {$IFC x}.
         * Real directives: $S, $R, $U, $I, $D, $E (SETC/ENDC/ELSEC/IFC), $L, $C, $%
         * Hex comments: {$80}, {$A0}, {$9D in a byte}, etc.
         * Rule: if the content after $ parses as a hex number (1-4 hex digits)
         * optionally followed by non-alnum, it's a comment. */
        {
            const char *scan = lex->pos + 2; /* first char after {$ */
            int hex_count = 0;
            while (isxdigit((unsigned char)*scan)) { hex_count++; scan++; }
            /* If we got 1-4 hex digits followed by non-alnum (space, }, text) → hex comment */
            if (hex_count >= 1 && hex_count <= 4 &&
                (*scan == '}' || *scan == ' ' || *scan == '\0' || !isalnum((unsigned char)*scan))) {
                /* Hex comment — skip */
                next_char(lex); /* { */
                while (*lex->pos && *lex->pos != '}') next_char(lex);
                if (*lex->pos == '}') next_char(lex);
                continue; /* re-lex */
            }
        }
        tok.type = TOK_DIRECTIVE;
        next_char(lex); /* { */
        next_char(lex); /* $ */
        int i = 0;
        tok.str_val[i++] = '$';
        while (*lex->pos && *lex->pos != '}' && i < 1022) {
            tok.str_val[i++] = next_char(lex);
        }
        tok.str_val[i] = '\0';
        if (*lex->pos == '}') next_char(lex);
        return tok;
    }

    /* Compiler directive: (*$...*) */
    if (c == '(' && lex->pos[1] == '*' && lex->pos[2] == '$') {
        tok.type = TOK_DIRECTIVE;
        next_char(lex); /* ( */
        next_char(lex); /* * */
        next_char(lex); /* $ */
        int i = 0;
        tok.str_val[i++] = '$';
        while (*lex->pos) {
            if (*lex->pos == '*' && lex->pos[1] == ')') {
                next_char(lex); next_char(lex);
                break;
            }
            if (i < 1022) tok.str_val[i++] = *lex->pos;
            next_char(lex);
        }
        tok.str_val[i] = '\0';
        return tok;
    }

    /* Number */
    if (isdigit((unsigned char)c) || (c == '$' && isxdigit((unsigned char)lex->pos[1]))) {
        tok.type = TOK_INTEGER;
        if (c == '$') {
            /* Hex */
            next_char(lex);
            int64_t val = 0;
            while (isxdigit((unsigned char)*lex->pos)) {
                char h = next_char(lex);
                val = val * 16 + (isdigit((unsigned char)h) ? h - '0' :
                      toupper((unsigned char)h) - 'A' + 10);
            }
            tok.int_val = val;
        } else {
            /* Decimal (possibly real) */
            int64_t val = 0;
            while (isdigit((unsigned char)*lex->pos)) {
                val = val * 10 + (next_char(lex) - '0');
            }
            if (*lex->pos == '.' && lex->pos[1] != '.') {
                /* Real number */
                tok.type = TOK_REAL;
                double frac = 0.1;
                double rval = (double)val;
                next_char(lex); /* consume . */
                while (isdigit((unsigned char)*lex->pos)) {
                    rval += (next_char(lex) - '0') * frac;
                    frac *= 0.1;
                }
                if (*lex->pos == 'E' || *lex->pos == 'e') {
                    next_char(lex);
                    int sign = 1;
                    if (*lex->pos == '+') next_char(lex);
                    else if (*lex->pos == '-') { sign = -1; next_char(lex); }
                    int exp = 0;
                    while (isdigit((unsigned char)*lex->pos)) {
                        exp = exp * 10 + (next_char(lex) - '0');
                    }
                    double mult = 1.0;
                    for (int e = 0; e < exp; e++) mult *= 10.0;
                    if (sign < 0) rval /= mult; else rval *= mult;
                }
                tok.real_val = rval;
            } else {
                tok.int_val = val;
            }
        }
        lex->current = tok;
        return tok;
    }

    /* String literal */
    if (c == '\'') {
        tok.type = TOK_STRING;
        next_char(lex);
        int i = 0;
        while (*lex->pos) {
            if (*lex->pos == '\'') {
                next_char(lex);
                if (*lex->pos == '\'') {
                    /* Escaped quote '' */
                    if (i < 1022) tok.str_val[i++] = '\'';
                    next_char(lex);
                    continue;
                }
                break;
            }
            if (i < 1022) tok.str_val[i++] = *lex->pos;
            next_char(lex);
        }
        tok.str_val[i] = '\0';
        lex->current = tok;
        return tok;
    }

    /* Identifier or keyword (% prefix for Lisa intrinsics) */
    if (isalpha((unsigned char)c) || c == '_' || c == '%') {
        int i = 0;
        while (isalnum((unsigned char)*lex->pos) || *lex->pos == '_' || *lex->pos == '%') {
            if (i < 1022) tok.str_val[i++] = next_char(lex);
            else next_char(lex);
        }
        tok.str_val[i] = '\0';
        tok.type = lookup_keyword(tok.str_val);
        lex->current = tok;
        return tok;
    }

    /* Operators and punctuation */
    next_char(lex);
    switch (c) {
        case '+': tok.type = TOK_PLUS; break;
        case '-': tok.type = TOK_MINUS; break;
        case '*': tok.type = TOK_STAR; break;
        case '/': tok.type = TOK_SLASH; break;
        case '=': tok.type = TOK_EQ; break;
        case '^': tok.type = TOK_CARET; break;
        case '@': tok.type = TOK_AT; break;
        case ',': tok.type = TOK_COMMA; break;
        case ';': tok.type = TOK_SEMICOLON; break;
        case '(': tok.type = TOK_LPAREN; break;
        case ')': tok.type = TOK_RPAREN; break;
        case '[': tok.type = TOK_LBRACKET; break;
        case ']': tok.type = TOK_RBRACKET; break;

        case ':':
            if (*lex->pos == '=') {
                next_char(lex);
                tok.type = TOK_ASSIGN;
            } else {
                tok.type = TOK_COLON;
            }
            break;

        case '.':
            if (*lex->pos == '.') {
                next_char(lex);
                tok.type = TOK_DOTDOT;
            } else {
                tok.type = TOK_DOT;
            }
            break;

        case '<':
            if (*lex->pos == '=') { next_char(lex); tok.type = TOK_LE; }
            else if (*lex->pos == '>') { next_char(lex); tok.type = TOK_NE; }
            else tok.type = TOK_LT;
            break;

        case '>':
            if (*lex->pos == '=') { next_char(lex); tok.type = TOK_GE; }
            else tok.type = TOK_GT;
            break;

        case '#':
            /* # character — skip and continue lexing */
            continue;  /* top of while(1) */

        default:
            if ((unsigned char)c >= 128) {
                /* Non-ASCII character (file padding) — skip */
                continue;  /* top of while(1) */
            }
            tok.type = TOK_ERROR;
            snprintf(tok.str_val, sizeof(tok.str_val), "unexpected character: '%c' (0x%02X)", c, (unsigned char)c);
            break;
    }

    return tok;
    } /* end while(1) */
}

/* Public lexer_next: wraps raw lexing with conditional compilation */
token_t lexer_next(lexer_t *lex) {
    if (lex->has_lookahead) {
        lex->has_lookahead = false;
        lex->current = lex->lookahead;
        return lex->current;
    }

    while (1) {
        token_t tok = lexer_raw_next(lex);

        if (tok.type == TOK_EOF) {
            lex->current = tok;
            return tok;
        }

        /* Handle conditional compilation directives */
        if (tok.type == TOK_DIRECTIVE) {
            if (lexer_handle_directive(lex, tok.str_val)) {
                /* Conditional directive consumed — continue lexing */
                continue;
            }
            /* Non-conditional directive — pass through if active */
            if (!lexer_is_active(lex)) continue;
            lex->current = tok;
            return tok;
        }

        /* If we're in an inactive conditional branch, skip this token */
        if (!lexer_is_active(lex)) continue;

        lex->current = tok;
        return tok;
    }
}

token_t lexer_peek(lexer_t *lex) {
    if (!lex->has_lookahead) {
        token_t saved = lex->current;
        lex->lookahead = lexer_next(lex);
        lex->current = saved;
        lex->has_lookahead = true;
    }
    return lex->lookahead;
}

bool lexer_match(lexer_t *lex, token_type_t type) {
    if (lex->current.type == type) {
        lexer_next(lex);
        return true;
    }
    return false;
}

bool lexer_expect(lexer_t *lex, token_type_t type) {
    if (lex->current.type == type) {
        lexer_next(lex);
        return true;
    }
    fprintf(stderr, "%s:%d:%d: expected %s, got %s\n",
            lex->filename, lex->line, lex->col,
            token_type_name(type), token_type_name(lex->current.type));
    return false;
}

const char *token_type_name(token_type_t type) {
    switch (type) {
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
        case TOK_INTEGER: return "INTEGER";
        case TOK_REAL: return "REAL";
        case TOK_STRING: return "STRING";
        case TOK_IDENT: return "IDENT";
        case TOK_DIRECTIVE: return "DIRECTIVE";
        case TOK_AND: return "AND"; case TOK_ARRAY: return "ARRAY";
        case TOK_BEGIN: return "BEGIN"; case TOK_CASE: return "CASE";
        case TOK_CONST: return "CONST"; case TOK_DIV: return "DIV";
        case TOK_DO: return "DO"; case TOK_DOWNTO: return "DOWNTO";
        case TOK_ELSE: return "ELSE"; case TOK_END: return "END";
        case TOK_FILE: return "FILE"; case TOK_FOR: return "FOR";
        case TOK_FUNCTION: return "FUNCTION"; case TOK_GOTO: return "GOTO";
        case TOK_IF: return "IF"; case TOK_IMPLEMENTATION: return "IMPLEMENTATION";
        case TOK_IN: return "IN"; case TOK_INTERFACE: return "INTERFACE";
        case TOK_LABEL: return "LABEL"; case TOK_MOD: return "MOD";
        case TOK_NIL: return "NIL"; case TOK_NOT: return "NOT";
        case TOK_OF: return "OF"; case TOK_OR: return "OR";
        case TOK_OTHERWISE: return "OTHERWISE"; case TOK_PACKED: return "PACKED";
        case TOK_PROCEDURE: return "PROCEDURE"; case TOK_PROGRAM: return "PROGRAM";
        case TOK_RECORD: return "RECORD"; case TOK_REPEAT: return "REPEAT";
        case TOK_SET: return "SET"; case TOK_STRING_KW: return "STRING";
        case TOK_THEN: return "THEN"; case TOK_TO: return "TO";
        case TOK_TYPE: return "TYPE"; case TOK_UNIT: return "UNIT";
        case TOK_UNTIL: return "UNTIL"; case TOK_USES: return "USES";
        case TOK_VAR: return "VAR"; case TOK_WHILE: return "WHILE";
        case TOK_WITH: return "WITH"; case TOK_XOR: return "XOR";
        case TOK_EXTERNAL: return "EXTERNAL"; case TOK_FORWARD: return "FORWARD";
        case TOK_INLINE: return "INLINE"; case TOK_BOOLEAN: return "BOOLEAN";
        case TOK_CHAR: return "CHAR"; case TOK_INTEGER_KW: return "INTEGER";
        case TOK_LONGINT: return "LONGINT"; case TOK_REAL_KW: return "REAL";
        case TOK_TEXT: return "TEXT"; case TOK_SHL: return "SHL";
        case TOK_SHR: return "SHR";
        case TOK_METHODS: return "METHODS";
        case TOK_SUBCLASS: return "SUBCLASS";
        case TOK_OVERRIDE: return "OVERRIDE";
        case TOK_PLUS: return "+"; case TOK_MINUS: return "-";
        case TOK_STAR: return "*"; case TOK_SLASH: return "/";
        case TOK_ASSIGN: return ":="; case TOK_EQ: return "=";
        case TOK_NE: return "<>"; case TOK_LT: return "<";
        case TOK_LE: return "<="; case TOK_GT: return ">";
        case TOK_GE: return ">="; case TOK_LPAREN: return "(";
        case TOK_RPAREN: return ")"; case TOK_LBRACKET: return "[";
        case TOK_RBRACKET: return "]"; case TOK_DOT: return ".";
        case TOK_DOTDOT: return ".."; case TOK_COMMA: return ",";
        case TOK_SEMICOLON: return ";"; case TOK_COLON: return ":";
        case TOK_CARET: return "^"; case TOK_AT: return "@";
        default: return "?";
    }
}
