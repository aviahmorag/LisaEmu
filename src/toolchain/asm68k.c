/*
 * LisaEm Toolchain — 68000 Cross-Assembler
 *
 * Two-pass assembler for Lisa-style Motorola 68000 assembly.
 * Pass 1: Collect symbols, compute sizes
 * Pass 2: Generate code, resolve references
 */

#include "asm68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* ========================================================================
 * String utilities
 * ======================================================================== */

static void str_upper(char *s) {
    for (; *s; s++) *s = toupper((unsigned char)*s);
}

static void str_trim(char *s) {
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    char *start = s;
    while (isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static bool str_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

/* ========================================================================
 * Error reporting
 * ======================================================================== */

static void asm_error(asm68k_t *as, const char *fmt, ...) {
    if (as->num_errors >= ASM_MAX_ERRORS) return;
    asm_error_t *err = &as->errors[as->num_errors++];
    err->line = as->line_num;
    strncpy(err->filename, as->current_file, sizeof(err->filename) - 1);

    va_list args;
    va_start(args, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, args);
    va_end(args);

    if (as->pass == 2) {
        fprintf(stderr, "%s:%d: error: %s\n", as->current_file, as->line_num, err->message);
    }
}

/* ========================================================================
 * Symbol table
 * ======================================================================== */

/* Lisa assembler truncates symbols to 8 significant characters for internal
 * matching (.REF/.DEF resolution). Exported symbols keep their full names
 * for Pascal interop — the truncation only affects the assembler's own
 * find_symbol lookups. */
#define ASM_SYM_SIGCHARS 8

static int find_symbol(asm68k_t *as, const char *name) {
    for (int i = 0; i < as->num_symbols; i++) {
        if (strncasecmp(as->symbols[i].name, name, ASM_SYM_SIGCHARS) == 0)
            return i;
    }
    return -1;
}

static int add_symbol(asm68k_t *as, const char *name, sym_type_t type, int32_t value) {
    int idx = find_symbol(as, name);
    if (idx >= 0) {
        /* Update existing symbol */
        if (as->pass == 1 && as->symbols[idx].defined && type == SYM_LABEL) {
            asm_error(as, "symbol '%s' already defined", name);
            return idx;
        }
        as->symbols[idx].value = value;
        as->symbols[idx].defined = true;
        if (type != SYM_REF) as->symbols[idx].type = type;
        return idx;
    }

    if (as->num_symbols >= ASM_MAX_SYMBOLS) {
        asm_error(as, "symbol table full");
        return -1;
    }

    idx = as->num_symbols++;
    strncpy(as->symbols[idx].name, name, ASM_MAX_LABEL - 1);
    /* Note: symbol names are stored in full for linker export.
     * The 8-char significant comparison happens in find_symbol(). */
    as->symbols[idx].type = type;
    as->symbols[idx].value = value;
    as->symbols[idx].defined = (type != SYM_REF);
    as->symbols[idx].exported = (type == SYM_DEF || type == SYM_PROC || type == SYM_FUNC);
    as->symbols[idx].external = (type == SYM_REF);
    as->symbols[idx].section = as->current_section;
    return idx;
}

static int32_t get_symbol_value(asm68k_t *as, const char *name) {
    int idx = find_symbol(as, name);
    if (idx < 0) {
        if (as->pass == 2) asm_error(as, "undefined symbol '%s'", name);
        return 0;
    }
    return as->symbols[idx].value;
}

/* ========================================================================
 * Output / Section management
 * ======================================================================== */

static void ensure_section(asm68k_t *as) {
    if (as->num_sections == 0) {
        /* Create default section */
        as->num_sections = 1;
        as->current_section = 0;
        strcpy(as->sections[0].name, "CODE");
        as->sections[0].data = malloc(ASM_MAX_OUTPUT);
        as->sections[0].capacity = ASM_MAX_OUTPUT;
        as->sections[0].size = 0;
        as->sections[0].origin = 0;
    }
}

static void emit8(asm68k_t *as, uint8_t val) {
    ensure_section(as);
    asm_section_t *sec = &as->sections[as->current_section];
    if (as->pass == 2 && sec->size < sec->capacity) {
        sec->data[sec->size] = val;
    }
    sec->size++;
    as->pc++;
}

static void emit16(asm68k_t *as, uint16_t val) {
    emit8(as, (val >> 8) & 0xFF);
    emit8(as, val & 0xFF);
}

static void emit32(asm68k_t *as, uint32_t val) {
    emit16(as, (val >> 16) & 0xFFFF);
    emit16(as, val & 0xFFFF);
}

/* ========================================================================
 * Expression parser
 *
 * Handles: decimal, hex ($), octal (@), binary (%), char ('x'),
 *          symbols, +, -, *, /, parentheses
 * ======================================================================== */

typedef struct {
    const char *p;
    asm68k_t *as;
} expr_ctx_t;

static int32_t parse_expr(expr_ctx_t *ctx);

static void skip_spaces(expr_ctx_t *ctx) {
    while (*ctx->p == ' ' || *ctx->p == '\t') ctx->p++;
}

static int32_t parse_number(expr_ctx_t *ctx) {
    skip_spaces(ctx);
    const char *p = ctx->p;

    if (*p == '$') {
        /* Hex */
        p++;
        int32_t val = 0;
        while (isxdigit((unsigned char)*p)) {
            val = val * 16 + (isdigit((unsigned char)*p) ? *p - '0' :
                  toupper((unsigned char)*p) - 'A' + 10);
            p++;
        }
        ctx->p = p;
        return val;
    }

    if (*p == '@') {
        /* @ followed by digits = local label reference (e.g., @1, @100).
         * Lisa assembler doesn't use @ for octal. */
        char name[ASM_MAX_LABEL];
        int i = 0;
        while (isalnum((unsigned char)*p) || *p == '@') {
            if (i < ASM_MAX_LABEL - 1) name[i++] = *p;
            p++;
        }
        name[i] = '\0';
        ctx->p = p;
        return get_symbol_value(ctx->as, name);
    }

    if (*p == '%') {
        /* Binary */
        p++;
        int32_t val = 0;
        while (*p == '0' || *p == '1') {
            val = val * 2 + (*p - '0');
            p++;
        }
        ctx->p = p;
        return val;
    }

    if (*p == '\'') {
        /* Character constant */
        p++;
        int32_t val = 0;
        while (*p && *p != '\'') {
            val = (val << 8) | (uint8_t)*p;
            p++;
        }
        if (*p == '\'') p++;
        ctx->p = p;
        return val;
    }

    if (isdigit((unsigned char)*p)) {
        int32_t val = 0;
        while (isdigit((unsigned char)*p)) {
            val = val * 10 + (*p - '0');
            p++;
        }
        ctx->p = p;
        return val;
    }

    /* Symbol name */
    if (isalpha((unsigned char)*p) || *p == '_' || *p == '.' || *p == '%' || *p == '@') {
        char name[ASM_MAX_LABEL];
        int i = 0;
        while (isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == '%' || *p == '@') {
            if (i < ASM_MAX_LABEL - 1) name[i++] = *p;
            p++;
        }
        name[i] = '\0';
        ctx->p = p;

        /* Special: * means current PC */
        if (strcmp(name, "*") == 0) return ctx->as->pc;

        return get_symbol_value(ctx->as, name);
    }

    if (*p == '*') {
        ctx->p = p + 1;
        return ctx->as->pc;
    }

    return 0;
}

static int32_t parse_unary(expr_ctx_t *ctx) {
    skip_spaces(ctx);
    if (*ctx->p == '-') {
        ctx->p++;
        return -parse_unary(ctx);
    }
    if (*ctx->p == '+') {
        ctx->p++;
        return parse_unary(ctx);
    }
    if (*ctx->p == '~' || *ctx->p == '!') {
        ctx->p++;
        return ~parse_unary(ctx);
    }
    if (*ctx->p == '(') {
        ctx->p++;
        int32_t val = parse_expr(ctx);
        skip_spaces(ctx);
        if (*ctx->p == ')') ctx->p++;
        return val;
    }
    return parse_number(ctx);
}

static int32_t parse_mul(expr_ctx_t *ctx) {
    int32_t val = parse_unary(ctx);
    skip_spaces(ctx);
    while (*ctx->p == '*' || *ctx->p == '/') {
        char op = *ctx->p++;
        int32_t right = parse_unary(ctx);
        if (op == '*') val *= right;
        else if (right != 0) val /= right;
        skip_spaces(ctx);
    }
    return val;
}

static int32_t parse_add(expr_ctx_t *ctx) {
    int32_t val = parse_mul(ctx);
    skip_spaces(ctx);
    while (*ctx->p == '+' || *ctx->p == '-') {
        char op = *ctx->p++;
        int32_t right = parse_mul(ctx);
        if (op == '+') val += right;
        else val -= right;
        skip_spaces(ctx);
    }
    return val;
}

static int32_t parse_expr(expr_ctx_t *ctx) {
    int32_t val = parse_add(ctx);
    skip_spaces(ctx);
    /* Comparison operators for .IF conditionals: =, <>, <, >, <=, >= */
    if (*ctx->p == '=') {
        ctx->p++;
        int32_t right = parse_add(ctx);
        return (val == right) ? 1 : 0;
    }
    if (*ctx->p == '<' && ctx->p[1] == '>') {
        ctx->p += 2;
        int32_t right = parse_add(ctx);
        return (val != right) ? 1 : 0;
    }
    if (*ctx->p == '<' && ctx->p[1] == '=') {
        ctx->p += 2;
        int32_t right = parse_add(ctx);
        return (val <= right) ? 1 : 0;
    }
    if (*ctx->p == '>' && ctx->p[1] == '=') {
        ctx->p += 2;
        int32_t right = parse_add(ctx);
        return (val >= right) ? 1 : 0;
    }
    if (*ctx->p == '<') {
        ctx->p++;
        int32_t right = parse_add(ctx);
        return (val < right) ? 1 : 0;
    }
    if (*ctx->p == '>') {
        ctx->p++;
        int32_t right = parse_add(ctx);
        return (val > right) ? 1 : 0;
    }
    return val;
}

static int32_t eval_expr(asm68k_t *as, const char *str) {
    expr_ctx_t ctx = { str, as };
    return parse_expr(&ctx);
}

/* ========================================================================
 * Operand parsing — 68000 addressing modes
 * ======================================================================== */

typedef enum {
    OP_NONE,
    OP_DATA_REG,       /* Dn */
    OP_ADDR_REG,       /* An */
    OP_ADDR_IND,       /* (An) */
    OP_POST_INC,       /* (An)+ */
    OP_PRE_DEC,        /* -(An) */
    OP_DISP_AN,        /* d(An) or d16(An) */
    OP_INDEX_AN,       /* d(An,Xn) */
    OP_ABS_SHORT,      /* abs.W */
    OP_ABS_LONG,       /* abs.L */
    OP_DISP_PC,        /* d(PC) */
    OP_INDEX_PC,       /* d(PC,Xn) */
    OP_IMMEDIATE,      /* #imm */
    OP_SR,             /* SR */
    OP_CCR,            /* CCR */
    OP_USP,            /* USP */
    OP_REG_LIST,       /* Register list for MOVEM */
} op_type_t;

typedef struct {
    op_type_t type;
    int reg;            /* Register number (0-7) */
    int xreg;           /* Index register number */
    bool xreg_is_addr;  /* Index reg is An (vs Dn) */
    bool xreg_is_long;  /* Index size .L (vs .W) */
    int32_t disp;       /* Displacement value */
    int32_t imm;        /* Immediate value */
    uint16_t regmask;   /* Register mask for MOVEM */
    char expr[256];     /* Raw expression text (for pass 2 resolution) */
} operand_t;

/* Parse register: Dn or An, returns reg number or -1 */
static int parse_data_reg(const char *s) {
    if ((s[0] == 'D' || s[0] == 'd') && s[1] >= '0' && s[1] <= '7' && (s[2] == '\0' || !isalnum((unsigned char)s[2])))
        return s[1] - '0';
    return -1;
}

static int parse_addr_reg(const char *s) {
    if ((s[0] == 'A' || s[0] == 'a') && s[1] >= '0' && s[1] <= '7' && (s[2] == '\0' || !isalnum((unsigned char)s[2])))
        return s[1] - '0';
    /* SP = A7 */
    if ((str_eq_nocase(s, "SP") || str_eq_nocase(s, "A7")) && (s[2] == '\0' || !isalnum((unsigned char)s[2])))
        return 7;
    return -1;
}

/* Parse a register list like D0-D3/A0-A2 for MOVEM */
static uint16_t parse_reglist(const char *s) {
    uint16_t mask = 0;
    const char *p = s;

    while (*p) {
        while (*p == '/' || *p == ' ') p++;
        if (!*p) break;

        int is_addr = 0;
        if (*p == 'D' || *p == 'd') is_addr = 0;
        else if (*p == 'A' || *p == 'a') is_addr = 1;
        else break;
        p++;

        if (!isdigit((unsigned char)*p)) break;
        int first = *p - '0';
        p++;

        if (*p == '-') {
            p++;
            if ((*p == 'D' || *p == 'd' || *p == 'A' || *p == 'a')) p++;
            if (!isdigit((unsigned char)*p)) break;
            int last = *p - '0';
            p++;
            for (int i = first; i <= last; i++) {
                mask |= (1 << (is_addr ? (i + 8) : i));
            }
        } else {
            mask |= (1 << (is_addr ? (first + 8) : first));
        }
    }
    return mask;
}

static bool parse_operand(asm68k_t *as, const char *str, operand_t *op) {
    memset(op, 0, sizeof(operand_t));
    strncpy(op->expr, str, sizeof(op->expr) - 1);

    char buf[256];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    str_trim(buf);

    if (buf[0] == '\0') {
        op->type = OP_NONE;
        return true;
    }

    /* SR, CCR, USP */
    if (str_eq_nocase(buf, "SR")) { op->type = OP_SR; return true; }
    if (str_eq_nocase(buf, "CCR")) { op->type = OP_CCR; return true; }
    if (str_eq_nocase(buf, "USP")) { op->type = OP_USP; return true; }

    /* Immediate: #expr */
    if (buf[0] == '#') {
        op->type = OP_IMMEDIATE;
        op->imm = eval_expr(as, buf + 1);
        return true;
    }

    /* Data register: Dn */
    int r = parse_data_reg(buf);
    if (r >= 0) { op->type = OP_DATA_REG; op->reg = r; return true; }

    /* Address register: An or SP */
    r = parse_addr_reg(buf);
    if (r >= 0) { op->type = OP_ADDR_REG; op->reg = r; return true; }

    /* Pre-decrement: -(An) */
    if (buf[0] == '-' && buf[1] == '(') {
        char *inner = buf + 2;
        char *paren = strchr(inner, ')');
        if (paren) {
            *paren = '\0';
            r = parse_addr_reg(inner);
            if (r >= 0) { op->type = OP_PRE_DEC; op->reg = r; return true; }
        }
    }

    /* (An)+, (An), d(An), d(An,Xn) */
    if (buf[0] == '(') {
        char *inner = buf + 1;
        char *paren = strchr(inner, ')');
        if (paren) {
            char after = paren[1];
            *paren = '\0';

            /* Check for index: (An,Xn) */
            char *comma = strchr(inner, ',');
            if (comma) {
                *comma = '\0';
                char *idx_str = comma + 1;
                str_trim(inner);
                str_trim(idx_str);

                /* Could be (An,Dn.W) or (An,Dn.L) */
                r = parse_addr_reg(inner);
                if (r < 0 && str_eq_nocase(inner, "PC")) r = -2; /* PC */

                /* Parse index register */
                bool is_long = false;
                char *dot = strchr(idx_str, '.');
                if (dot) {
                    is_long = (toupper((unsigned char)dot[1]) == 'L');
                    *dot = '\0';
                }

                int xr = parse_data_reg(idx_str);
                bool xa = false;
                if (xr < 0) { xr = parse_addr_reg(idx_str); xa = true; }

                if (r == -2) {
                    op->type = OP_INDEX_PC;
                } else {
                    op->type = OP_INDEX_AN;
                    op->reg = r;
                }
                op->xreg = xr;
                op->xreg_is_addr = xa;
                op->xreg_is_long = is_long;
                op->disp = 0;
                return true;
            }

            r = parse_addr_reg(inner);
            if (r >= 0) {
                if (after == '+') {
                    op->type = OP_POST_INC;
                    op->reg = r;
                    return true;
                }
                op->type = OP_ADDR_IND;
                op->reg = r;
                return true;
            }
            if (str_eq_nocase(inner, "PC")) {
                op->type = OP_DISP_PC;
                op->disp = 0;
                return true;
            }
        }
    }

    /* d(An) or d(PC) or d(An,Xn) — displacement before parenthesis */
    char *paren = strchr(buf, '(');
    if (paren) {
        char disp_str[128];
        size_t disp_len = paren - buf;
        strncpy(disp_str, buf, disp_len);
        disp_str[disp_len] = '\0';
        str_trim(disp_str);

        int32_t disp = eval_expr(as, disp_str);

        char *inner = paren + 1;
        char *close = strchr(inner, ')');
        if (close) *close = '\0';

        /* Check for index reg */
        char *comma = strchr(inner, ',');
        if (comma) {
            *comma = '\0';
            char *idx_str = comma + 1;
            str_trim(inner);
            str_trim(idx_str);

            bool is_long = false;
            char *dot = strchr(idx_str, '.');
            if (dot) {
                is_long = (toupper((unsigned char)dot[1]) == 'L');
                *dot = '\0';
            }

            int xr = parse_data_reg(idx_str);
            bool xa = false;
            if (xr < 0) { xr = parse_addr_reg(idx_str); xa = true; }

            if (str_eq_nocase(inner, "PC")) {
                op->type = OP_INDEX_PC;
            } else {
                r = parse_addr_reg(inner);
                op->type = OP_INDEX_AN;
                op->reg = r;
            }
            op->xreg = xr;
            op->xreg_is_addr = xa;
            op->xreg_is_long = is_long;
            op->disp = disp;
            return true;
        }

        str_trim(inner);
        if (str_eq_nocase(inner, "PC")) {
            op->type = OP_DISP_PC;
            op->disp = disp;
            return true;
        }
        r = parse_addr_reg(inner);
        if (r >= 0) {
            op->type = OP_DISP_AN;
            op->reg = r;
            op->disp = disp;
            return true;
        }
    }

    /* Register list (for MOVEM) - check if it contains / or - with register names */
    if (strchr(buf, '/') || (strchr(buf, '-') && (buf[0] == 'D' || buf[0] == 'd' || buf[0] == 'A' || buf[0] == 'a'))) {
        op->type = OP_REG_LIST;
        op->regmask = parse_reglist(buf);
        return true;
    }

    /* Absolute address or forward reference */
    op->type = OP_ABS_LONG;
    op->disp = eval_expr(as, buf);

    /* Use short if value fits and we know it */
    if (op->disp >= -32768 && op->disp <= 32767 && as->pass == 2) {
        /* Keep as long for safety unless explicitly .W */
    }

    return true;
}

/* Encode effective address mode/reg fields for instruction */
static void encode_ea(operand_t *op, int *mode, int *reg) {
    switch (op->type) {
        case OP_DATA_REG:  *mode = 0; *reg = op->reg; break;
        case OP_ADDR_REG:  *mode = 1; *reg = op->reg; break;
        case OP_ADDR_IND:  *mode = 2; *reg = op->reg; break;
        case OP_POST_INC:  *mode = 3; *reg = op->reg; break;
        case OP_PRE_DEC:   *mode = 4; *reg = op->reg; break;
        case OP_DISP_AN:   *mode = 5; *reg = op->reg; break;
        case OP_INDEX_AN:  *mode = 6; *reg = op->reg; break;
        case OP_ABS_SHORT: *mode = 7; *reg = 0; break;
        case OP_ABS_LONG:  *mode = 7; *reg = 1; break;
        case OP_DISP_PC:   *mode = 7; *reg = 2; break;
        case OP_INDEX_PC:  *mode = 7; *reg = 3; break;
        case OP_IMMEDIATE: *mode = 7; *reg = 4; break;
        default:           *mode = 0; *reg = 0; break;
    }
}

/* Emit extension words for an effective address */
static void emit_ea_extension(asm68k_t *as, operand_t *op, int size) {
    switch (op->type) {
        case OP_DISP_AN:
        case OP_DISP_PC:
            emit16(as, (uint16_t)(int16_t)op->disp);
            break;
        case OP_INDEX_AN:
        case OP_INDEX_PC: {
            uint16_t ext = ((op->xreg_is_addr ? 1 : 0) << 15) |
                           (op->xreg << 12) |
                           ((op->xreg_is_long ? 1 : 0) << 11) |
                           (op->disp & 0xFF);
            emit16(as, ext);
            break;
        }
        case OP_ABS_SHORT:
            emit16(as, (uint16_t)(int16_t)op->disp);
            break;
        case OP_ABS_LONG:
            emit32(as, (uint32_t)op->disp);
            break;
        case OP_IMMEDIATE:
            if (size == 1) emit16(as, op->imm & 0xFF);
            else if (size == 2) emit16(as, op->imm & 0xFFFF);
            else emit32(as, op->imm);
            break;
        default:
            break;
    }
}

/* Size of EA extension in bytes */
static int __attribute__((unused)) ea_extension_size(operand_t *op, int size) {
    switch (op->type) {
        case OP_DISP_AN: case OP_DISP_PC: case OP_ABS_SHORT:
        case OP_INDEX_AN: case OP_INDEX_PC:
            return 2;
        case OP_ABS_LONG:
            return 4;
        case OP_IMMEDIATE:
            return (size == 4) ? 4 : 2;
        default:
            return 0;
    }
}

/* ========================================================================
 * Instruction size parsing (.B, .W, .L, .S)
 * ======================================================================== */

static int parse_size_suffix(const char *mnemonic, char *base_out) {
    size_t len = strlen(mnemonic);
    if (len >= 3 && mnemonic[len - 2] == '.') {
        strncpy(base_out, mnemonic, len - 2);
        base_out[len - 2] = '\0';
        char sz = toupper((unsigned char)mnemonic[len - 1]);
        switch (sz) {
            case 'B': return 1;
            case 'W': return 2;
            case 'L': return 4;
            case 'S': return 1; /* short branch */
        }
    }
    strcpy(base_out, mnemonic);
    return 2; /* default word */
}

/* ========================================================================
 * Instruction encoding
 * ======================================================================== */

/* Size encoding for most instructions: 00=byte, 01=word, 10=long */
static int size_enc(int size) {
    switch (size) {
        case 1: return 0;
        case 2: return 1;
        case 4: return 2;
    }
    return 1;
}

/* Condition code encoding */
static int parse_cc(const char *cc) {
    static const char *ccs[] = {
        "T","F","HI","LS","CC","CS","NE","EQ",
        "VC","VS","PL","MI","GE","LT","GT","LE"
    };
    /* Also: HS=CC, LO=CS */
    if (str_eq_nocase(cc, "HS")) return 4;
    if (str_eq_nocase(cc, "LO")) return 5;
    for (int i = 0; i < 16; i++) {
        if (str_eq_nocase(cc, ccs[i])) return i;
    }
    return -1;
}

/* Split operands by comma, respecting parentheses */
static void split_operands(const char *ops, char *op1, char *op2) {
    op1[0] = op2[0] = '\0';
    if (!ops || !ops[0]) return;

    int depth = 0;
    const char *p = ops;
    while (*p) {
        if (*p == '(') depth++;
        if (*p == ')') depth--;
        if (*p == ',' && depth == 0) {
            size_t len = p - ops;
            strncpy(op1, ops, len);
            op1[len] = '\0';
            strcpy(op2, p + 1);
            str_trim(op1);
            str_trim(op2);
            return;
        }
        p++;
    }
    strcpy(op1, ops);
    str_trim(op1);
}

/* Assemble one instruction */
static bool assemble_instruction(asm68k_t *as, const char *mnemonic, const char *operands) {
    char base[64];
    int size = parse_size_suffix(mnemonic, base);
    str_upper(base);

    char op1_str[256] = "", op2_str[256] = "";
    split_operands(operands, op1_str, op2_str);

    operand_t op1, op2;
    parse_operand(as, op1_str, &op1);
    parse_operand(as, op2_str, &op2);

    int src_mode, src_reg, dst_mode, dst_reg;

    /* ---- NOP ---- */
    if (strcmp(base, "NOP") == 0) { emit16(as, 0x4E71); return true; }

    /* ---- RTS ---- */
    if (strcmp(base, "RTS") == 0) { emit16(as, 0x4E75); return true; }

    /* ---- RTE ---- */
    if (strcmp(base, "RTE") == 0) { emit16(as, 0x4E73); return true; }

    /* ---- RTR ---- */
    if (strcmp(base, "RTR") == 0) { emit16(as, 0x4E77); return true; }

    /* ---- RESET ---- */
    if (strcmp(base, "RESET") == 0) { emit16(as, 0x4E70); return true; }

    /* ---- TRAPV ---- */
    if (strcmp(base, "TRAPV") == 0) { emit16(as, 0x4E76); return true; }

    /* ---- ILLEGAL ---- */
    if (strcmp(base, "ILLEGAL") == 0) { emit16(as, 0x4AFC); return true; }

    /* ---- STOP #imm ---- */
    if (strcmp(base, "STOP") == 0) {
        emit16(as, 0x4E72);
        emit16(as, (uint16_t)op1.imm);
        return true;
    }

    /* ---- TRAP #vector ---- */
    if (strcmp(base, "TRAP") == 0) {
        emit16(as, 0x4E40 | (op1.imm & 0xF));
        return true;
    }

    /* ---- LINK An,#disp ---- */
    if (strcmp(base, "LINK") == 0) {
        emit16(as, 0x4E50 | op1.reg);
        emit16(as, (uint16_t)(int16_t)op2.imm);
        return true;
    }

    /* ---- UNLK An ---- */
    if (strcmp(base, "UNLK") == 0) {
        emit16(as, 0x4E58 | op1.reg);
        return true;
    }

    /* ---- MOVE ---- */
    if (strcmp(base, "MOVE") == 0) {
        /* MOVE to/from SR, CCR, USP */
        if (op1.type == OP_SR) {
            encode_ea(&op2, &dst_mode, &dst_reg);
            emit16(as, 0x40C0 | (dst_mode << 3) | dst_reg);
            emit_ea_extension(as, &op2, 2);
            return true;
        }
        if (op2.type == OP_SR) {
            encode_ea(&op1, &src_mode, &src_reg);
            emit16(as, 0x46C0 | (src_mode << 3) | src_reg);
            emit_ea_extension(as, &op1, 2);
            return true;
        }
        if (op2.type == OP_CCR) {
            encode_ea(&op1, &src_mode, &src_reg);
            emit16(as, 0x44C0 | (src_mode << 3) | src_reg);
            emit_ea_extension(as, &op1, 2);
            return true;
        }
        if (op1.type == OP_USP) {
            emit16(as, 0x4E68 | op2.reg);
            return true;
        }
        if (op2.type == OP_USP) {
            emit16(as, 0x4E60 | op1.reg);
            return true;
        }

        /* Regular MOVE */
        encode_ea(&op1, &src_mode, &src_reg);
        encode_ea(&op2, &dst_mode, &dst_reg);
        int sz_enc;
        switch (size) {
            case 1: sz_enc = 1; break;
            case 2: sz_enc = 3; break;
            case 4: sz_enc = 2; break;
            default: sz_enc = 3; break;
        }
        emit16(as, (sz_enc << 12) | (dst_reg << 9) | (dst_mode << 6) | (src_mode << 3) | src_reg);
        emit_ea_extension(as, &op1, size);
        emit_ea_extension(as, &op2, size);
        return true;
    }

    /* ---- ADDA, SUBA, CMPA ---- */
    if (strcmp(base, "ADDA") == 0 || strcmp(base, "SUBA") == 0 || strcmp(base, "CMPA") == 0) {
        encode_ea(&op1, &src_mode, &src_reg);
        int base_op;
        if (strcmp(base, "ADDA") == 0) base_op = 0xD000;
        else if (strcmp(base, "SUBA") == 0) base_op = 0x9000;
        else base_op = 0xB000;
        int sz = (size == 4) ? 7 : 3;
        emit16(as, base_op | (op2.reg << 9) | (sz << 6) | (src_mode << 3) | src_reg);
        emit_ea_extension(as, &op1, size);
        return true;
    }

    /* ---- MOVEA ---- */
    if (strcmp(base, "MOVEA") == 0) {
        encode_ea(&op1, &src_mode, &src_reg);
        int sz_enc = (size == 4) ? 2 : 3;
        emit16(as, (sz_enc << 12) | (op2.reg << 9) | (1 << 6) | (src_mode << 3) | src_reg);
        emit_ea_extension(as, &op1, size);
        return true;
    }

    /* ---- MOVEQ ---- */
    if (strcmp(base, "MOVEQ") == 0) {
        emit16(as, 0x7000 | (op2.reg << 9) | (op1.imm & 0xFF));
        return true;
    }

    /* ---- MOVEM ---- */
    if (strcmp(base, "MOVEM") == 0) {
        int sz = (size == 4) ? 1 : 0;
        if (op1.type == OP_REG_LIST) {
            /* Register to memory */
            encode_ea(&op2, &dst_mode, &dst_reg);
            emit16(as, 0x4880 | (sz << 6) | (dst_mode << 3) | dst_reg);
            uint16_t mask = op1.regmask;
            if (op2.type == OP_PRE_DEC) {
                /* Reverse bit order for predecrement */
                uint16_t rev = 0;
                for (int i = 0; i < 16; i++)
                    if (mask & (1 << i)) rev |= (1 << (15 - i));
                mask = rev;
            }
            emit16(as, mask);
            emit_ea_extension(as, &op2, size);
        } else {
            /* Memory to register */
            encode_ea(&op1, &src_mode, &src_reg);
            emit16(as, 0x4C80 | (sz << 6) | (src_mode << 3) | src_reg);
            emit16(as, op2.regmask);
            emit_ea_extension(as, &op1, size);
        }
        return true;
    }

    /* ---- LEA ea,An ---- */
    if (strcmp(base, "LEA") == 0) {
        encode_ea(&op1, &src_mode, &src_reg);
        emit16(as, 0x41C0 | (op2.reg << 9) | (src_mode << 3) | src_reg);
        emit_ea_extension(as, &op1, 4);
        return true;
    }

    /* ---- PEA ea ---- */
    if (strcmp(base, "PEA") == 0) {
        encode_ea(&op1, &src_mode, &src_reg);
        emit16(as, 0x4840 | (src_mode << 3) | src_reg);
        emit_ea_extension(as, &op1, 4);
        return true;
    }

    /* ---- CLR ---- */
    if (strcmp(base, "CLR") == 0) {
        encode_ea(&op1, &dst_mode, &dst_reg);
        emit16(as, 0x4200 | (size_enc(size) << 6) | (dst_mode << 3) | dst_reg);
        emit_ea_extension(as, &op1, size);
        return true;
    }

    /* ---- TST ---- */
    if (strcmp(base, "TST") == 0) {
        encode_ea(&op1, &dst_mode, &dst_reg);
        emit16(as, 0x4A00 | (size_enc(size) << 6) | (dst_mode << 3) | dst_reg);
        emit_ea_extension(as, &op1, size);
        return true;
    }

    /* ---- NEG, NEGX, NOT ---- */
    if (strcmp(base, "NEG") == 0 || strcmp(base, "NEGX") == 0 || strcmp(base, "NOT") == 0) {
        int op_base;
        if (strcmp(base, "NEGX") == 0) op_base = 0x4000;
        else if (strcmp(base, "NEG") == 0) op_base = 0x4400;
        else op_base = 0x4600;
        encode_ea(&op1, &dst_mode, &dst_reg);
        emit16(as, op_base | (size_enc(size) << 6) | (dst_mode << 3) | dst_reg);
        emit_ea_extension(as, &op1, size);
        return true;
    }

    /* ---- EXT ---- */
    if (strcmp(base, "EXT") == 0) {
        int opcode = (size == 4) ? 0x48C0 : 0x4880;
        emit16(as, opcode | op1.reg);
        return true;
    }

    /* ---- SWAP ---- */
    if (strcmp(base, "SWAP") == 0) {
        emit16(as, 0x4840 | op1.reg);
        return true;
    }

    /* ---- EXG ---- */
    if (strcmp(base, "EXG") == 0) {
        int opmode;
        if (op1.type == OP_DATA_REG && op2.type == OP_DATA_REG) opmode = 0x08;
        else if (op1.type == OP_ADDR_REG && op2.type == OP_ADDR_REG) opmode = 0x09;
        else opmode = 0x11;
        int rx = (op1.type == OP_DATA_REG || (op1.type == OP_ADDR_REG && opmode != 0x11)) ? op1.reg : op1.reg;
        int ry = op2.reg;
        emit16(as, 0xC100 | (rx << 9) | (opmode << 3) | ry);
        return true;
    }

    /* ---- ADD, SUB, AND, OR, CMP, EOR ---- */
    {
        static const struct { const char *name; int base_op; } alu_ops[] = {
            {"ADD", 0xD000}, {"SUB", 0x9000}, {"CMP", 0xB000},
            {"AND", 0xC000}, {"OR",  0x8000},
            {NULL, 0}
        };
        for (int i = 0; alu_ops[i].name; i++) {
            if (strcmp(base, alu_ops[i].name) != 0) continue;

            /* Determine direction */
            int dn_reg;
            bool to_ea;
            if (op2.type == OP_DATA_REG) {
                dn_reg = op2.reg;
                to_ea = false; /* <ea> op Dn -> Dn */
            } else {
                dn_reg = op1.reg;
                to_ea = true;  /* Dn op <ea> -> <ea> */
            }

            operand_t *ea_op = to_ea ? &op2 : &op1;
            encode_ea(ea_op, &src_mode, &src_reg);

            int dir = to_ea ? 1 : 0;
            int sz = size_enc(size);

            /* ADDA/SUBA/CMPA */
            if (op2.type == OP_ADDR_REG) {
                sz = (size == 4) ? 7 : 3;
                encode_ea(&op1, &src_mode, &src_reg);
                emit16(as, alu_ops[i].base_op | (op2.reg << 9) | (sz << 6) | (src_mode << 3) | src_reg);
                emit_ea_extension(as, &op1, size);
                return true;
            }

            emit16(as, alu_ops[i].base_op | (dn_reg << 9) | (dir << 8) | (sz << 6) | (src_mode << 3) | src_reg);
            emit_ea_extension(as, ea_op, size);
            return true;
        }

        if (strcmp(base, "EOR") == 0) {
            encode_ea(&op2, &dst_mode, &dst_reg);
            emit16(as, 0xB100 | (op1.reg << 9) | (size_enc(size) << 6) | (dst_mode << 3) | dst_reg);
            emit_ea_extension(as, &op2, size);
            return true;
        }
    }

    /* ---- ADDI, SUBI, CMPI, ANDI, ORI, EORI ---- */
    {
        static const struct { const char *name; int opcode; } imm_ops[] = {
            {"ORI",  0x0000}, {"ANDI", 0x0200}, {"SUBI", 0x0400},
            {"ADDI", 0x0600}, {"EORI", 0x0A00}, {"CMPI", 0x0C00},
            {NULL, 0}
        };
        for (int i = 0; imm_ops[i].name; i++) {
            if (strcmp(base, imm_ops[i].name) != 0) continue;
            encode_ea(&op2, &dst_mode, &dst_reg);
            emit16(as, imm_ops[i].opcode | (size_enc(size) << 6) | (dst_mode << 3) | dst_reg);
            if (size == 1) emit16(as, op1.imm & 0xFF);
            else if (size == 2) emit16(as, op1.imm & 0xFFFF);
            else emit32(as, op1.imm);
            emit_ea_extension(as, &op2, size);
            return true;
        }
    }

    /* ---- ADDQ, SUBQ ---- */
    if (strcmp(base, "ADDQ") == 0 || strcmp(base, "SUBQ") == 0) {
        int data = op1.imm & 7;
        if (op1.imm == 8) data = 0;
        int sub = (strcmp(base, "SUBQ") == 0) ? 1 : 0;
        encode_ea(&op2, &dst_mode, &dst_reg);
        emit16(as, 0x5000 | (data << 9) | (sub << 8) | (size_enc(size) << 6) | (dst_mode << 3) | dst_reg);
        emit_ea_extension(as, &op2, size);
        return true;
    }

    /* ---- CMPM ---- */
    if (strcmp(base, "CMPM") == 0) {
        /* CMPM (Ay)+,(Ax)+  opcode: 1011 Ax 1 ss 001 Ay */
        int ax = op2.reg;
        int ay = op1.reg;
        emit16(as, 0xB108 | (ax << 9) | (size_enc(size) << 6) | ay);
        return true;
    }

    /* ---- ADDX, SUBX ---- */
    if (strcmp(base, "ADDX") == 0 || strcmp(base, "SUBX") == 0) {
        int sub = (strcmp(base, "SUBX") == 0) ? 1 : 0;
        int rm = (op1.type == OP_PRE_DEC) ? 1 : 0;
        int rx = op2.reg;
        int ry = op1.reg;
        /* ADDX: 1101 Rx 1 ss 00 rm Ry  SUBX: 1001 Rx 1 ss 00 rm Ry */
        uint16_t base_op = sub ? 0x9100 : 0xD100;
        emit16(as, base_op | (rx << 9) | (size_enc(size) << 6) | (rm << 3) | ry);
        return true;
    }

    /* ---- MULU, MULS, DIVU, DIVS ---- */
    if (strcmp(base, "MULU") == 0 || strcmp(base, "MULS") == 0 ||
        strcmp(base, "DIVU") == 0 || strcmp(base, "DIVS") == 0) {
        encode_ea(&op1, &src_mode, &src_reg);
        int base_op;
        if (strcmp(base, "MULU") == 0) base_op = 0xC0C0;
        else if (strcmp(base, "MULS") == 0) base_op = 0xC1C0;
        else if (strcmp(base, "DIVU") == 0) base_op = 0x80C0;
        else base_op = 0x81C0;
        emit16(as, base_op | (op2.reg << 9) | (src_mode << 3) | src_reg);
        emit_ea_extension(as, &op1, 2);
        return true;
    }

    /* ---- Shifts: ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR ---- */
    {
        static const struct { const char *name; int type; int dir; } shifts[] = {
            {"ASR", 0, 0}, {"ASL", 0, 1}, {"LSR", 1, 0}, {"LSL", 1, 1},
            {"ROXR", 2, 0}, {"ROXL", 2, 1}, {"ROR", 3, 0}, {"ROL", 3, 1},
            {NULL, 0, 0}
        };
        for (int i = 0; shifts[i].name; i++) {
            if (strcmp(base, shifts[i].name) != 0) continue;

            if (op2.type == OP_NONE || op2.type == OP_DATA_REG || op2.type == OP_ADDR_IND ||
                op2.type == OP_POST_INC || op2.type == OP_PRE_DEC || op2.type == OP_DISP_AN ||
                op2.type == OP_ABS_SHORT || op2.type == OP_ABS_LONG) {
                if (op2.type == OP_NONE) {
                    /* Memory shift: op ea */
                    encode_ea(&op1, &dst_mode, &dst_reg);
                    emit16(as, 0xE0C0 | (shifts[i].type << 9) | (shifts[i].dir << 8) | (dst_mode << 3) | dst_reg);
                    emit_ea_extension(as, &op1, 2);
                } else {
                    /* Register shift */
                    int count;
                    int ir;
                    if (op1.type == OP_IMMEDIATE) {
                        count = op1.imm & 7;
                        if (op1.imm == 8) count = 0;
                        ir = 0;
                    } else {
                        count = op1.reg;
                        ir = 1;
                    }
                    emit16(as, 0xE000 | (count << 9) | (shifts[i].dir << 8) | (size_enc(size) << 6) | (ir << 5) | (shifts[i].type << 3) | op2.reg);
                }
                return true;
            }
        }
    }

    /* ---- BTST, BCHG, BCLR, BSET ---- */
    {
        static const struct { const char *name; int type; } bits[] = {
            {"BTST", 0}, {"BCHG", 1}, {"BCLR", 2}, {"BSET", 3}, {NULL, 0}
        };
        for (int i = 0; bits[i].name; i++) {
            if (strcmp(base, bits[i].name) != 0) continue;
            encode_ea(&op2, &dst_mode, &dst_reg);
            if (op1.type == OP_IMMEDIATE) {
                /* Static */
                emit16(as, 0x0800 | (bits[i].type << 6) | (dst_mode << 3) | dst_reg);
                emit16(as, op1.imm & 0xFF);
            } else {
                /* Dynamic */
                emit16(as, 0x0100 | (op1.reg << 9) | (bits[i].type << 6) | (dst_mode << 3) | dst_reg);
            }
            emit_ea_extension(as, &op2, 1);
            return true;
        }
    }

    /* ---- Bcc, BRA, BSR ---- */
    if (base[0] == 'B' && strlen(base) >= 2) {
        int cc = -1;
        if (strcmp(base, "BRA") == 0) cc = 0;
        else if (strcmp(base, "BSR") == 0) cc = 1;
        else cc = parse_cc(base + 1);

        if (cc >= 0) {
            int32_t target = op1.disp;
            if (op1.type == OP_ABS_LONG) target = op1.disp;
            int32_t disp = target - (as->pc + 2);

            /* Check for .S suffix (byte displacement) */
            bool force_short = (mnemonic[strlen(mnemonic) - 1] == 'S' || mnemonic[strlen(mnemonic) - 1] == 's')
                               && mnemonic[strlen(mnemonic) - 2] == '.';

            if (force_short || (disp >= -128 && disp <= 127 && disp != 0)) {
                emit16(as, 0x6000 | (cc << 8) | (disp & 0xFF));
            } else {
                emit16(as, 0x6000 | (cc << 8));
                emit16(as, (uint16_t)(int16_t)disp);
            }
            return true;
        }
    }

    /* ---- DBcc ---- */
    if (base[0] == 'D' && base[1] == 'B') {
        int cc = parse_cc(base + 2);
        if (cc < 0 && str_eq_nocase(base + 2, "RA")) cc = 0;
        if (cc < 0 && str_eq_nocase(base + 2, "F")) cc = 1;
        if (cc >= 0) {
            int32_t target = op2.disp;
            int32_t disp = target - (as->pc + 2);
            emit16(as, 0x50C8 | (cc << 8) | op1.reg);
            emit16(as, (uint16_t)(int16_t)disp);
            return true;
        }
    }

    /* ---- Scc ---- */
    if (base[0] == 'S' && strlen(base) >= 2) {
        int cc = parse_cc(base + 1);
        if (cc >= 0) {
            encode_ea(&op1, &dst_mode, &dst_reg);
            emit16(as, 0x50C0 | (cc << 8) | (dst_mode << 3) | dst_reg);
            emit_ea_extension(as, &op1, 1);
            return true;
        }
    }

    /* ---- JMP, JSR ---- */
    if (strcmp(base, "JMP") == 0) {
        encode_ea(&op1, &dst_mode, &dst_reg);
        emit16(as, 0x4EC0 | (dst_mode << 3) | dst_reg);
        emit_ea_extension(as, &op1, 4);
        return true;
    }
    if (strcmp(base, "JSR") == 0) {
        encode_ea(&op1, &dst_mode, &dst_reg);
        emit16(as, 0x4E80 | (dst_mode << 3) | dst_reg);
        emit_ea_extension(as, &op1, 4);
        return true;
    }

    /* ---- CHK ---- */
    if (strcmp(base, "CHK") == 0) {
        encode_ea(&op1, &src_mode, &src_reg);
        emit16(as, 0x4180 | (op2.reg << 9) | (src_mode << 3) | src_reg);
        emit_ea_extension(as, &op1, 2);
        return true;
    }

    /* ---- TAS ---- */
    if (strcmp(base, "TAS") == 0) {
        encode_ea(&op1, &dst_mode, &dst_reg);
        emit16(as, 0x4AC0 | (dst_mode << 3) | dst_reg);
        emit_ea_extension(as, &op1, 1);
        return true;
    }

    /* ---- NBCD, ABCD, SBCD ---- */
    if (strcmp(base, "NBCD") == 0) {
        encode_ea(&op1, &dst_mode, &dst_reg);
        emit16(as, 0x4800 | (dst_mode << 3) | dst_reg);
        emit_ea_extension(as, &op1, 1);
        return true;
    }

    /* ---- MOVEP ---- */
    if (strcmp(base, "MOVEP") == 0) {
        int opmode;
        if (op1.type == OP_DATA_REG) {
            /* Dn -> d(An): register to memory */
            opmode = (size == 4) ? 7 : 6;
            emit16(as, 0x0008 | (op1.reg << 9) | (opmode << 6) | op2.reg);
            emit16(as, (uint16_t)(int16_t)op2.disp);
        } else {
            /* d(An) -> Dn: memory to register */
            opmode = (size == 4) ? 5 : 4;
            emit16(as, 0x0008 | (op2.reg << 9) | (opmode << 6) | op1.reg);
            emit16(as, (uint16_t)(int16_t)op1.disp);
        }
        return true;
    }

    /* If we failed with size suffix stripped, try with the original full mnemonic */
    if (strcmp(base, mnemonic) != 0) {
        /* Try again treating the whole thing as the base (e.g., MOVEQ, DBRA) */
        char orig[64];
        strncpy(orig, mnemonic, sizeof(orig) - 1);
        str_upper(orig);

        /* MOVEQ */
        if (strcmp(orig, "MOVEQ") == 0) {
            emit16(as, 0x7000 | (op2.reg << 9) | (op1.imm & 0xFF));
            return true;
        }

        /* DBRA = DBF */
        if (strcmp(orig, "DBRA") == 0) {
            int32_t target = op2.disp;
            if (op2.type == OP_ABS_LONG) target = op2.disp;
            int32_t disp = target - (as->pc + 2);
            emit16(as, 0x51C8 | op1.reg);
            emit16(as, (uint16_t)(int16_t)disp);
            return true;
        }
    }

    asm_error(as, "unknown instruction: %s", mnemonic);
    return false;
}

/* ========================================================================
 * Directive handling
 * ======================================================================== */

/* Forward declaration — needed for .INCLUDE processing */
static void assemble_line(asm68k_t *as, const char *raw_line);

static bool handle_directive(asm68k_t *as, const char *directive, const char *args, const char *label) {
    char dir[64];
    strncpy(dir, directive, sizeof(dir) - 1);
    str_upper(dir);

    /* .PROC name[,stacksize] */
    if (strcmp(dir, ".PROC") == 0) {
        char name[ASM_MAX_LABEL];
        strncpy(name, args, ASM_MAX_LABEL - 1);
        /* Strip comma and anything after */
        char *comma = strchr(name, ',');
        if (comma) *comma = '\0';
        str_trim(name);
        if (name[0]) {
            add_symbol(as, name, SYM_PROC, as->pc);
        }
        return true;
    }

    /* .FUNC name[,stacksize] */
    if (strcmp(dir, ".FUNC") == 0) {
        char name[ASM_MAX_LABEL];
        strncpy(name, args, ASM_MAX_LABEL - 1);
        char *comma = strchr(name, ',');
        if (comma) *comma = '\0';
        str_trim(name);
        if (name[0]) {
            add_symbol(as, name, SYM_FUNC, as->pc);
        }
        return true;
    }

    /* .DEF name[,name,...] */
    if (strcmp(dir, ".DEF") == 0) {
        char buf[512];
        strncpy(buf, args, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *tok = strtok(buf, ",");
        while (tok) {
            while (*tok == ' ' || *tok == '\t') tok++;
            char name[ASM_MAX_LABEL];
            strncpy(name, tok, ASM_MAX_LABEL - 1);
            name[ASM_MAX_LABEL - 1] = '\0';
            str_trim(name);
            if (name[0]) {
                int idx = find_symbol(as, name);
                if (idx >= 0) {
                    as->symbols[idx].exported = true;
                } else {
                    add_symbol(as, name, SYM_DEF, 0);
                }
            }
            tok = strtok(NULL, ",");
        }
        return true;
    }

    /* .REF name[,name,...] */
    if (strcmp(dir, ".REF") == 0) {
        char buf[512];
        strncpy(buf, args, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *tok = strtok(buf, ",");
        while (tok) {
            while (*tok == ' ' || *tok == '\t') tok++;
            char name[ASM_MAX_LABEL];
            strncpy(name, tok, ASM_MAX_LABEL - 1);
            name[ASM_MAX_LABEL - 1] = '\0';
            str_trim(name);
            if (name[0]) {
                int idx = find_symbol(as, name);
                if (idx < 0) {
                    add_symbol(as, name, SYM_REF, 0);
                }
            }
            tok = strtok(NULL, ",");
        }
        return true;
    }

    /* .EQU or just EQU: label .EQU value */
    if (strcmp(dir, ".EQU") == 0 || strcmp(dir, "EQU") == 0) {
        if (label[0]) {
            int32_t val = eval_expr(as, args);
            add_symbol(as, label, SYM_EQU, val);
        }
        return true;
    }

    /* .INCLUDE filename */
    if (strcmp(dir, ".INCLUDE") == 0) {
        char inc_name[256];
        strncpy(inc_name, args, sizeof(inc_name) - 1);
        inc_name[sizeof(inc_name) - 1] = '\0';
        /* Strip trailing comment (everything after ;) */
        char *semi = strchr(inc_name, ';');
        if (semi) *semi = '\0';
        str_trim(inc_name);

        /* Resolve Lisa-style include path to archive path.
         * Lisa paths like "source/pascaldefs.text" map to archive names like
         * "source-PASCALDEFS.TEXT.unix.txt" in the same directory.
         * The convention: replace / with - and append .unix.txt */
        char resolved[512];
        bool found = false;

        /* Strategy 1: direct path under base_dir */
        snprintf(resolved, sizeof(resolved), "%s/%s", as->base_dir, inc_name);
        FILE *f = fopen(resolved, "r");
        if (f) { found = true; }

        /* Strategy 2: direct path + .unix.txt */
        if (!f) {
            snprintf(resolved, sizeof(resolved), "%s/%s.unix.txt", as->base_dir, inc_name);
            f = fopen(resolved, "r");
            if (f) found = true;
        }

        /* Strategy 3: convert Lisa path: source/file.text → source-FILE.TEXT.unix.txt */
        if (!f) {
            char mapped[256];
            strncpy(mapped, inc_name, sizeof(mapped) - 1);
            mapped[sizeof(mapped) - 1] = '\0';
            /* Replace / with - */
            for (char *c = mapped; *c; c++) {
                if (*c == '/') *c = '-';
            }
            /* Try exact case with .unix.txt */
            snprintf(resolved, sizeof(resolved), "%s/%s.unix.txt", as->base_dir, mapped);
            f = fopen(resolved, "r");
            if (f) found = true;
        }

        /* Strategy 4: uppercase the mapped name */
        if (!f) {
            char mapped[256];
            strncpy(mapped, inc_name, sizeof(mapped) - 1);
            mapped[sizeof(mapped) - 1] = '\0';
            for (char *c = mapped; *c; c++) {
                if (*c == '/') *c = '-';
                else *c = toupper((unsigned char)*c);
            }
            snprintf(resolved, sizeof(resolved), "%s/%s.unix.txt", as->base_dir, mapped);
            f = fopen(resolved, "r");
            if (f) found = true;
        }

        /* Strategy 5a: infer library prefix from base_dir (e.g. in LIBPL dir, paslibequs.text → libpl-PASLIBEQUS.TEXT.unix.txt) */
        if (!f) {
            /* Extract last directory component to use as prefix */
            const char *dir_end = as->base_dir + strlen(as->base_dir);
            const char *dir_start = dir_end - 1;
            while (dir_start > as->base_dir && *dir_start != '/') dir_start--;
            if (*dir_start == '/') dir_start++;
            char dir_prefix[64];
            size_t plen = dir_end - dir_start;
            if (plen < sizeof(dir_prefix)) {
                strncpy(dir_prefix, dir_start, plen);
                dir_prefix[plen] = '\0';
                char lower_pref[64];
                strncpy(lower_pref, dir_prefix, sizeof(lower_pref));
                for (char *c = lower_pref; *c; c++) *c = tolower((unsigned char)*c);

                /* Strip .text suffix from inc_name if present */
                char base_name[128];
                strncpy(base_name, inc_name, sizeof(base_name) - 1);
                base_name[sizeof(base_name) - 1] = '\0';
                char *dot = strcasestr(base_name, ".text");
                if (dot) *dot = '\0';

                const char *sfx5[] = { ".TEXT.unix.txt", ".text.unix.txt", NULL };
                for (int si = 0; sfx5[si] && !f; si++) {
                    /* Try: basedir/prefix-FILENAME.TEXT.unix.txt */
                    char upper_name[128];
                    strncpy(upper_name, base_name, sizeof(upper_name));
                    for (char *c = upper_name; *c; c++) *c = toupper((unsigned char)*c);
                    snprintf(resolved, sizeof(resolved), "%s/%s-%s%s",
                             as->base_dir, lower_pref, upper_name, sfx5[si]);
                    f = fopen(resolved, "r");
                    if (!f) {
                        /* Try original case */
                        snprintf(resolved, sizeof(resolved), "%s/%s-%s%s",
                                 as->base_dir, lower_pref, base_name, sfx5[si]);
                        f = fopen(resolved, "r");
                    }
                }
                if (f) found = true;
            }
        }

        /* Strategy 5b: same-directory self-reference (e.g. libfp/sanemacs from within LIBFP dir) */
        if (!f) {
            char *sl = strchr(inc_name, '/');
            if (sl) {
                char prefix[64], filename[128];
                size_t dlen = sl - inc_name;
                if (dlen < sizeof(prefix)) {
                    strncpy(prefix, inc_name, dlen);
                    prefix[dlen] = '\0';
                    strncpy(filename, sl + 1, sizeof(filename) - 1);
                    filename[sizeof(filename) - 1] = '\0';
                    /* Lowercase prefix for filename convention */
                    char lower_prefix[64];
                    strncpy(lower_prefix, prefix, sizeof(lower_prefix));
                    for (char *c = lower_prefix; *c; c++) *c = tolower((unsigned char)*c);

                    /* Try: base_dir/prefix-filename.text.unix.txt (various cases) */
                    const char *suffixes[] = {
                        ".text.unix.txt", ".TEXT.unix.txt", ".unix.txt", NULL
                    };
                    for (int si = 0; suffixes[si] && !f; si++) {
                        /* lowercase prefix - original case filename */
                        snprintf(resolved, sizeof(resolved), "%s/%s-%s%s",
                                 as->base_dir, lower_prefix, filename, suffixes[si]);
                        f = fopen(resolved, "r");
                        if (!f) {
                            /* lowercase prefix - UPPERCASE filename */
                            char upper_file[128];
                            strncpy(upper_file, filename, sizeof(upper_file));
                            for (char *c = upper_file; *c; c++) *c = toupper((unsigned char)*c);
                            snprintf(resolved, sizeof(resolved), "%s/%s-%s%s",
                                     as->base_dir, lower_prefix, upper_file, suffixes[si]);
                            f = fopen(resolved, "r");
                        }
                    }
                    if (f) found = true;
                }
            }
        }

        /* Strategy 6: try include paths with library convention */
        if (!f) {
            for (int i = 0; i < as->num_include_paths && !f; i++) {
                /* Direct */
                snprintf(resolved, sizeof(resolved), "%s/%s", as->include_paths[i], inc_name);
                f = fopen(resolved, "r");
                if (!f) {
                    snprintf(resolved, sizeof(resolved), "%s/%s.unix.txt", as->include_paths[i], inc_name);
                    f = fopen(resolved, "r");
                }
                /* Lisa library convention: LibXX/file → LIBXX/libxx-file.text.unix.txt */
                if (!f) {
                    char *sl2 = strchr(inc_name, '/');
                    if (sl2) {
                        char libdir[64], filename2[128];
                        size_t dlen2 = sl2 - inc_name;
                        if (dlen2 < sizeof(libdir)) {
                            strncpy(libdir, inc_name, dlen2);
                            libdir[dlen2] = '\0';
                            strncpy(filename2, sl2 + 1, sizeof(filename2) - 1);
                            char upper_dir[64], lower_dir[64];
                            strncpy(upper_dir, libdir, sizeof(upper_dir));
                            for (char *c = upper_dir; *c; c++) *c = toupper((unsigned char)*c);
                            strncpy(lower_dir, libdir, sizeof(lower_dir));
                            for (char *c = lower_dir; *c; c++) *c = tolower((unsigned char)*c);

                            const char *sfx[] = { ".text.unix.txt", ".TEXT.unix.txt", ".unix.txt", NULL };
                            for (int si = 0; sfx[si] && !f; si++) {
                                snprintf(resolved, sizeof(resolved), "%s/%s/%s-%s%s",
                                         as->include_paths[i], upper_dir, lower_dir, filename2, sfx[si]);
                                f = fopen(resolved, "r");
                                if (!f) {
                                    char upper_file[128];
                                    strncpy(upper_file, filename2, sizeof(upper_file));
                                    for (char *c = upper_file; *c; c++) *c = toupper((unsigned char)*c);
                                    snprintf(resolved, sizeof(resolved), "%s/%s/%s-%s%s",
                                             as->include_paths[i], upper_dir, lower_dir, upper_file, sfx[si]);
                                    f = fopen(resolved, "r");
                                }
                            }
                        }
                    }
                }
                if (f) found = true;
            }
        }

        if (found && f) {
            /* Read and assemble the included file */
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *inc_source = malloc(size + 1);
            if (inc_source) {
                fread(inc_source, 1, size, f);
                inc_source[size] = '\0';
                fclose(f);

                /* Save current state */
                char saved_file[256];
                int saved_line = as->line_num;
                strncpy(saved_file, as->current_file, sizeof(saved_file) - 1);
                strncpy(as->current_file, resolved, sizeof(as->current_file) - 1);

                /* Assemble included content line by line */
                const char *p = inc_source;
                char line[ASM_MAX_LINE];
                as->line_num = 0;
                while (*p) {
                    as->line_num++;
                    int li = 0;
                    while (*p && *p != '\n' && *p != '\r' && li < ASM_MAX_LINE - 1) {
                        line[li++] = *p++;
                    }
                    line[li] = '\0';
                    if (*p == '\r') p++;
                    if (*p == '\n') p++;
                    assemble_line(as, line);
                }

                /* Restore state */
                strncpy(as->current_file, saved_file, sizeof(as->current_file) - 1);
                as->line_num = saved_line;
                free(inc_source);
            } else {
                fclose(f);
            }
        } else {
            if (f) fclose(f);
            if (as->pass == 2) {
                asm_error(as, "cannot resolve .INCLUDE '%s'", inc_name);
            }
        }
        return true;
    }

    /* .IF / .ENDC / .ELSE */
    if (strcmp(dir, ".IF") == 0) {
        int32_t val = eval_expr(as, args);
        if (as->cond_depth < ASM_MAX_COND_DEPTH) {
            as->cond_stack[as->cond_depth].active = (val != 0);
            as->cond_stack[as->cond_depth].had_true = (val != 0);
            as->cond_depth++;
        }
        return true;
    }
    if (strcmp(dir, ".ELSE") == 0) {
        if (as->cond_depth > 0) {
            asm_cond_t *c = &as->cond_stack[as->cond_depth - 1];
            c->active = !c->had_true;
        }
        return true;
    }
    if (strcmp(dir, ".ENDC") == 0) {
        if (as->cond_depth > 0) as->cond_depth--;
        return true;
    }

    /* .MACRO / .ENDM */
    if (strcmp(dir, ".MACRO") == 0) {
        if (as->num_macros < ASM_MAX_MACROS) {
            as->in_macro_def = true;
            as->current_macro = as->num_macros++;
            /* Macro name can be in label field (name .MACRO) or args field (.MACRO name) */
            const char *mname = (label[0]) ? label : args;
            char macro_name[ASM_MAX_LABEL];
            strncpy(macro_name, mname, ASM_MAX_LABEL - 1);
            macro_name[ASM_MAX_LABEL - 1] = '\0';
            str_trim(macro_name);
            /* Strip any parameter list after the name */
            char *space = strchr(macro_name, ' ');
            if (space) *space = '\0';
            space = strchr(macro_name, '\t');
            if (space) *space = '\0';
            strncpy(as->macros[as->current_macro].name, macro_name, ASM_MAX_LABEL - 1);
            as->macros[as->current_macro].line_count = 0;
        }
        return true;
    }
    if (strcmp(dir, ".ENDM") == 0) {
        as->in_macro_def = false;
        return true;
    }

    /* Data directives */
    if (strcmp(dir, ".BYTE") == 0 || strcmp(dir, "DC.B") == 0) {
        /* Parse comma-separated byte values */
        char buf[256];
        strncpy(buf, args, sizeof(buf) - 1);
        char *tok = strtok(buf, ",");
        while (tok) {
            str_trim(tok);
            if (tok[0] == '\'' || tok[0] == '"') {
                /* String literal */
                char *p = tok + 1;
                while (*p && *p != tok[0]) { emit8(as, *p++); }
            } else {
                emit8(as, (uint8_t)eval_expr(as, tok));
            }
            tok = strtok(NULL, ",");
        }
        return true;
    }

    if (strcmp(dir, ".WORD") == 0 || strcmp(dir, "DC.W") == 0) {
        char buf[256];
        strncpy(buf, args, sizeof(buf) - 1);
        char *tok = strtok(buf, ",");
        while (tok) {
            str_trim(tok);
            emit16(as, (uint16_t)eval_expr(as, tok));
            tok = strtok(NULL, ",");
        }
        return true;
    }

    if (strcmp(dir, ".LONG") == 0 || strcmp(dir, "DC.L") == 0) {
        char buf[256];
        strncpy(buf, args, sizeof(buf) - 1);
        char *tok = strtok(buf, ",");
        while (tok) {
            str_trim(tok);
            emit32(as, (uint32_t)eval_expr(as, tok));
            tok = strtok(NULL, ",");
        }
        return true;
    }

    /* .BLOCK n — reserve n bytes */
    if (strcmp(dir, ".BLOCK") == 0 || strcmp(dir, "DS.B") == 0 ||
        strcmp(dir, "DS.W") == 0 || strcmp(dir, "DS.L") == 0) {
        int32_t count = eval_expr(as, args);
        int mult = 1;
        if (strcmp(dir, "DS.W") == 0) mult = 2;
        if (strcmp(dir, "DS.L") == 0) mult = 4;
        for (int32_t i = 0; i < count * mult; i++) emit8(as, 0);
        return true;
    }

    /* .ASCII 'string' */
    if (strcmp(dir, ".ASCII") == 0) {
        const char *p = args;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\'' || *p == '"') {
            char delim = *p++;
            while (*p && *p != delim) emit8(as, *p++);
        }
        return true;
    }

    /* .ALIGN n — align to n-byte boundary */
    if (strcmp(dir, ".ALIGN") == 0 || strcmp(dir, "EVEN") == 0) {
        int align = (strcmp(dir, "EVEN") == 0) ? 2 : (int)eval_expr(as, args);
        if (align <= 0) align = 2;
        while (as->pc % align) emit8(as, 0);
        return true;
    }

    /* .SEG name — set segment name (for linker) */
    if (strcmp(dir, ".SEG") == 0) {
        /* Record segment name for object file output */
        return true;
    }

    return false;
}

/* ========================================================================
 * Line parser
 * ======================================================================== */

static void assemble_line(asm68k_t *as, const char *raw_line) {
    char line[ASM_MAX_LINE];
    strncpy(line, raw_line, ASM_MAX_LINE - 1);
    line[ASM_MAX_LINE - 1] = '\0';

    /* Strip comment (;) */
    char *comment = strchr(line, ';');
    if (comment) *comment = '\0';

    /* Strip trailing whitespace only (preserve leading for column detection) */
    {
        char *end = line + strlen(line) - 1;
        while (end > line && isspace((unsigned char)*end)) *end-- = '\0';
    }
    /* Check if line is blank */
    {
        const char *check = line;
        while (isspace((unsigned char)*check)) check++;
        if (*check == '\0') return;
    }

    /* Check conditional assembly */
    if (as->cond_depth > 0 && !as->cond_stack[as->cond_depth - 1].active) {
        /* Only process .IF/.ELSE/.ENDC in inactive sections */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '.') {
            char dir[64] = "";
            int i = 0;
            while (*p && !isspace((unsigned char)*p) && i < 63) dir[i++] = *p++;
            dir[i] = '\0';
            str_upper(dir);
            if (strcmp(dir, ".IF") == 0 || strcmp(dir, ".ELSE") == 0 || strcmp(dir, ".ENDC") == 0) {
                handle_directive(as, dir, p, "");
            }
        }
        return;
    }

    /* If in macro definition, collect lines */
    if (as->in_macro_def) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '.' && (str_eq_nocase(p, ".ENDM") || strncasecmp(p, ".ENDM", 5) == 0)) {
            as->in_macro_def = false;
            return;
        }
        asm_macro_t *m = &as->macros[as->current_macro];
        if (m->line_count < ASM_MAX_MACRO_LINES) {
            m->lines[m->line_count++] = strdup(line);
        }
        return;
    }

    /* Parse: [label] [directive/instruction] [operands] */
    char label[ASM_MAX_LABEL] = "";
    char *p = line;

    /* Label: starts at column 0, not whitespace, not directive.
     * Also handle @N local labels (e.g., @1, @100) at column 0. */
    if (!isspace((unsigned char)line[0]) && line[0] != '.') {
        int i = 0;
        while (*p && !isspace((unsigned char)*p) && *p != ':' && i < ASM_MAX_LABEL - 1) {
            label[i++] = *p++;
        }
        label[i] = '\0';
        if (*p == ':') p++;

        /* Check if next non-space is EQU */
        char *peek = p;
        while (*peek == ' ' || *peek == '\t') peek++;

        /* Handle: label EQU value (without dot) */
        if (strncasecmp(peek, "EQU", 3) == 0 && (peek[3] == ' ' || peek[3] == '\t')) {
            handle_directive(as, "EQU", peek + 3, label);
            return;
        }

        /* Handle: label .EQU value */
        if (strncasecmp(peek, ".EQU", 4) == 0 && (peek[4] == ' ' || peek[4] == '\t')) {
            handle_directive(as, ".EQU", peek + 4, label);
            return;
        }

        /* Define label at current PC */
        if (label[0]) {
            add_symbol(as, label, SYM_LABEL, as->pc);
        }
    }

    /* Skip whitespace to get to mnemonic/directive */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return;

    /* Get mnemonic/directive */
    char mnemonic[64] = "";
    int mi = 0;
    while (*p && !isspace((unsigned char)*p) && mi < 63) {
        mnemonic[mi++] = *p++;
    }
    mnemonic[mi] = '\0';

    /* Skip whitespace to get operands */
    while (*p == ' ' || *p == '\t') p++;
    char *operands = p;
    str_trim(operands);

    /* @N local labels can appear indented (e.g., "     @1 SWAP D2").
     * If mnemonic starts with @, treat it as a label and re-parse the rest. */
    if (mnemonic[0] == '@') {
        add_symbol(as, mnemonic, SYM_LABEL, as->pc);
        /* The rest of the line (operands) is actually "INSTRUCTION OPERANDS" */
        if (operands[0]) {
            /* Re-parse: split operands into new mnemonic + new operands */
            char *np = operands;
            mi = 0;
            while (*np && !isspace((unsigned char)*np) && mi < 63) {
                mnemonic[mi++] = *np++;
            }
            mnemonic[mi] = '\0';
            while (*np == ' ' || *np == '\t') np++;
            operands = np;
            str_trim(operands);
            /* Fall through to process the actual instruction */
        } else {
            return; /* Label-only line */
        }
    }

    /* Is it a directive? (starts with . or is a known directive like DC.W, DS.B, EVEN) */
    if (mnemonic[0] == '.') {
        handle_directive(as, mnemonic, operands, label);
        return;
    }

    /* Check for EQU without dot */
    if (str_eq_nocase(mnemonic, "EQU")) {
        handle_directive(as, "EQU", operands, label);
        return;
    }

    /* DC.x and DS.x directives (no leading dot) */
    if (strncasecmp(mnemonic, "DC.", 3) == 0 || strncasecmp(mnemonic, "DS.", 3) == 0) {
        handle_directive(as, mnemonic, operands, label);
        return;
    }
    if (str_eq_nocase(mnemonic, "EVEN")) {
        handle_directive(as, "EVEN", operands, label);
        return;
    }

    /* Check if it's a macro invocation */
    for (int i = 0; i < as->num_macros; i++) {
        if (str_eq_nocase(as->macros[i].name, mnemonic)) {
            /* Parse macro arguments (comma-separated) */
            char *macro_args[16];
            int nargs = 0;
            if (operands[0]) {
                char argbuf[512];
                strncpy(argbuf, operands, sizeof(argbuf) - 1);
                argbuf[sizeof(argbuf) - 1] = '\0';
                char *tok = strtok(argbuf, ",");
                while (tok && nargs < 16) {
                    while (*tok == ' ' || *tok == '\t') tok++;
                    /* Trim trailing whitespace */
                    char *end = tok + strlen(tok) - 1;
                    while (end > tok && (*end == ' ' || *end == '\t')) *end-- = '\0';
                    macro_args[nargs] = strdup(tok);
                    nargs++;
                    tok = strtok(NULL, ",");
                }
            }
            /* Expand macro with parameter substitution */
            for (int j = 0; j < as->macros[i].line_count; j++) {
                const char *src = as->macros[i].lines[j];
                char expanded[ASM_MAX_LINE];
                int ei = 0;
                while (*src && ei < ASM_MAX_LINE - 1) {
                    if (*src == '%' && src[1] >= '1' && src[1] <= '9') {
                        int pn = src[1] - '1'; /* %1 = arg 0 */
                        if (pn < nargs && macro_args[pn]) {
                            const char *arg = macro_args[pn];
                            while (*arg && ei < ASM_MAX_LINE - 1) {
                                expanded[ei++] = *arg++;
                            }
                        }
                        src += 2;
                    } else {
                        expanded[ei++] = *src++;
                    }
                }
                expanded[ei] = '\0';
                assemble_line(as, expanded);
            }
            /* Free arg copies */
            for (int a = 0; a < nargs; a++) free(macro_args[a]);
            return;
        }
    }

    /* It's an instruction */
    assemble_instruction(as, mnemonic, operands);
}

/* Forward declaration for built-in macros */
static void add_builtin_macro(asm68k_t *as, const char *name, const char *line);

/* ========================================================================
 * Two-pass assembly driver
 * ======================================================================== */

static bool assemble_pass(asm68k_t *as, const char *source) {
    as->pc = 0;
    as->line_num = 0;
    as->cond_depth = 0;
    as->in_macro_def = false;

    /* Free and reset macros for this pass — they'll be re-collected.
     * This prevents the macro table from doubling on pass 2. */
    for (int i = 0; i < as->num_macros; i++) {
        for (int j = 0; j < as->macros[i].line_count; j++) {
            free(as->macros[i].lines[j]);
            as->macros[i].lines[j] = NULL;
        }
        as->macros[i].line_count = 0;
    }
    as->num_macros = 0;

    /* Re-add built-in macros */
    add_builtin_macro(as, "FCOMPOUND", "        FCOMPOUNDX");
    add_builtin_macro(as, "FANNUITY",  "        FANNUITYX");

    /* Reset sections for this pass */
    for (int i = 0; i < as->num_sections; i++) {
        as->sections[i].size = 0;
    }

    const char *p = source;
    char line[ASM_MAX_LINE];

    while (*p) {
        as->line_num++;
        int i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < ASM_MAX_LINE - 1) {
            line[i++] = *p++;
        }
        line[i] = '\0';
        if (*p == '\r') p++;
        if (*p == '\n') p++;

        assemble_line(as, line);
    }

    return as->num_errors == 0;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

static void add_builtin_macro(asm68k_t *as, const char *name, const char *line) {
    if (as->num_macros >= ASM_MAX_MACROS) return;
    int idx = as->num_macros++;
    strncpy(as->macros[idx].name, name, ASM_MAX_LABEL - 1);
    as->macros[idx].lines[0] = strdup(line);
    as->macros[idx].line_count = 1;
    as->macros[idx].param_count = 0;
}

void asm68k_init(asm68k_t *as) {
    memset(as, 0, sizeof(asm68k_t));

    /* Built-in macros for SANE floating-point library.
     * FCOMPOUND/FANNUITY are missing from sanemacs but used in elemsASM.
     * They're equivalent to the X (extended precision) variants. */
    add_builtin_macro(as, "FCOMPOUND", "        FCOMPOUNDX");
    add_builtin_macro(as, "FANNUITY",  "        FANNUITYX");
}

void asm68k_free(asm68k_t *as) {
    for (int i = 0; i < as->num_sections; i++) {
        free(as->sections[i].data);
    }
    for (int i = 0; i < as->num_macros; i++) {
        for (int j = 0; j < as->macros[i].line_count; j++) {
            free(as->macros[i].lines[j]);
        }
    }
    if (as->output) free(as->output);
    memset(as, 0, sizeof(asm68k_t));
}

void asm68k_set_base_dir(asm68k_t *as, const char *dir) {
    strncpy(as->base_dir, dir, sizeof(as->base_dir) - 1);
}

void asm68k_add_include_path(asm68k_t *as, const char *path) {
    if (as->num_include_paths < ASM_MAX_INCLUDES) {
        strncpy(as->include_paths[as->num_include_paths], path, 255);
        as->num_include_paths++;
    }
}

bool asm68k_assemble_string(asm68k_t *as, const char *source, const char *filename) {
    strncpy(as->current_file, filename, sizeof(as->current_file) - 1);
    as->num_errors = 0;

    /* Pass 1: collect symbols and sizes */
    as->pass = 1;
    assemble_pass(as, source);

    /* Pass 2: generate code */
    as->pass = 2;
    as->num_errors = 0;
    assemble_pass(as, source);

    /* Consolidate output */
    if (as->num_sections > 0) {
        asm_section_t *sec = &as->sections[0];
        as->output = sec->data;
        as->output_size = sec->size;
        sec->data = NULL; /* Prevent double free */
    }

    return as->num_errors == 0;
}

bool asm68k_assemble_file(asm68k_t *as, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Cannot open: %s\n", filename);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *source = malloc(size + 1);
    fread(source, 1, size, f);
    source[size] = '\0';
    fclose(f);

    bool result = asm68k_assemble_string(as, source, filename);
    free(source);
    return result;
}

const uint8_t *asm68k_get_output(asm68k_t *as, uint32_t *size) {
    if (size) *size = as->output_size;
    return as->output;
}

int asm68k_get_error_count(asm68k_t *as) {
    return as->num_errors;
}

const char *asm68k_get_error(asm68k_t *as, int index) {
    if (index < 0 || index >= as->num_errors) return "";
    return as->errors[index].message;
}

void asm68k_dump_symbols(asm68k_t *as) {
    printf("=== Symbol Table (%d symbols) ===\n", as->num_symbols);
    for (int i = 0; i < as->num_symbols; i++) {
        asm_symbol_t *s = &as->symbols[i];
        const char *type_str;
        switch (s->type) {
            case SYM_LABEL: type_str = "LABEL"; break;
            case SYM_EQU:   type_str = "EQU  "; break;
            case SYM_DEF:   type_str = "DEF  "; break;
            case SYM_REF:   type_str = "REF  "; break;
            case SYM_PROC:  type_str = "PROC "; break;
            case SYM_FUNC:  type_str = "FUNC "; break;
            case SYM_MACRO: type_str = "MACRO"; break;
            default:        type_str = "?????"; break;
        }
        printf("  %s  %08X  %s%s%s\n", type_str, s->value, s->name,
               s->exported ? " [EXPORT]" : "",
               s->external ? " [EXTERN]" : "");
    }
}

bool asm68k_write_obj(asm68k_t *as, const char *filename) {
    /* Write a simple Lisa-compatible object file */
    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    /* Simple object format header:
       Magic: "LOBJ"
       Version: 1
       Code size: 4 bytes
       Num symbols: 4 bytes
       Code data: ...
       Symbol table: ...
    */
    fwrite("LOBJ", 1, 4, f);
    uint32_t version = 1;
    fwrite(&version, 4, 1, f);
    fwrite(&as->output_size, 4, 1, f);
    uint32_t nsyms = as->num_symbols;
    fwrite(&nsyms, 4, 1, f);

    /* Code */
    if (as->output && as->output_size > 0) {
        fwrite(as->output, 1, as->output_size, f);
    }

    /* Symbols */
    for (int i = 0; i < as->num_symbols; i++) {
        asm_symbol_t *s = &as->symbols[i];
        uint8_t type = (uint8_t)s->type;
        uint8_t flags = (s->exported ? 1 : 0) | (s->external ? 2 : 0) | (s->defined ? 4 : 0);
        uint8_t namelen = (uint8_t)strlen(s->name);
        fwrite(&type, 1, 1, f);
        fwrite(&flags, 1, 1, f);
        fwrite(&namelen, 1, 1, f);
        fwrite(s->name, 1, namelen, f);
        fwrite(&s->value, 4, 1, f);
    }

    fclose(f);
    printf("Wrote object file: %s (%u bytes code, %d symbols)\n",
           filename, as->output_size, as->num_symbols);
    return true;
}
