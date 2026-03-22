#include "parser/lexer.h"

#include "foundation/util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocate zeroed memory from the lexer's arena for word_part/param_exp nodes. */
static void *lexer_alloc(lexer_t *lex, size_t size)
{
    return arena_calloc(lex->arena, size);
}


static void lexer_error(lexer_t *lex, const char *msg)
{
    fprintf(stderr, "opsh: %s:%u: %s\n", lex->filename, lex->lineno, msg);
    lex->error_count++;
}

static char lexer_char(const lexer_t *lex)
{
    if (lex->pos >= lex->length) {
        return '\0';
    }
    return lex->source[lex->pos];
}

static char lexer_peek_char(const lexer_t *lex, size_t offset)
{
    size_t p = lex->pos + offset;
    if (p >= lex->length) {
        return '\0';
    }
    return lex->source[p];
}

static void lexer_advance(lexer_t *lex)
{
    if (lex->pos < lex->length) {
        if (lex->source[lex->pos] == '\n') {
            lex->lineno++;
        }
        lex->pos++;
    }
}

static void lexer_skip_whitespace(lexer_t *lex)
{
    while (lex->pos < lex->length) {
        char c = lex->source[lex->pos];
        if (c == ' ' || c == '\t') {
            lex->pos++;
        } else if (c == '#') {
            /* skip comment to end of line */
            while (lex->pos < lex->length && lex->source[lex->pos] != '\n') {
                lex->pos++;
            }
        } else if (c == '\\' && lexer_peek_char(lex, 1) == '\n') {
            /* line continuation */
            lex->pos += 2;
            lex->lineno++;
        } else {
            break;
        }
    }
}

static bool is_operator_start(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')';
}

static bool is_word_char(char c)
{
    if (c == '\0' || c == '\n') {
        return false;
    }
    if (c == ' ' || c == '\t') {
        return false;
    }
    if (is_operator_start(c)) {
        return false;
    }
    return true;
}

/*
 * Parse a parameter expansion starting after the $.
 * Handles $name, ${name}, ${name:-word}, ${name#pattern}, ${#name}, etc.
 */
static word_part_t *parse_param_expansion(lexer_t *lex)
{
    word_part_t *wu = lexer_alloc(lex, sizeof(*wu));
    wu->type = WP_PARAM;

    param_exp_t *pe = lexer_alloc(lex, sizeof(*pe));
    wu->part.param = pe;
    pe->type = PE_NONE;
    pe->flags = 0;

    char c = lexer_char(lex);

    /* Special parameters: $?, $$, $!, $#, $@, $*, $0-$9 */
    if (c == '?' || c == '$' || c == '!' || c == '#' || c == '@' || c == '*' || c == '-' ||
        c == '_' || (c >= '0' && c <= '9')) {
        char buf[2] = {c, '\0'};
        pe->name = xmalloc(2);
        memcpy(pe->name, buf, 2);
        lexer_advance(lex);
        return wu;
    }

    /* $name (unbraced) */
    if (c != '{') {
        strbuf_t name;
        strbuf_init(&name);
        while (lex->pos < lex->length) {
            c = lexer_char(lex);
            if (isalnum((unsigned char)c) || c == '_') {
                strbuf_append_byte(&name, c);
                lexer_advance(lex);
            } else {
                break;
            }
        }
        if (name.length == 0) {
            strbuf_destroy(&name);
            /* bare $ with nothing after it -- treat as literal */
            wu->type = WP_LITERAL;
            /* pe is arena-allocated, no need to free */
            wu->part.string = xmalloc(2);
            wu->part.string[0] = '$';
            wu->part.string[1] = '\0';
            return wu;
        }
        pe->name = strbuf_detach(&name);
        return wu;
    }

    /* ${...} braced expansion */
    lexer_advance(lex); /* skip { */

    /* ${#name} -- length */
    if (lexer_char(lex) == '#' && lexer_peek_char(lex, 1) != '}') {
        pe->flags |= PE_STRLEN;
        lexer_advance(lex); /* skip # */
    }

    /* Read the variable name */
    {
        strbuf_t name;
        strbuf_init(&name);

        c = lexer_char(lex);
        /* Special parameters inside braces */
        if (c == '?' || c == '$' || c == '!' || c == '#' || c == '@' || c == '*' || c == '-' ||
            (c >= '0' && c <= '9')) {
            strbuf_append_byte(&name, c);
            lexer_advance(lex);
        } else {
            while (lex->pos < lex->length) {
                c = lexer_char(lex);
                if (isalnum((unsigned char)c) || c == '_') {
                    strbuf_append_byte(&name, c);
                    lexer_advance(lex);
                } else {
                    break;
                }
            }
        }
        pe->name = strbuf_detach(&name);
    }

    c = lexer_char(lex);

    /* ${name} -- simple braced expansion */
    if (c == '}') {
        lexer_advance(lex);
        return wu;
    }

    /* Determine the operation */
    if (c == ':') {
        pe->flags |= PE_COLON;
        lexer_advance(lex);
        c = lexer_char(lex);
    }

    if (c == '-') {
        pe->type = PE_DEFAULT;
        lexer_advance(lex);
    } else if (c == '+') {
        pe->type = PE_ALTERNATE;
        lexer_advance(lex);
    } else if (c == '=') {
        pe->type = PE_ASSIGN;
        lexer_advance(lex);
    } else if (c == '?') {
        pe->type = PE_ERROR;
        lexer_advance(lex);
    } else if (c == '#') {
        pe->type = PE_TRIM;
        pe->flags |= PE_PREFIX;
        lexer_advance(lex);
        if (lexer_char(lex) == '#') {
            pe->flags |= PE_LONGEST;
            lexer_advance(lex);
        }
    } else if (c == '%') {
        pe->type = PE_TRIM;
        pe->flags |= PE_SUFFIX;
        lexer_advance(lex);
        if (lexer_char(lex) == '%') {
            pe->flags |= PE_LONGEST;
            lexer_advance(lex);
        }
    } else if (c == '/') {
        pe->type = PE_REPLACE;
        lexer_advance(lex);
        if (lexer_char(lex) == '/') {
            pe->flags |= PE_GLOBAL;
            lexer_advance(lex);
        }
    } else {
        /* Unexpected character -- bail */
        lexer_error(lex, "unexpected character in parameter expansion");
        /* Skip to closing brace */
        while (lex->pos < lex->length && lexer_char(lex) != '}') {
            lexer_advance(lex);
        }
        if (lexer_char(lex) == '}') {
            lexer_advance(lex);
        }
        return wu;
    }

    /* Parse the word argument (up to } or / for substitution) */
    {
        strbuf_t word;
        int depth = 1;
        strbuf_init(&word);

        while (lex->pos < lex->length && depth > 0) {
            c = lexer_char(lex);
            if (c == '}') {
                depth--;
                if (depth == 0) {
                    break;
                }
            } else if (c == '{') {
                depth++;
            } else if (c == '/' && pe->type == PE_REPLACE && pe->pattern == NULL) {
                /* Switch from pattern to replacement */
                word_part_t *mw = lexer_alloc(lex, sizeof(*mw));
                mw->type = WP_LITERAL;
                mw->part.string = strbuf_detach(&word);
                pe->pattern = mw;
                strbuf_init(&word);
                lexer_advance(lex);
                continue;
            }
            strbuf_append_byte(&word, c);
            lexer_advance(lex);
        }

        if (word.length > 0) {
            word_part_t *ww = lexer_alloc(lex, sizeof(*ww));
            ww->type = WP_LITERAL;
            ww->part.string = strbuf_detach(&word);
            if (pe->type == PE_REPLACE && pe->pattern != NULL) {
                pe->replacement = ww;
            } else if (pe->type == PE_REPLACE) {
                pe->pattern = ww;
            } else {
                pe->pattern = ww;
            }
        } else {
            strbuf_destroy(&word);
        }
    }

    if (lexer_char(lex) == '}') {
        lexer_advance(lex);
    } else {
        lexer_error(lex, "missing closing } in parameter expansion");
    }

    return wu;
}

/*
 * Parse a command substitution $(...) starting after the (.
 * Tracks parenthesis nesting to find the matching ).
 * The content is stored as unparsed text; the parser will re-parse it.
 */
static word_part_t *parse_cmdsub(lexer_t *lex)
{
    word_part_t *wu = lexer_alloc(lex, sizeof(*wu));
    wu->type = WP_CMDSUB;
    wu->part.cmdsub.is_preparsed = false;

    strbuf_t content;
    strbuf_init(&content);
    int depth = 1;

    while (lex->pos < lex->length && depth > 0) {
        char c = lexer_char(lex);
        if (c == '\'') {
            /* Skip single-quoted string */
            strbuf_append_byte(&content, c);
            lexer_advance(lex);
            while (lex->pos < lex->length && lexer_char(lex) != '\'') {
                strbuf_append_byte(&content, lexer_char(lex));
                lexer_advance(lex);
            }
            if (lex->pos < lex->length) {
                strbuf_append_byte(&content, '\'');
                lexer_advance(lex);
            }
            continue;
        }
        if (c == '"') {
            /* Skip double-quoted string */
            strbuf_append_byte(&content, c);
            lexer_advance(lex);
            while (lex->pos < lex->length && lexer_char(lex) != '"') {
                if (lexer_char(lex) == '\\' && lex->pos + 1 < lex->length) {
                    strbuf_append_byte(&content, '\\');
                    lexer_advance(lex);
                }
                strbuf_append_byte(&content, lexer_char(lex));
                lexer_advance(lex);
            }
            if (lex->pos < lex->length) {
                strbuf_append_byte(&content, '"');
                lexer_advance(lex);
            }
            continue;
        }
        if (c == '\\' && lex->pos + 1 < lex->length) {
            strbuf_append_byte(&content, c);
            lexer_advance(lex);
            strbuf_append_byte(&content, lexer_char(lex));
            lexer_advance(lex);
            continue;
        }
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                lexer_advance(lex);
                break;
            }
        }
        strbuf_append_byte(&content, c);
        lexer_advance(lex);
    }

    wu->part.cmdsub.u.unparsed = strbuf_detach(&content);
    return wu;
}

/*
 * Parse an arithmetic expansion $((...)) starting after the ((.
 */
static word_part_t *parse_arith(lexer_t *lex)
{
    word_part_t *wu = lexer_alloc(lex, sizeof(*wu));
    wu->type = WP_ARITH;

    strbuf_t content;
    strbuf_init(&content);
    int depth = 2; /* started with (( */

    while (lex->pos < lex->length && depth > 0) {
        char c = lexer_char(lex);
        if (c == '\'') {
            strbuf_append_byte(&content, c);
            lexer_advance(lex);
            while (lex->pos < lex->length && lexer_char(lex) != '\'') {
                strbuf_append_byte(&content, lexer_char(lex));
                lexer_advance(lex);
            }
            if (lex->pos < lex->length) {
                strbuf_append_byte(&content, '\'');
                lexer_advance(lex);
            }
            continue;
        }
        if (c == '"') {
            strbuf_append_byte(&content, c);
            lexer_advance(lex);
            while (lex->pos < lex->length && lexer_char(lex) != '"') {
                if (lexer_char(lex) == '\\' && lex->pos + 1 < lex->length) {
                    strbuf_append_byte(&content, '\\');
                    lexer_advance(lex);
                }
                strbuf_append_byte(&content, lexer_char(lex));
                lexer_advance(lex);
            }
            if (lex->pos < lex->length) {
                strbuf_append_byte(&content, '"');
                lexer_advance(lex);
            }
            continue;
        }
        if (c == '(') {
            depth++;
            strbuf_append_byte(&content, c);
            lexer_advance(lex);
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                lexer_advance(lex); /* skip final ) */
                break;
            }
            strbuf_append_byte(&content, c);
            lexer_advance(lex);
        } else {
            strbuf_append_byte(&content, c);
            lexer_advance(lex);
        }
    }

    /* Store the arithmetic expression as a string wordunit */
    word_part_t *expr = lexer_alloc(lex, sizeof(*expr));
    expr->type = WP_LITERAL;
    expr->part.string = strbuf_detach(&content);
    wu->part.arith = expr;

    return wu;
}

/*
 * Parse a dollar expression: $var, ${...}, $(...), $((...))
 * Called after consuming the $.
 */
static word_part_t *parse_dollar(lexer_t *lex)
{
    char c = lexer_char(lex);

    if (c == '(') {
        lexer_advance(lex);
        if (lexer_char(lex) == '(') {
            lexer_advance(lex);
            return parse_arith(lex);
        }
        return parse_cmdsub(lex);
    }

    return parse_param_expansion(lex);
}

/*
 * Parse backtick command substitution `...`
 * Called after consuming the opening `.
 */
static word_part_t *parse_backtick(lexer_t *lex)
{
    word_part_t *wu = lexer_alloc(lex, sizeof(*wu));
    wu->type = WP_CMDSUB;
    wu->part.cmdsub.is_preparsed = false;

    strbuf_t content;
    strbuf_init(&content);

    {
        bool found_close = false;
        while (lex->pos < lex->length) {
            char c = lexer_char(lex);
            if (c == '`') {
                lexer_advance(lex);
                found_close = true;
                break;
            }
            if (c == '\\') {
                lexer_advance(lex);
                c = lexer_char(lex);
                if (c == '$' || c == '`' || c == '\\') {
                    strbuf_append_byte(&content, c);
                    lexer_advance(lex);
                    continue;
                }
                strbuf_append_byte(&content, '\\');
                if (c != '\0') {
                    strbuf_append_byte(&content, c);
                    lexer_advance(lex);
                }
                continue;
            }
            strbuf_append_byte(&content, c);
            lexer_advance(lex);
        }
        if (!found_close) {
            lexer_error(lex, "missing closing backtick");
        }
    }

    wu->part.cmdsub.u.unparsed = strbuf_detach(&content);
    return wu;
}

/*
 * Parse a word, building a linked list of word_part_t nodes.
 * Handles quoting, expansions, and concatenation.
 *
 * If `in_dquote` is true, we are inside double quotes.
 */
static word_part_t *parse_word_units(lexer_t *lex, bool in_dquote, strbuf_t *raw)
{
    word_part_t *head = NULL;
    word_part_t **tail = &head;
    strbuf_t literal;
    strbuf_init(&literal);

    /* Flush accumulated literal text as a WP_LITERAL node */
#define FLUSH_LITERAL()                                                                            \
    do {                                                                                           \
        if (literal.length > 0) {                                                                  \
            word_part_t *wu = lexer_alloc(lex, sizeof(*wu));                                       \
            wu->type = WP_LITERAL;                                                                 \
            wu->part.string = strbuf_detach(&literal);                                             \
            *tail = wu;                                                                            \
            tail = &wu->next;                                                                      \
            strbuf_init(&literal);                                                                 \
        }                                                                                          \
    } while (0)

    while (lex->pos < lex->length) {
        char c = lexer_char(lex);

        if (in_dquote) {
            if (c == '"') {
                break; /* end of double-quoted section */
            }
            if (c == '\\') {
                lexer_advance(lex);
                if (raw) {
                    strbuf_append_byte(raw, '\\');
                }
                c = lexer_char(lex);
                if (c == '$' || c == '`' || c == '"' || c == '\\' || c == '\n') {
                    if (c != '\n') {
                        strbuf_append_byte(&literal, c);
                        if (raw) {
                            strbuf_append_byte(raw, c);
                        }
                    }
                    lexer_advance(lex);
                    continue;
                }
                strbuf_append_byte(&literal, '\\');
                if (c != '\0') {
                    strbuf_append_byte(&literal, c);
                    if (raw) {
                        strbuf_append_byte(raw, c);
                    }
                    lexer_advance(lex);
                }
                continue;
            }
            if (c == '$') {
                if (raw) {
                    strbuf_append_byte(raw, '$');
                }
                lexer_advance(lex);
                FLUSH_LITERAL();
                word_part_t *wu = parse_dollar(lex);
                *tail = wu;
                tail = &wu->next;
                continue;
            }
            if (c == '`') {
                if (raw) {
                    strbuf_append_byte(raw, '`');
                }
                lexer_advance(lex);
                FLUSH_LITERAL();
                word_part_t *wu = parse_backtick(lex);
                *tail = wu;
                tail = &wu->next;
                continue;
            }
            strbuf_append_byte(&literal, c);
            if (raw) {
                strbuf_append_byte(raw, c);
            }
            lexer_advance(lex);
            continue;
        }

        /* Not in double quotes */
        if (!is_word_char(c) && c != '\'' && c != '"' && c != '\\' && c != '$' && c != '`') {
            break;
        }

        if (c == '\'') {
            if (raw) {
                strbuf_append_byte(raw, '\'');
            }
            lexer_advance(lex);
            /* Single-quoted string: everything is literal until closing ' */
            {
                bool found_close = false;
                while (lex->pos < lex->length) {
                    c = lexer_char(lex);
                    if (c == '\'') {
                        if (raw) {
                            strbuf_append_byte(raw, '\'');
                        }
                        lexer_advance(lex);
                        found_close = true;
                        break;
                    }
                    strbuf_append_byte(&literal, c);
                    if (raw) {
                        strbuf_append_byte(raw, c);
                    }
                    lexer_advance(lex);
                }
                if (!found_close) {
                    lexer_error(lex, "missing closing single quote");
                }
            }
            continue;
        }

        if (c == '"') {
            if (raw) {
                strbuf_append_byte(raw, '"');
            }
            lexer_advance(lex);
            /* Recursively parse double-quoted content */
            word_part_t *dq = parse_word_units(lex, true, raw);
            if (lexer_char(lex) == '"') {
                if (raw) {
                    strbuf_append_byte(raw, '"');
                }
                lexer_advance(lex);
            } else {
                lexer_error(lex, "missing closing double quote");
            }
            /* Merge the double-quoted content */
            FLUSH_LITERAL();
            if (dq != NULL) {
                *tail = dq;
                while (*tail != NULL) {
                    tail = &(*tail)->next;
                }
            }
            continue;
        }

        if (c == '\\') {
            if (raw) {
                strbuf_append_byte(raw, '\\');
            }
            lexer_advance(lex);
            c = lexer_char(lex);
            if (c == '\n') {
                /* Line continuation */
                lexer_advance(lex);
                continue;
            }
            if (c != '\0') {
                strbuf_append_byte(&literal, c);
                if (raw) {
                    strbuf_append_byte(raw, c);
                }
                lexer_advance(lex);
            }
            continue;
        }

        if (c == '$') {
            if (raw) {
                strbuf_append_byte(raw, '$');
            }
            lexer_advance(lex);
            FLUSH_LITERAL();
            word_part_t *wu = parse_dollar(lex);
            *tail = wu;
            tail = &wu->next;
            continue;
        }

        if (c == '`') {
            if (raw) {
                strbuf_append_byte(raw, '`');
            }
            lexer_advance(lex);
            FLUSH_LITERAL();
            word_part_t *wu = parse_backtick(lex);
            *tail = wu;
            tail = &wu->next;
            continue;
        }

        /* Regular character */
        strbuf_append_byte(&literal, c);
        if (raw) {
            strbuf_append_byte(raw, c);
        }
        lexer_advance(lex);
    }

#undef FLUSH_LITERAL

    /* Flush any remaining literal */
    if (literal.length > 0) {
        word_part_t *wu = lexer_alloc(lex, sizeof(*wu));
        wu->type = WP_LITERAL;
        wu->part.string = strbuf_detach(&literal);
        *tail = wu;
    } else {
        strbuf_destroy(&literal);
    }

    return head;
}

static token_type_t check_reserved_word(const char *word)
{
    static const struct {
        const char *name;
        token_type_t type;
    } reserved[] = {
        {"if", TOK_IF},           {"then", TOK_THEN},
        {"else", TOK_ELSE},       {"elif", TOK_ELIF},
        {"fi", TOK_FI},           {"do", TOK_DO},
        {"done", TOK_DONE},       {"case", TOK_CASE},
        {"esac", TOK_ESAC},       {"while", TOK_WHILE},
        {"until", TOK_UNTIL},     {"for", TOK_FOR},
        {"in", TOK_IN},           {"function", TOK_FUNCTION},
        {"!", TOK_BANG},          {"[[", TOK_DBRACK_OPEN},
        {"]]", TOK_DBRACK_CLOSE}, {"{", TOK_LBRACE},
        {"}", TOK_RBRACE},        {NULL, TOK_EOF},
    };

    int i;
    for (i = 0; reserved[i].name != NULL; i++) {
        if (strcmp(word, reserved[i].name) == 0) {
            return reserved[i].type;
        }
    }
    return TOK_WORD;
}

static token_t make_token(token_type_t type, unsigned int lineno)
{
    token_t tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = type;
    tok.lineno = lineno;
    return tok;
}

/*
 * Check if a word is an assignment (contains = not at position 0, preceded by
 * valid name characters only).
 */
static bool is_assignment_word(const char *raw)
{
    const char *eq;
    const char *p;

    if (raw == NULL || raw[0] == '=' || raw[0] == '\0') {
        return false;
    }
    eq = strchr(raw, '=');
    if (eq == NULL) {
        return false;
    }
    /* Check that everything before = is a valid variable name */
    if (!isalpha((unsigned char)raw[0]) && raw[0] != '_') {
        return false;
    }
    for (p = raw + 1; p < eq; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            return false;
        }
    }
    return true;
}

static token_t lexer_read_token(lexer_t *lex)
{
    unsigned int lineno;
    char c, c2;
    token_t tok;

    lexer_skip_whitespace(lex);

    if (lex->pos >= lex->length) {
        return make_token(TOK_EOF, lex->lineno);
    }

    lineno = lex->lineno;
    c = lexer_char(lex);

    /* Newline */
    if (c == '\n') {
        lexer_advance(lex);
        return make_token(TOK_NEWLINE, lineno);
    }

    /* Operators */
    c2 = lexer_peek_char(lex, 1);

    if (c == '|') {
        lexer_advance(lex);
        if (c2 == '|') {
            lexer_advance(lex);
            return make_token(TOK_OR_IF, lineno);
        }
        return make_token(TOK_PIPE, lineno);
    }

    if (c == '&') {
        lexer_advance(lex);
        if (c2 == '&') {
            lexer_advance(lex);
            return make_token(TOK_AND_IF, lineno);
        }
        return make_token(TOK_AMP, lineno);
    }

    if (c == ';') {
        lexer_advance(lex);
        if (c2 == ';') {
            lexer_advance(lex);
            return make_token(TOK_DSEMI, lineno);
        }
        if (c2 == '&') {
            lexer_advance(lex);
            return make_token(TOK_SEMIAND, lineno);
        }
        if (c2 == '|') {
            lexer_advance(lex);
            return make_token(TOK_SEMIPIPE, lineno);
        }
        return make_token(TOK_SEMI, lineno);
    }

    if (c == '(') {
        lexer_advance(lex);
        return make_token(TOK_LPAREN, lineno);
    }

    if (c == ')') {
        lexer_advance(lex);
        return make_token(TOK_RPAREN, lineno);
    }

    if (c == '<') {
        lexer_advance(lex);
        if (c2 == '<') {
            lexer_advance(lex);
            char c3 = lexer_char(lex);
            if (c3 == '-') {
                lexer_advance(lex);
                return make_token(TOK_DLESSDASH, lineno);
            }
            if (c3 == '<') {
                lexer_advance(lex);
                return make_token(TOK_TLESS, lineno);
            }
            return make_token(TOK_DLESS, lineno);
        }
        if (c2 == '&') {
            lexer_advance(lex);
            return make_token(TOK_LESSAND, lineno);
        }
        if (c2 == '>') {
            lexer_advance(lex);
            return make_token(TOK_LESSGREAT, lineno);
        }
        return make_token(TOK_LESS, lineno);
    }

    if (c == '>') {
        lexer_advance(lex);
        if (c2 == '>') {
            lexer_advance(lex);
            return make_token(TOK_DGREAT, lineno);
        }
        if (c2 == '&') {
            lexer_advance(lex);
            return make_token(TOK_GREATAND, lineno);
        }
        if (c2 == '|') {
            lexer_advance(lex);
            return make_token(TOK_CLOBBER, lineno);
        }
        return make_token(TOK_GREAT, lineno);
    }

    /* Word (may include quoted strings, expansions, etc.) */
    {
        strbuf_t raw;
        strbuf_init(&raw);

        /* Check for IO_NUMBER: digits followed immediately by < or > */
        size_t save_pos = lex->pos;
        unsigned int save_lineno = lex->lineno;
        bool all_digits = true;

        while (lex->pos < lex->length) {
            c = lexer_char(lex);
            if (isdigit((unsigned char)c)) {
                strbuf_append_byte(&raw, c);
                lexer_advance(lex);
            } else {
                break;
            }
        }

        if (raw.length > 0 && all_digits) {
            c = lexer_char(lex);
            if (c == '<' || c == '>') {
                tok = make_token(TOK_IO_NUMBER, lineno);
                tok.value = strbuf_detach(&raw);
                return tok;
            }
        }

        /* Not an IO_NUMBER; restore position and parse as word */
        lex->pos = save_pos;
        lex->lineno = save_lineno;
        strbuf_clear(&raw);

        word_part_t *word = parse_word_units(lex, false, &raw);

        if (word == NULL) {
            strbuf_destroy(&raw);
            lexer_error(lex, "unexpected character");
            return make_token(TOK_EOF, lineno);
        }

        /* Check for reserved words */
        tok.type = TOK_WORD;
        tok.lineno = lineno;
        tok.value = strbuf_detach(&raw);
        tok.word = word;

        if (lex->recognize_reserved) {
            token_type_t rw = check_reserved_word(tok.value);
            if (rw != TOK_WORD) {
                tok.type = rw;
            }
        }

        /* Check for assignment */
        if (tok.type == TOK_WORD && is_assignment_word(tok.value)) {
            tok.type = TOK_ASSIGNMENT;
        }

        return tok;
    }
}

/*
 * Public API
 */

void lexer_init(lexer_t *lex, const char *source, const char *filename)
{
    memset(lex, 0, sizeof(*lex));
    lex->source = source;
    lex->length = strlen(source);
    lex->lineno = 1;
    lex->filename = filename;
    lex->recognize_reserved = true;
    lex->has_lookahead = false;
    lex->error_count = 0;
}

void lexer_destroy(lexer_t *lex)
{
    if (lex->has_lookahead) {
        token_free(&lex->lookahead);
        lex->has_lookahead = false;
    }
}

token_t lexer_next(lexer_t *lex)
{
    if (lex->has_lookahead) {
        lex->has_lookahead = false;
        return lex->lookahead;
    }
    return lexer_read_token(lex);
}

const token_t *lexer_peek(lexer_t *lex)
{
    if (!lex->has_lookahead) {
        lex->lookahead = lexer_read_token(lex);
        lex->has_lookahead = true;
    }
    return &lex->lookahead;
}

void token_free(token_t *tok)
{
    free(tok->value);
    tok->value = NULL;
    word_part_free(tok->word);
    tok->word = NULL;
}

bool token_is_word(const token_t *tok)
{
    return tok->type == TOK_WORD || tok->type == TOK_ASSIGNMENT;
}

const char *token_type_name(token_type_t type)
{
    switch (type) {
    case TOK_EOF:
        return "end of input";
    case TOK_NEWLINE:
        return "newline";
    case TOK_WORD:
        return "word";
    case TOK_IO_NUMBER:
        return "IO number";
    case TOK_ASSIGNMENT:
        return "assignment";
    case TOK_PIPE:
        return "'|'";
    case TOK_AND_IF:
        return "'&&'";
    case TOK_OR_IF:
        return "'||'";
    case TOK_SEMI:
        return "';'";
    case TOK_DSEMI:
        return "';;'";
    case TOK_SEMIAND:
        return "';&'";
    case TOK_SEMIPIPE:
        return "';|'";
    case TOK_AMP:
        return "'&'";
    case TOK_LPAREN:
        return "'('";
    case TOK_RPAREN:
        return "')'";
    case TOK_LBRACE:
        return "'{'";
    case TOK_RBRACE:
        return "'}'";
    case TOK_LESS:
        return "'<'";
    case TOK_GREAT:
        return "'>'";
    case TOK_DLESS:
        return "'<<'";
    case TOK_DGREAT:
        return "'>>'";
    case TOK_LESSAND:
        return "'<&'";
    case TOK_GREATAND:
        return "'>&'";
    case TOK_LESSGREAT:
        return "'<>'";
    case TOK_CLOBBER:
        return "'>|'";
    case TOK_TLESS:
        return "'<<<'";
    case TOK_DLESSDASH:
        return "'<<-'";
    case TOK_IF:
        return "'if'";
    case TOK_THEN:
        return "'then'";
    case TOK_ELSE:
        return "'else'";
    case TOK_ELIF:
        return "'elif'";
    case TOK_FI:
        return "'fi'";
    case TOK_DO:
        return "'do'";
    case TOK_DONE:
        return "'done'";
    case TOK_CASE:
        return "'case'";
    case TOK_ESAC:
        return "'esac'";
    case TOK_WHILE:
        return "'while'";
    case TOK_UNTIL:
        return "'until'";
    case TOK_FOR:
        return "'for'";
    case TOK_IN:
        return "'in'";
    case TOK_FUNCTION:
        return "'function'";
    case TOK_BANG:
        return "'!'";
    case TOK_DBRACK_OPEN:
        return "'[['";
    case TOK_DBRACK_CLOSE:
        return "']]'";
    }
    return "unknown";
}
