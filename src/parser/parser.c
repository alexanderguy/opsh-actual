#include "parser/parser.h"

#include "foundation/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void parser_error(parser_t *p, const char *msg)
{
    fprintf(stderr, "opsh: %s:%u: %s\n", p->filename, p->lexer.lineno, msg);
    if (p->error_count < MAX_PARSE_ERRORS) {
        parse_error_t *e = &p->errors[p->error_count];
        e->lineno = p->lexer.lineno;
        e->message = xmalloc(strlen(msg) + 1);
        strcpy(e->message, msg);
    }
    p->error_count++;
}

static void parser_error_unexpected(parser_t *p, const token_t *tok)
{
    if (tok->type == TOK_EOF) {
        parser_error(p, "unexpected end of input");
    } else if (tok->value != NULL) {
        char buf[256];
        snprintf(buf, sizeof(buf), "unexpected token '%s'", tok->value);
        parser_error(p, buf);
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "unexpected %s", token_type_name(tok->type));
        parser_error(p, buf);
    }
}

static token_t next(parser_t *p)
{
    return lexer_next(&p->lexer);
}

static const token_t *peek(parser_t *p)
{
    return lexer_peek(&p->lexer);
}

/* Consume and discard a token, freeing its resources */
static void consume(parser_t *p)
{
    token_t tok = next(p);
    token_free(&tok);
}

/* Expect a specific token type; consume and free it. Returns false on mismatch. */
static bool expect(parser_t *p, token_type_t type)
{
    token_t tok = next(p);
    if (tok.type != type) {
        char buf[256];
        snprintf(buf, sizeof(buf), "expected %s, got %s", token_type_name(type),
                 token_type_name(tok.type));
        parser_error(p, buf);
        token_free(&tok);
        return false;
    }
    token_free(&tok);
    return true;
}

/* Skip newlines */
static void skip_newlines(parser_t *p)
{
    while (peek(p)->type == TOK_NEWLINE) {
        consume(p);
    }
}

/* Check if token starts a command */
static bool is_command_start(token_type_t type)
{
    switch (type) {
    case TOK_WORD:
    case TOK_ASSIGNMENT:
    case TOK_IO_NUMBER:
    case TOK_BANG:
    case TOK_LPAREN:
    case TOK_LBRACE:
    case TOK_IF:
    case TOK_FOR:
    case TOK_WHILE:
    case TOK_UNTIL:
    case TOK_CASE:
    case TOK_FUNCTION:
    case TOK_DBRACK_OPEN:
        return true;
    default:
        return false;
    }
}

/* Check if a token type is a reserved word that can appear as a command argument.
 * After the first word in a simple command, reserved words are regular words. */
static bool is_word_token(token_type_t type)
{
    switch (type) {
    case TOK_IF:
    case TOK_THEN:
    case TOK_ELSE:
    case TOK_ELIF:
    case TOK_FI:
    case TOK_DO:
    case TOK_DONE:
    case TOK_CASE:
    case TOK_ESAC:
    case TOK_WHILE:
    case TOK_UNTIL:
    case TOK_FOR:
    case TOK_IN:
    case TOK_FUNCTION:
    case TOK_BANG:
    case TOK_DBRACK_OPEN:
    case TOK_DBRACK_CLOSE:
    case TOK_LBRACE:
    case TOK_RBRACE:
        return true;
    default:
        return false;
    }
}

/* Check if token is a redirection operator */
static bool is_redir_op(token_type_t type)
{
    switch (type) {
    case TOK_LESS:
    case TOK_GREAT:
    case TOK_DLESS:
    case TOK_DGREAT:
    case TOK_LESSAND:
    case TOK_GREATAND:
    case TOK_LESSGREAT:
    case TOK_CLOBBER:
    case TOK_TLESS:
    case TOK_DLESSDASH:
        return true;
    default:
        return false;
    }
}

static io_redir_type_t token_to_redir_type(token_type_t type)
{
    switch (type) {
    case TOK_LESS:
        return REDIR_IN;
    case TOK_GREAT:
        return REDIR_OUT;
    case TOK_DGREAT:
        return REDIR_APPEND;
    case TOK_CLOBBER:
        return REDIR_CLOBBER;
    case TOK_LESSGREAT:
        return REDIR_RDWR;
    case TOK_LESSAND:
        return REDIR_DUPIN;
    case TOK_GREATAND:
        return REDIR_DUPOUT;
    case TOK_DLESS:
        return REDIR_HEREDOC;
    case TOK_DLESSDASH:
        return REDIR_HEREDOC_STRIP;
    case TOK_TLESS:
        return REDIR_HERESTR;
    default:
        return REDIR_IN;
    }
}

/* Forward declarations */
static sh_list_t *parse_list(parser_t *p);
static sh_list_t *parse_sh_list(parser_t *p);
static and_or_t *parse_and_or(parser_t *p);
static command_t *parse_command(parser_t *p);
static command_t *parse_simple_command(parser_t *p);
static command_t *parse_if(parser_t *p);
static command_t *parse_for(parser_t *p);
static command_t *parse_while_until(parser_t *p, bool is_until);
static command_t *parse_case(parser_t *p);
static command_t *parse_brace_group(parser_t *p);
static command_t *parse_subshell(parser_t *p);
static command_t *parse_funcdef(parser_t *p);
static command_t *parse_bracket(parser_t *p);
static io_redir_t *parse_redirection(parser_t *p);
static void parse_redirections(parser_t *p, io_redir_t **redir_tail);
static cond_expr_t *parse_cond_expr(parser_t *p);
static cond_expr_t *parse_cond_expr_or(parser_t *p);
static cond_expr_t *parse_cond_expr_and(parser_t *p);
static cond_expr_t *parse_cond_expr_not(parser_t *p);
static cond_expr_t *parse_cond_expr_primary(parser_t *p);

/*
 * Parse a complete program: list of and-or commands separated by ; or & or newline.
 */
static sh_list_t *parse_list(parser_t *p)
{
    sh_list_t *head = NULL;
    sh_list_t **tail = &head;

    skip_newlines(p);

    while (is_command_start(peek(p)->type)) {
        sh_list_t *ao = parse_sh_list(p);
        if (ao == NULL) {
            break;
        }
        *tail = ao;
        tail = &ao->next;

        /* Check for separator */
        token_type_t sep = peek(p)->type;
        if (sep == TOK_SEMI || sep == TOK_AMP || sep == TOK_NEWLINE) {
            if (sep == TOK_AMP) {
                ao->background = true;
            }
            consume(p);
            skip_newlines(p);
        } else {
            break;
        }
    }

    return head;
}

/*
 * Parse an sh-list: pipeline ((&& | ||) pipeline)*
 */
static sh_list_t *parse_sh_list(parser_t *p)
{
    and_or_t *first_pl = parse_and_or(p);
    if (first_pl == NULL) {
        return NULL;
    }

    and_or_t *last_pl = first_pl;

    while (peek(p)->type == TOK_AND_IF || peek(p)->type == TOK_OR_IF) {
        bool is_and = (peek(p)->type == TOK_AND_IF);
        last_pl->connector = is_and;

        consume(p);
        skip_newlines(p);

        and_or_t *next_pl = parse_and_or(p);
        if (next_pl == NULL) {
            parser_error(p, "expected command after && or ||");
            break;
        }
        last_pl->next = next_pl;
        last_pl = next_pl;
    }

    sh_list_t *ao = xcalloc(1, sizeof(*ao));
    ao->pipelines = first_pl;
    ao->background = false;
    return ao;
}

/*
 * Parse a pipeline: [!] command (| command)*
 */
static and_or_t *parse_and_or(parser_t *p)
{
    bool negated = false;

    if (peek(p)->type == TOK_BANG) {
        consume(p);
        negated = true;
    }

    command_t *first_cmd = parse_command(p);
    if (first_cmd == NULL) {
        return NULL;
    }

    command_t **cmd_tail = &first_cmd->next;

    while (peek(p)->type == TOK_PIPE) {
        consume(p);
        skip_newlines(p);

        command_t *next_cmd = parse_command(p);
        if (next_cmd == NULL) {
            parser_error(p, "expected command after |");
            break;
        }
        *cmd_tail = next_cmd;
        cmd_tail = &next_cmd->next;
    }

    and_or_t *pl = xcalloc(1, sizeof(*pl));
    pl->commands = first_cmd;
    pl->negated = negated;
    return pl;
}

/*
 * Parse a single redirection.
 */
static io_redir_t *parse_redirection(parser_t *p)
{
    io_redir_t *rd = xcalloc(1, sizeof(*rd));
    token_t tok = next(p);

    rd->type = token_to_redir_type(tok.type);
    token_free(&tok);

    /* Default FD: 0 for input-like redirections, 1 for output-like.
     * Caller overrides this if an IO_NUMBER preceded the operator. */
    switch (rd->type) {
    case REDIR_IN:
    case REDIR_DUPIN:
    case REDIR_HEREDOC:
    case REDIR_HEREDOC_STRIP:
    case REDIR_HERESTR:
    case REDIR_RDWR:
        rd->fd = 0;
        break;
    default:
        rd->fd = 1;
        break;
    }

    /* Read the target word */
    if (rd->type == REDIR_HEREDOC || rd->type == REDIR_HEREDOC_STRIP) {
        /* Here-document delimiter */
        tok = next(p);
        if (!token_is_word(&tok)) {
            parser_error(p, "expected here-document delimiter");
            token_free(&tok);
            free(rd);
            return NULL;
        }
        /* Check if delimiter is quoted (any quoting disables expansion) */
        rd->heredoc_expand = true;
        if (tok.value != NULL) {
            const char *v = tok.value;
            while (*v) {
                if (*v == '\'' || *v == '"' || *v == '\\') {
                    rd->heredoc_expand = false;
                    break;
                }
                v++;
            }
        }
        rd->target = tok.word;
        tok.word = NULL;
        free(tok.value);
    } else {
        tok = next(p);
        if (!token_is_word(&tok)) {
            parser_error(p, "expected filename after redirection");
            token_free(&tok);
            free(rd);
            return NULL;
        }
        /* Check for <&- or >&- (close) */
        if ((rd->type == REDIR_DUPIN || rd->type == REDIR_DUPOUT) && tok.value != NULL &&
            strcmp(tok.value, "-") == 0) {
            rd->type = REDIR_CLOSE;
        }
        rd->target = tok.word;
        tok.word = NULL;
        free(tok.value);
    }

    return rd;
}

/*
 * Parse zero or more redirections, appending to the given tail pointer.
 */
static void parse_redirections(parser_t *p, io_redir_t **redir_tail)
{
    while (is_redir_op(peek(p)->type) || peek(p)->type == TOK_IO_NUMBER) {
        int fd = -1;

        if (peek(p)->type == TOK_IO_NUMBER) {
            token_t num_tok = next(p);
            fd = atoi(num_tok.value);
            token_free(&num_tok);
        }

        if (!is_redir_op(peek(p)->type)) {
            parser_error(p, "expected redirection operator");
            break;
        }

        io_redir_t *rd = parse_redirection(p);
        if (rd == NULL) {
            break;
        }
        if (fd >= 0) {
            rd->fd = fd;
        }
        *redir_tail = rd;
        redir_tail = &rd->next;
    }
}

/*
 * Parse a command (simple or compound).
 */
static command_t *parse_command(parser_t *p)
{
    token_type_t type = peek(p)->type;

    /* Check for compound commands */
    switch (type) {
    case TOK_IF:
        return parse_if(p);
    case TOK_FOR:
        return parse_for(p);
    case TOK_WHILE:
        return parse_while_until(p, false);
    case TOK_UNTIL:
        return parse_while_until(p, true);
    case TOK_CASE:
        return parse_case(p);
    case TOK_LBRACE:
        return parse_brace_group(p);
    case TOK_LPAREN:
        return parse_subshell(p);
    case TOK_FUNCTION:
        return parse_funcdef(p);
    case TOK_DBRACK_OPEN:
        return parse_bracket(p);
    default:
        break;
    }

    return parse_simple_command(p);
}

/*
 * Parse a simple command: assignments, words, and redirections.
 * Also detects function definitions (name followed by ()).
 */
static command_t *parse_simple_command(parser_t *p)
{
    command_t *cmd = xcalloc(1, sizeof(*cmd));
    cmd->type = CT_SIMPLE;
    cmd->lineno = peek(p)->lineno;
    refcount_init(&cmd->refcount);
    plist_init(&cmd->u.simple.assigns);
    plist_init(&cmd->u.simple.words);

    io_redir_t **redir_tail = &cmd->redirs;
    bool has_words = false;

    /* Parse redirections that appear before the command */
    parse_redirections(p, redir_tail);
    while (*redir_tail != NULL) {
        redir_tail = &(*redir_tail)->next;
    }

    while (peek(p)->type == TOK_ASSIGNMENT || peek(p)->type == TOK_WORD ||
           peek(p)->type == TOK_IO_NUMBER || is_redir_op(peek(p)->type) ||
           (has_words && is_word_token(peek(p)->type))) {

        if (is_redir_op(peek(p)->type) || peek(p)->type == TOK_IO_NUMBER) {
            parse_redirections(p, redir_tail);
            while (*redir_tail != NULL) {
                redir_tail = &(*redir_tail)->next;
            }
            continue;
        }

        token_t tok = next(p);

        if (tok.type == TOK_ASSIGNMENT && !has_words) {
            /* Assignment before any command word */
            plist_add(&cmd->u.simple.assigns, tok.word);
            tok.word = NULL;
            free(tok.value);
            continue;
        }

        /* Check for function definition: word ( ) */
        if (tok.type == TOK_WORD && !has_words && cmd->u.simple.assigns.length == 0) {
            if (peek(p)->type == TOK_LPAREN) {
                /* Save the function name */
                char *funcname = tok.value;
                tok.value = NULL;
                word_part_free(tok.word);
                tok.word = NULL;

                consume(p); /* ( */
                if (!expect(p, TOK_RPAREN)) {
                    free(funcname);
                    command_free(cmd);
                    return NULL;
                }

                skip_newlines(p);

                /* Parse the function body */
                command_t *body = parse_command(p);
                if (body == NULL) {
                    parser_error(p, "expected function body");
                    free(funcname);
                    command_free(cmd);
                    return NULL;
                }

                /* Convert cmd to a funcdef */
                plist_destroy(&cmd->u.simple.assigns);
                plist_destroy(&cmd->u.simple.words);
                cmd->type = CT_FUNCDEF;
                cmd->u.func_def.name = funcname;
                cmd->u.func_def.body = body;
                return cmd;
            }
        }

        /* Regular command word */
        plist_add(&cmd->u.simple.words, tok.word);
        tok.word = NULL;
        free(tok.value);
        has_words = true;

        /* After the first word, assignments become regular words */
        p->lexer.recognize_reserved = false;
    }

    p->lexer.recognize_reserved = true;

    if (cmd->u.simple.words.length == 0 && cmd->u.simple.assigns.length == 0 &&
        cmd->redirs == NULL) {
        command_free(cmd);
        return NULL;
    }

    return cmd;
}

/*
 * Parse an if command.
 */
static command_t *parse_if(parser_t *p)
{
    command_t *cmd = xcalloc(1, sizeof(*cmd));
    cmd->type = CT_IF;
    cmd->lineno = peek(p)->lineno;
    refcount_init(&cmd->refcount);

    if_clause_t **ic_tail = &cmd->u.if_clause.clauses;

    consume(p); /* 'if' */

    /* Parse if condition */
    {
        if_clause_t *ic = xcalloc(1, sizeof(*ic));
        ic->condition = parse_list(p);
        if (!expect(p, TOK_THEN)) {
            if_clause_free(ic);
            command_free(cmd);
            return NULL;
        }
        ic->body = parse_list(p);
        *ic_tail = ic;
        ic_tail = &ic->next;
    }

    /* Parse elif clauses */
    while (peek(p)->type == TOK_ELIF) {
        consume(p);
        if_clause_t *ic = xcalloc(1, sizeof(*ic));
        ic->condition = parse_list(p);
        if (!expect(p, TOK_THEN)) {
            if_clause_free(ic);
            command_free(cmd);
            return NULL;
        }
        ic->body = parse_list(p);
        *ic_tail = ic;
        ic_tail = &ic->next;
    }

    /* Parse else clause */
    if (peek(p)->type == TOK_ELSE) {
        consume(p);
        if_clause_t *ic = xcalloc(1, sizeof(*ic));
        ic->condition = NULL; /* marks this as else */
        ic->body = parse_list(p);
        *ic_tail = ic;
    }

    if (!expect(p, TOK_FI)) {
        command_free(cmd);
        return NULL;
    }

    /* Parse trailing redirections */
    io_redir_t **rt = &cmd->redirs;
    parse_redirections(p, rt);

    return cmd;
}

/*
 * Parse a for command.
 */
static command_t *parse_for(parser_t *p)
{
    command_t *cmd = xcalloc(1, sizeof(*cmd));
    cmd->type = CT_FOR;
    cmd->lineno = peek(p)->lineno;
    refcount_init(&cmd->refcount);
    plist_init(&cmd->u.for_clause.wordlist);

    consume(p); /* 'for' */

    /* Variable name */
    {
        token_t name_tok = next(p);
        if (name_tok.type != TOK_WORD) {
            parser_error(p, "expected variable name after 'for'");
            token_free(&name_tok);
            command_free(cmd);
            return NULL;
        }
        cmd->u.for_clause.varname = name_tok.value;
        name_tok.value = NULL;
        word_part_free(name_tok.word);
    }

    skip_newlines(p);

    /* Optional 'in word...' */
    if (peek(p)->type == TOK_IN) {
        consume(p);
        while (peek(p)->type == TOK_WORD || peek(p)->type == TOK_ASSIGNMENT) {
            token_t tok = next(p);
            plist_add(&cmd->u.for_clause.wordlist, tok.word);
            tok.word = NULL;
            free(tok.value);
        }
        /* Expect separator after word list */
        if (peek(p)->type == TOK_SEMI || peek(p)->type == TOK_NEWLINE) {
            consume(p);
        }
    } else if (peek(p)->type == TOK_SEMI) {
        consume(p);
    }

    skip_newlines(p);

    if (!expect(p, TOK_DO)) {
        command_free(cmd);
        return NULL;
    }

    cmd->u.for_clause.body = parse_list(p);

    if (!expect(p, TOK_DONE)) {
        command_free(cmd);
        return NULL;
    }

    io_redir_t **rt = &cmd->redirs;
    parse_redirections(p, rt);

    return cmd;
}

/*
 * Parse while/until.
 */
static command_t *parse_while_until(parser_t *p, bool is_until)
{
    command_t *cmd = xcalloc(1, sizeof(*cmd));
    cmd->type = is_until ? CT_UNTIL : CT_WHILE;
    cmd->lineno = peek(p)->lineno;
    refcount_init(&cmd->refcount);

    consume(p); /* 'while' or 'until' */

    cmd->u.while_clause.condition = parse_list(p);

    if (!expect(p, TOK_DO)) {
        command_free(cmd);
        return NULL;
    }

    cmd->u.while_clause.body = parse_list(p);

    if (!expect(p, TOK_DONE)) {
        command_free(cmd);
        return NULL;
    }

    io_redir_t **rt = &cmd->redirs;
    parse_redirections(p, rt);

    return cmd;
}

/*
 * Parse a case command.
 */
static command_t *parse_case(parser_t *p)
{
    command_t *cmd = xcalloc(1, sizeof(*cmd));
    cmd->type = CT_CASE;
    cmd->lineno = peek(p)->lineno;
    refcount_init(&cmd->refcount);

    consume(p); /* 'case' */

    /* Case word */
    {
        token_t tok = next(p);
        if (!token_is_word(&tok)) {
            parser_error(p, "expected word after 'case'");
            token_free(&tok);
            command_free(cmd);
            return NULL;
        }
        cmd->u.case_clause.subject = tok.word;
        tok.word = NULL;
        free(tok.value);
    }

    skip_newlines(p);

    if (!expect(p, TOK_IN)) {
        command_free(cmd);
        return NULL;
    }

    skip_newlines(p);

    /* Parse case items */
    case_item_t **ci_tail = &cmd->u.case_clause.items;

    while (peek(p)->type != TOK_ESAC && peek(p)->type != TOK_EOF) {
        case_item_t *ci = xcalloc(1, sizeof(*ci));
        plist_init(&ci->patterns);
        ci->terminator = CASE_BREAK;

        /* Optional leading ( */
        if (peek(p)->type == TOK_LPAREN) {
            consume(p);
        }

        /* Parse patterns separated by | */
        for (;;) {
            token_t tok = next(p);
            /* Allow reserved words as patterns */
            if (tok.word == NULL && tok.value == NULL) {
                parser_error(p, "expected pattern in case item");
                token_free(&tok);
                case_item_free(ci);
                command_free(cmd);
                return NULL;
            }
            if (tok.word != NULL) {
                plist_add(&ci->patterns, tok.word);
                tok.word = NULL;
            } else {
                /* Create a wordunit for the reserved word text */
                word_part_t *wu = arena_calloc(&p->arena, sizeof(*wu));
                wu->type = WP_LITERAL;
                wu->part.string = xmalloc(strlen(tok.value) + 1);
                strcpy(wu->part.string, tok.value);
                plist_add(&ci->patterns, wu);
            }
            free(tok.value);

            if (peek(p)->type == TOK_PIPE) {
                consume(p);
                continue;
            }
            break;
        }

        if (!expect(p, TOK_RPAREN)) {
            case_item_free(ci);
            command_free(cmd);
            return NULL;
        }

        ci->body = parse_list(p);

        /* Check for terminator */
        token_type_t term = peek(p)->type;
        if (term == TOK_DSEMI) {
            ci->terminator = CASE_BREAK;
            consume(p);
        } else if (term == TOK_SEMIAND) {
            ci->terminator = CASE_FALLTHROUGH;
            consume(p);
        } else if (term == TOK_SEMIPIPE) {
            ci->terminator = CASE_CONTINUE;
            consume(p);
        }

        *ci_tail = ci;
        ci_tail = &ci->next;

        skip_newlines(p);
    }

    if (!expect(p, TOK_ESAC)) {
        command_free(cmd);
        return NULL;
    }

    io_redir_t **rt = &cmd->redirs;
    parse_redirections(p, rt);

    return cmd;
}

/*
 * Parse { list }
 */
static command_t *parse_brace_group(parser_t *p)
{
    command_t *cmd = xcalloc(1, sizeof(*cmd));
    cmd->type = CT_GROUP;
    cmd->lineno = peek(p)->lineno;
    refcount_init(&cmd->refcount);

    consume(p); /* { */

    cmd->u.group.body = parse_list(p);

    if (!expect(p, TOK_RBRACE)) {
        command_free(cmd);
        return NULL;
    }

    io_redir_t **rt = &cmd->redirs;
    parse_redirections(p, rt);

    return cmd;
}

/*
 * Parse ( list )
 */
static command_t *parse_subshell(parser_t *p)
{
    command_t *cmd = xcalloc(1, sizeof(*cmd));
    cmd->type = CT_SUBSHELL;
    cmd->lineno = peek(p)->lineno;
    refcount_init(&cmd->refcount);

    consume(p); /* ( */

    cmd->u.group.body = parse_list(p);

    if (!expect(p, TOK_RPAREN)) {
        command_free(cmd);
        return NULL;
    }

    io_redir_t **rt = &cmd->redirs;
    parse_redirections(p, rt);

    return cmd;
}

/*
 * Parse function definition: function name { ... } or function name () { ... }
 */
static command_t *parse_funcdef(parser_t *p)
{
    command_t *cmd = xcalloc(1, sizeof(*cmd));
    cmd->type = CT_FUNCDEF;
    cmd->lineno = peek(p)->lineno;
    refcount_init(&cmd->refcount);

    consume(p); /* 'function' */

    /* Function name */
    {
        token_t tok = next(p);
        if (tok.type != TOK_WORD) {
            parser_error(p, "expected function name");
            token_free(&tok);
            command_free(cmd);
            return NULL;
        }
        cmd->u.func_def.name = tok.value;
        tok.value = NULL;
        word_part_free(tok.word);
    }

    /* Optional () */
    if (peek(p)->type == TOK_LPAREN) {
        consume(p);
        expect(p, TOK_RPAREN);
    }

    skip_newlines(p);

    /* Function body */
    cmd->u.func_def.body = parse_command(p);
    if (cmd->u.func_def.body == NULL) {
        parser_error(p, "expected function body");
        command_free(cmd);
        return NULL;
    }

    return cmd;
}

/*
 * Parse [[ expression ]]
 */
static command_t *parse_bracket(parser_t *p)
{
    command_t *cmd = xcalloc(1, sizeof(*cmd));
    cmd->type = CT_BRACKET;
    cmd->lineno = peek(p)->lineno;
    refcount_init(&cmd->refcount);

    consume(p); /* [[ */

    bool saved_reserved = p->lexer.recognize_reserved;
    p->lexer.recognize_reserved = false;

    cmd->u.cond.expr = parse_cond_expr(p);

    p->lexer.recognize_reserved = saved_reserved;

    /* Expect ]] -- need to handle it specially since reserved words are off */
    {
        token_t tok = next(p);
        if (tok.value == NULL || strcmp(tok.value, "]]") != 0) {
            parser_error(p, "expected ']]'");
            token_free(&tok);
            command_free(cmd);
            return NULL;
        }
        token_free(&tok);
    }

    io_redir_t **rt = &cmd->redirs;
    parse_redirections(p, rt);

    return cmd;
}

/*
 * Double-bracket expression parsing.
 * Precedence: || (lowest), &&, ! (highest among composites), then primaries.
 */

static cond_expr_t *parse_cond_expr(parser_t *p)
{
    return parse_cond_expr_or(p);
}

static cond_expr_t *parse_cond_expr_or(parser_t *p)
{
    cond_expr_t *left = parse_cond_expr_and(p);
    if (left == NULL) {
        return NULL;
    }

    while (peek(p)->type == TOK_OR_IF) {
        consume(p);
        cond_expr_t *right = parse_cond_expr_and(p);
        if (right == NULL) {
            parser_error(p, "expected expression after ||");
            cond_expr_free(left);
            return NULL;
        }
        cond_expr_t *node = xcalloc(1, sizeof(*node));
        node->type = COND_OR;
        node->u.andor.left = left;
        node->u.andor.right = right;
        left = node;
    }

    return left;
}

static cond_expr_t *parse_cond_expr_and(parser_t *p)
{
    cond_expr_t *left = parse_cond_expr_not(p);
    if (left == NULL) {
        return NULL;
    }

    while (peek(p)->type == TOK_AND_IF) {
        consume(p);
        cond_expr_t *right = parse_cond_expr_not(p);
        if (right == NULL) {
            parser_error(p, "expected expression after &&");
            cond_expr_free(left);
            return NULL;
        }
        cond_expr_t *node = xcalloc(1, sizeof(*node));
        node->type = COND_AND;
        node->u.andor.left = left;
        node->u.andor.right = right;
        left = node;
    }

    return left;
}

static cond_expr_t *parse_cond_expr_not(parser_t *p)
{
    if ((peek(p)->type == TOK_WORD || peek(p)->type == TOK_BANG) && peek(p)->value != NULL &&
        strcmp(peek(p)->value, "!") == 0) {
        consume(p);
        cond_expr_t *child = parse_cond_expr_not(p);
        if (child == NULL) {
            parser_error(p, "expected expression after !");
            return NULL;
        }
        cond_expr_t *node = xcalloc(1, sizeof(*node));
        node->type = COND_NOT;
        node->u.not.child = child;
        return node;
    }

    return parse_cond_expr_primary(p);
}

static bool is_unary_test_op(const char *s)
{
    if (s == NULL || s[0] != '-' || s[1] == '\0' || s[2] != '\0') {
        return false;
    }
    return s[1] == 'f' || s[1] == 'd' || s[1] == 'e' || s[1] == 'r' || s[1] == 'w' || s[1] == 'x' ||
           s[1] == 's' || s[1] == 'L' || s[1] == 'n' || s[1] == 'z' || s[1] == 'b' || s[1] == 'c' ||
           s[1] == 'p' || s[1] == 'h' || s[1] == 'S' || s[1] == 'g' || s[1] == 'u' || s[1] == 'k' ||
           s[1] == 'O' || s[1] == 'G' || s[1] == 'N' || s[1] == 't';
}

static bool is_binary_test_op(const char *s)
{
    if (s == NULL) {
        return false;
    }
    return strcmp(s, "==") == 0 || strcmp(s, "!=") == 0 || strcmp(s, "<") == 0 ||
           strcmp(s, ">") == 0 || strcmp(s, "-eq") == 0 || strcmp(s, "-ne") == 0 ||
           strcmp(s, "-lt") == 0 || strcmp(s, "-gt") == 0 || strcmp(s, "-le") == 0 ||
           strcmp(s, "-ge") == 0 || strcmp(s, "=~") == 0 || strcmp(s, "=") == 0 ||
           strcmp(s, "-nt") == 0 || strcmp(s, "-ot") == 0 || strcmp(s, "-ef") == 0;
}

static cond_expr_t *parse_cond_expr_primary(parser_t *p)
{
    const token_t *peeked = peek(p);

    /* End of [[ ]] expression */
    if (peeked->value != NULL && strcmp(peeked->value, "]]") == 0) {
        return NULL;
    }
    if (peeked->type == TOK_EOF) {
        return NULL;
    }

    /* Parenthesized expression */
    if (peeked->value != NULL && strcmp(peeked->value, "(") == 0) {
        consume(p);
        cond_expr_t *inner = parse_cond_expr(p);
        token_t tok = next(p);
        if (tok.value == NULL || strcmp(tok.value, ")") != 0) {
            parser_error(p, "expected ')' in [[ ]] expression");
            token_free(&tok);
            cond_expr_free(inner);
            return NULL;
        }
        token_free(&tok);
        return inner;
    }

    /* Unary operators */
    if (peeked->value != NULL && is_unary_test_op(peeked->value)) {
        token_t op_tok = next(p);
        token_t arg_tok = next(p);

        cond_expr_t *node = xcalloc(1, sizeof(*node));
        node->type = COND_UNARY;
        node->u.unary.op = op_tok.value;
        op_tok.value = NULL;
        word_part_free(op_tok.word);
        node->u.unary.arg = arg_tok.word;
        arg_tok.word = NULL;
        free(arg_tok.value);
        return node;
    }

    /* Must be a word -- could be binary op or bare string */
    {
        token_t left_tok = next(p);
        if (left_tok.type == TOK_EOF ||
            (left_tok.value != NULL && strcmp(left_tok.value, "]]") == 0)) {
            token_free(&left_tok);
            return NULL;
        }

        /* Check if next token is a binary operator */
        const token_t *maybe_op = peek(p);
        if (maybe_op->value != NULL && is_binary_test_op(maybe_op->value)) {
            token_t op_tok = next(p);
            token_t right_tok = next(p);

            cond_expr_t *node = xcalloc(1, sizeof(*node));
            node->type = COND_BINARY;
            node->u.binary.op = op_tok.value;
            op_tok.value = NULL;
            word_part_free(op_tok.word);
            node->u.binary.left = left_tok.word;
            left_tok.word = NULL;
            free(left_tok.value);
            node->u.binary.right = right_tok.word;
            right_tok.word = NULL;
            free(right_tok.value);
            return node;
        }

        /* Bare string (equivalent to -n test) */
        cond_expr_t *node = xcalloc(1, sizeof(*node));
        node->type = COND_STRING;
        node->u.string.word = left_tok.word;
        left_tok.word = NULL;
        free(left_tok.value);
        return node;
    }
}

/*
 * Public API
 */

void parser_init(parser_t *p, const char *source, const char *filename)
{
    memset(p, 0, sizeof(*p));
    arena_init(&p->arena);
    lexer_init(&p->lexer, source, filename);
    p->lexer.arena = &p->arena;
    p->filename = filename;
    p->error_count = 0;
}

void parser_destroy(parser_t *p)
{
    lexer_destroy(&p->lexer);
    arena_destroy(&p->arena);
    {
        int i;
        int n = p->error_count < MAX_PARSE_ERRORS ? p->error_count : MAX_PARSE_ERRORS;
        for (i = 0; i < n; i++) {
            free(p->errors[i].message);
        }
    }
}

sh_list_t *parser_parse(parser_t *p)
{
    sh_list_t *result = parse_list(p);
    if (peek(p)->type != TOK_EOF) {
        parser_error_unexpected(p, peek(p));
    }
    return result;
}

int parser_error_count(const parser_t *p)
{
    return p->error_count + p->lexer.error_count;
}

comment_t *parser_take_comments(parser_t *p)
{
    comment_t *comments = p->lexer.comments;
    p->lexer.comments = NULL;
    p->lexer.comments_tail = &p->lexer.comments;
    return comments;
}
