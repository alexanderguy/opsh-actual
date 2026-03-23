#include "vm/arith.h"

#include "foundation/rcstr.h"
#include "foundation/util.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARITH_MAX_DEPTH 32

typedef struct {
    const char *pos;
    environ_t *env;
    arith_error_t err;
    int depth;   /* recursion depth for variable evaluation */
    bool noexec; /* when true, parse but suppress side effects */
} arith_state_t;

static void skip_ws(arith_state_t *s)
{
    while (*s->pos == ' ' || *s->pos == '\t' || *s->pos == '\n') {
        s->pos++;
    }
}

static bool is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* Forward declarations */
static int64_t parse_expr(arith_state_t *s);
static int64_t parse_assign(arith_state_t *s);

/* Resolve a variable name to its integer value, with recursive evaluation. */
static int64_t resolve_var(arith_state_t *s, const char *name)
{
    if (s->depth >= ARITH_MAX_DEPTH) {
        s->err = ARITH_ERR_DEPTH;
        return 0;
    }

    variable_t *var = environ_get(s->env, name);
    if (var == NULL || var->value.type != VT_STRING) {
        return 0; /* unset or non-string treated as 0 */
    }

    const char *val = var->value.data.string;
    if (val[0] == '\0') {
        return 0;
    }

    /* Recursively evaluate the variable's value as an arithmetic expression */
    arith_state_t sub;
    sub.pos = val;
    sub.env = s->env;
    sub.err = ARITH_OK;
    sub.depth = s->depth + 1;
    sub.noexec = s->noexec;
    int64_t result = parse_expr(&sub);
    if (sub.err != ARITH_OK) {
        s->err = sub.err;
    }
    return result;
}

/* Set a variable to an integer value. Suppressed in noexec mode. */
static void set_var(arith_state_t *s, const char *name, int64_t val)
{
    if (s->noexec) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, val);
    environ_assign(s->env, name, value_string(rcstr_new(buf)));
}

/* Parse an identifier name. Returns malloc'd string or NULL. */
static char *parse_ident(arith_state_t *s)
{
    skip_ws(s);
    if (!is_ident_start(*s->pos)) {
        return NULL;
    }
    const char *start = s->pos;
    while (is_ident_char(*s->pos)) {
        s->pos++;
    }
    size_t len = (size_t)(s->pos - start);
    char *name = xmalloc(len + 1);
    memcpy(name, start, len);
    name[len] = '\0';
    return name;
}

/* Parse a primary expression: number, variable, or parenthesized expression. */
static int64_t parse_primary(arith_state_t *s)
{
    skip_ws(s);
    if (s->err != ARITH_OK) {
        return 0;
    }

    /* Parenthesized expression */
    if (*s->pos == '(') {
        s->pos++;
        int64_t val = parse_expr(s);
        skip_ws(s);
        if (*s->pos == ')') {
            s->pos++;
        } else {
            s->err = ARITH_ERR_SYNTAX;
        }
        return val;
    }

    /* Number literal (base 0 handles octal and hex) */
    if (isdigit((unsigned char)*s->pos)) {
        char *endp;
        int64_t val = strtoll(s->pos, &endp, 0);
        s->pos = endp;
        return val;
    }

    /* Variable with optional pre-increment/decrement */
    if (is_ident_start(*s->pos)) {
        char *name = parse_ident(s);
        skip_ws(s);

        /* Check for post-increment/decrement */
        if (s->pos[0] == '+' && s->pos[1] == '+') {
            s->pos += 2;
            int64_t val = resolve_var(s, name);
            set_var(s, name, val + 1);
            free(name);
            return val; /* return old value */
        }
        if (s->pos[0] == '-' && s->pos[1] == '-') {
            s->pos += 2;
            int64_t val = resolve_var(s, name);
            set_var(s, name, val - 1);
            free(name);
            return val; /* return old value */
        }

        /* Check for assignment operators */
        if (*s->pos == '=' && s->pos[1] != '=') {
            s->pos++;
            int64_t rhs = parse_assign(s);
            set_var(s, name, rhs);
            free(name);
            return rhs;
        }

        /* Compound assignment operators */
        {
            char op1 = s->pos[0];
            char op2 = op1 ? s->pos[1] : '\0';
            if (op2 == '=' && (op1 == '+' || op1 == '-' || op1 == '*' || op1 == '/' || op1 == '%' ||
                               op1 == '&' || op1 == '^' || op1 == '|')) {
                s->pos += 2;
                int64_t lhs = resolve_var(s, name);
                int64_t rhs = parse_assign(s);
                int64_t result = 0;
                switch (op1) {
                case '+':
                    result = lhs + rhs;
                    break;
                case '-':
                    result = lhs - rhs;
                    break;
                case '*':
                    result = lhs * rhs;
                    break;
                case '/':
                    if (rhs == 0) {
                        if (!s->noexec) {
                            s->err = ARITH_ERR_DIV_ZERO;
                        }
                        free(name);
                        return 0;
                    }
                    result = (lhs == INT64_MIN && rhs == -1) ? INT64_MIN : lhs / rhs;
                    break;
                case '%':
                    if (rhs == 0) {
                        if (!s->noexec) {
                            s->err = ARITH_ERR_DIV_ZERO;
                        }
                        free(name);
                        return 0;
                    }
                    result = (lhs == INT64_MIN && rhs == -1) ? 0 : lhs % rhs;
                    break;
                case '&':
                    result = lhs & rhs;
                    break;
                case '^':
                    result = lhs ^ rhs;
                    break;
                case '|':
                    result = lhs | rhs;
                    break;
                }
                set_var(s, name, result);
                free(name);
                return result;
            }

            /* <<= and >>= */
            if ((op1 == '<' || op1 == '>') && op1 == op2 && op2 != '\0' && s->pos[2] == '=') {
                s->pos += 3;
                int64_t lhs = resolve_var(s, name);
                int64_t rhs = parse_assign(s);
                int64_t shift = (rhs < 0 || rhs >= 64) ? 0 : rhs;
                int64_t result = (op1 == '<') ? (int64_t)((uint64_t)lhs << shift) : (lhs >> shift);
                set_var(s, name, result);
                free(name);
                return result;
            }
        }

        /* Plain variable reference */
        int64_t val = resolve_var(s, name);
        free(name);
        return val;
    }

    /* Nothing recognized */
    if (*s->pos != '\0') {
        s->err = ARITH_ERR_SYNTAX;
    }
    return 0;
}

/* Parse unary: +, -, !, ~, pre-increment/decrement */
static int64_t parse_unary(arith_state_t *s)
{
    skip_ws(s);
    if (s->err != ARITH_OK) {
        return 0;
    }

    if (*s->pos == '+' && s->pos[1] != '+' && s->pos[1] != '=') {
        s->pos++;
        return parse_unary(s);
    }
    if (*s->pos == '-' && s->pos[1] != '-' && s->pos[1] != '=') {
        s->pos++;
        return (int64_t)(-(uint64_t)parse_unary(s));
    }
    if (*s->pos == '!') {
        s->pos++;
        return !parse_unary(s);
    }
    if (*s->pos == '~') {
        s->pos++;
        return ~parse_unary(s);
    }

    /* Pre-increment/decrement */
    if (s->pos[0] == '+' && s->pos[1] == '+') {
        s->pos += 2;
        skip_ws(s);
        char *name = parse_ident(s);
        if (name == NULL) {
            s->err = ARITH_ERR_SYNTAX;
            return 0;
        }
        int64_t val = resolve_var(s, name) + 1;
        set_var(s, name, val);
        free(name);
        return val;
    }
    if (s->pos[0] == '-' && s->pos[1] == '-') {
        s->pos += 2;
        skip_ws(s);
        char *name = parse_ident(s);
        if (name == NULL) {
            s->err = ARITH_ERR_SYNTAX;
            return 0;
        }
        int64_t val = resolve_var(s, name) - 1;
        set_var(s, name, val);
        free(name);
        return val;
    }

    return parse_primary(s);
}

/* Multiplicative: * / % */
static int64_t parse_mul(arith_state_t *s)
{
    int64_t left = parse_unary(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return left;
        }
        char op = *s->pos;
        if (op == '*' && s->pos[1] != '=') {
            s->pos++;
            left = (int64_t)((uint64_t)left * (uint64_t)parse_unary(s));
        } else if (op == '/' && s->pos[1] != '=') {
            s->pos++;
            int64_t rhs = parse_unary(s);
            if (rhs == 0) {
                if (!s->noexec) {
                    s->err = ARITH_ERR_DIV_ZERO;
                }
                return 0;
            }
            if (left == INT64_MIN && rhs == -1) {
                left = INT64_MIN;
            } else {
                left /= rhs;
            }
        } else if (op == '%' && s->pos[1] != '=') {
            s->pos++;
            int64_t rhs = parse_unary(s);
            if (rhs == 0) {
                if (!s->noexec) {
                    s->err = ARITH_ERR_DIV_ZERO;
                }
                return 0;
            }
            if (left == INT64_MIN && rhs == -1) {
                left = 0;
            } else {
                left %= rhs;
            }
        } else {
            break;
        }
    }
    return left;
}

/* Additive: + - */
static int64_t parse_add(arith_state_t *s)
{
    int64_t left = parse_mul(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return left;
        }
        char op = *s->pos;
        if (op == '+' && s->pos[1] != '+' && s->pos[1] != '=') {
            s->pos++;
            left = (int64_t)((uint64_t)left + (uint64_t)parse_mul(s));
        } else if (op == '-' && s->pos[1] != '-' && s->pos[1] != '=') {
            s->pos++;
            left = (int64_t)((uint64_t)left - (uint64_t)parse_mul(s));
        } else {
            break;
        }
    }
    return left;
}

/* Shift: << >> */
static int64_t parse_shift(arith_state_t *s)
{
    int64_t left = parse_add(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return left;
        }
        if (s->pos[0] == '<' && s->pos[1] == '<' && s->pos[2] != '=') {
            s->pos += 2;
            int64_t rhs = parse_add(s);
            left = (rhs < 0 || rhs >= 64) ? 0 : (int64_t)((uint64_t)left << rhs);
        } else if (s->pos[0] == '>' && s->pos[1] == '>' && s->pos[2] != '=') {
            s->pos += 2;
            int64_t rhs = parse_add(s);
            left = (rhs < 0 || rhs >= 64) ? 0 : (left >> rhs);
        } else {
            break;
        }
    }
    return left;
}

/* Relational: < <= > >= */
static int64_t parse_rel(arith_state_t *s)
{
    int64_t left = parse_shift(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return left;
        }
        if (s->pos[0] == '<' && s->pos[1] == '=') {
            s->pos += 2;
            left = (left <= parse_shift(s)) ? 1 : 0;
        } else if (s->pos[0] == '>' && s->pos[1] == '=') {
            s->pos += 2;
            left = (left >= parse_shift(s)) ? 1 : 0;
        } else if (s->pos[0] == '<' && s->pos[1] != '<') {
            s->pos++;
            left = (left < parse_shift(s)) ? 1 : 0;
        } else if (s->pos[0] == '>' && s->pos[1] != '>') {
            s->pos++;
            left = (left > parse_shift(s)) ? 1 : 0;
        } else {
            break;
        }
    }
    return left;
}

/* Equality: == != */
static int64_t parse_eq(arith_state_t *s)
{
    int64_t left = parse_rel(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return left;
        }
        if (s->pos[0] == '=' && s->pos[1] == '=') {
            s->pos += 2;
            left = (left == parse_rel(s)) ? 1 : 0;
        } else if (s->pos[0] == '!' && s->pos[1] == '=') {
            s->pos += 2;
            left = (left != parse_rel(s)) ? 1 : 0;
        } else {
            break;
        }
    }
    return left;
}

/* Bitwise AND: & */
static int64_t parse_bit_and(arith_state_t *s)
{
    int64_t left = parse_eq(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return left;
        }
        if (s->pos[0] == '&' && s->pos[1] != '&' && s->pos[1] != '=') {
            s->pos++;
            left &= parse_eq(s);
        } else {
            break;
        }
    }
    return left;
}

/* Bitwise XOR: ^ */
static int64_t parse_bit_xor(arith_state_t *s)
{
    int64_t left = parse_bit_and(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return left;
        }
        if (s->pos[0] == '^' && s->pos[1] != '=') {
            s->pos++;
            left ^= parse_bit_and(s);
        } else {
            break;
        }
    }
    return left;
}

/* Bitwise OR: | */
static int64_t parse_bit_or(arith_state_t *s)
{
    int64_t left = parse_bit_xor(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return left;
        }
        if (s->pos[0] == '|' && s->pos[1] != '|' && s->pos[1] != '=') {
            s->pos++;
            left |= parse_bit_xor(s);
        } else {
            break;
        }
    }
    return left;
}

/* Logical AND: && (short-circuit) */
static int64_t parse_log_and(arith_state_t *s)
{
    int64_t left = parse_bit_or(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return left;
        }
        if (s->pos[0] == '&' && s->pos[1] == '&') {
            s->pos += 2;
            if (left == 0) {
                bool was_noexec = s->noexec;
                s->noexec = true;
                parse_bit_or(s);
                s->noexec = was_noexec;
                left = 0;
            } else {
                left = (parse_bit_or(s) != 0) ? 1 : 0;
            }
        } else {
            break;
        }
    }
    return left;
}

/* Logical OR: || (short-circuit) */
static int64_t parse_log_or(arith_state_t *s)
{
    int64_t left = parse_log_and(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return left;
        }
        if (s->pos[0] == '|' && s->pos[1] == '|') {
            s->pos += 2;
            if (left != 0) {
                bool was_noexec = s->noexec;
                s->noexec = true;
                parse_log_and(s);
                s->noexec = was_noexec;
                left = 1;
            } else {
                left = (parse_log_and(s) != 0) ? 1 : 0;
            }
        } else {
            break;
        }
    }
    return left;
}

/* Ternary: expr ? expr : expr (short-circuit) */
static int64_t parse_ternary(arith_state_t *s)
{
    int64_t cond = parse_log_or(s);
    skip_ws(s);
    if (s->err != ARITH_OK) {
        return cond;
    }
    if (*s->pos != '?') {
        return cond;
    }
    s->pos++;

    if (cond != 0) {
        int64_t val = parse_assign(s);
        skip_ws(s);
        if (*s->pos == ':') {
            s->pos++;
            bool was_noexec = s->noexec;
            s->noexec = true;
            parse_assign(s);
            s->noexec = was_noexec;
        } else {
            s->err = ARITH_ERR_SYNTAX;
        }
        return val;
    } else {
        bool was_noexec = s->noexec;
        s->noexec = true;
        parse_assign(s);
        s->noexec = was_noexec;
        skip_ws(s);
        if (*s->pos == ':') {
            s->pos++;
            return parse_assign(s);
        }
        s->err = ARITH_ERR_SYNTAX;
        return 0;
    }
}

/* Assignment expression (right-associative).
 * Assignment operators are handled in parse_primary when we see an identifier. */
static int64_t parse_assign(arith_state_t *s)
{
    return parse_ternary(s);
}

/* Comma: expr, expr (lowest precedence) */
static int64_t parse_expr(arith_state_t *s)
{
    int64_t val = parse_assign(s);
    for (;;) {
        skip_ws(s);
        if (s->err != ARITH_OK) {
            return val;
        }
        if (*s->pos == ',') {
            s->pos++;
            val = parse_assign(s);
        } else {
            break;
        }
    }
    return val;
}

int64_t arith_eval(const char *expr, environ_t *env, arith_error_t *err)
{
    arith_state_t s;
    s.pos = expr;
    s.env = env;
    s.err = ARITH_OK;
    s.depth = 0;
    s.noexec = false;

    /* The lexer includes a trailing ) from the $((...)) delimiter.
     * Check for empty expression (just the trailing paren). */
    skip_ws(&s);
    if (*s.pos == ')' && s.pos[1] == '\0') {
        *err = ARITH_OK;
        return 0;
    }

    int64_t result = parse_expr(&s);

    skip_ws(&s);
    if (*s.pos == ')') {
        s.pos++;
    }
    skip_ws(&s);
    if (s.err == ARITH_OK && *s.pos != '\0') {
        s.err = ARITH_ERR_SYNTAX;
    }

    *err = s.err;
    return (s.err == ARITH_OK) ? result : 0;
}
