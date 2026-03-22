#include "format/format.h"

#include "foundation/util.h"
#include "parser/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Formatter state */
typedef struct {
    strbuf_t *out;
    const format_options_t *opts;
    int depth;
    const comment_t *next_comment; /* next comment to emit (walks the list) */
} fmt_t;

static void emit_indent(fmt_t *f)
{
    int i;
    if (f->opts->indent_width == 0) {
        for (i = 0; i < f->depth; i++) {
            strbuf_append_byte(f->out, '\t');
        }
    } else {
        int spaces = f->depth * f->opts->indent_width;
        for (i = 0; i < spaces; i++) {
            strbuf_append_byte(f->out, ' ');
        }
    }
}

static void emit(fmt_t *f, const char *s)
{
    strbuf_append_str(f->out, s);
}

static void emit_char(fmt_t *f, char c)
{
    strbuf_append_byte(f->out, c);
}

static void emit_newline(fmt_t *f)
{
    strbuf_append_byte(f->out, '\n');
}

/* Emit all comments with lineno < before_line, indented at current depth */
static void emit_comments_before(fmt_t *f, unsigned int before_line)
{
    while (f->next_comment != NULL && f->next_comment->lineno < before_line) {
        emit_indent(f);
        strbuf_append_str(f->out, f->next_comment->text);
        strbuf_append_byte(f->out, '\n');
        f->next_comment = f->next_comment->next;
    }
}

/* Emit a trailing comment on the same line if it matches the given lineno */
static void emit_trailing_comment(fmt_t *f, unsigned int lineno)
{
    if (f->next_comment != NULL && f->next_comment->lineno == lineno) {
        strbuf_append_byte(f->out, ' ');
        strbuf_append_str(f->out, f->next_comment->text);
        f->next_comment = f->next_comment->next;
    }
}

/* Emit all remaining comments */
static void emit_remaining_comments(fmt_t *f)
{
    while (f->next_comment != NULL) {
        emit_indent(f);
        strbuf_append_str(f->out, f->next_comment->text);
        strbuf_append_byte(f->out, '\n');
        f->next_comment = f->next_comment->next;
    }
}

/* Forward declarations */
static void format_sh_list(fmt_t *f, const sh_list_t *ao);
static void format_command(fmt_t *f, const command_t *cmd);
static void format_word(fmt_t *f, const word_part_t *w);
static void format_redir(fmt_t *f, const io_redir_t *r);
static void format_cond_expr(fmt_t *f, const cond_expr_t *d);

/* Get the source line number of the first command in an sh_list_t */
static unsigned int sh_list_lineno(const sh_list_t *ao)
{
    if (ao != NULL && ao->pipelines != NULL && ao->pipelines->commands != NULL) {
        return ao->pipelines->commands->lineno;
    }
    return 0;
}

/*
 * Word unit formatting
 */
static void format_param_exp(fmt_t *f, const param_exp_t *pe)
{
    if (pe->flags & PE_STRLEN) {
        emit(f, "${#");
        emit(f, pe->name);
        emit_char(f, '}');
        return;
    }

    if (pe->type == PE_NONE && pe->pattern == NULL && pe->replacement == NULL) {
        /* Simple ${var} — use short form $var if name is simple */
        bool simple = true;
        const char *c;
        for (c = pe->name; *c != '\0'; c++) {
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '_')) {
                simple = false;
                break;
            }
        }
        /* Special parameters: $?, $$, $!, $#, $@, $*, $-, $0-$9 */
        if (!simple && pe->name[0] != '\0' && pe->name[1] == '\0') {
            char ch = pe->name[0];
            if (ch == '?' || ch == '$' || ch == '!' || ch == '#' || ch == '@' || ch == '*' ||
                ch == '-' || (ch >= '0' && ch <= '9')) {
                simple = true;
            }
        }
        if (simple && pe->name[0] != '\0') {
            emit_char(f, '$');
            emit(f, pe->name);
            return;
        }
        emit(f, "${");
        emit(f, pe->name);
        emit_char(f, '}');
        return;
    }

    emit(f, "${");
    emit(f, pe->name);

    switch (pe->type) {
    case PE_NONE:
        break;
    case PE_DEFAULT:
        if (pe->flags & PE_COLON) {
            emit(f, ":-");
        } else {
            emit_char(f, '-');
        }
        if (pe->pattern != NULL) {
            format_word(f, pe->pattern);
        }
        break;
    case PE_ALTERNATE:
        if (pe->flags & PE_COLON) {
            emit(f, ":+");
        } else {
            emit_char(f, '+');
        }
        if (pe->pattern != NULL) {
            format_word(f, pe->pattern);
        }
        break;
    case PE_ASSIGN:
        if (pe->flags & PE_COLON) {
            emit(f, ":=");
        } else {
            emit_char(f, '=');
        }
        if (pe->pattern != NULL) {
            format_word(f, pe->pattern);
        }
        break;
    case PE_ERROR:
        if (pe->flags & PE_COLON) {
            emit(f, ":?");
        } else {
            emit_char(f, '?');
        }
        if (pe->pattern != NULL) {
            format_word(f, pe->pattern);
        }
        break;
    case PE_TRIM:
        if (pe->flags & PE_SUFFIX) {
            if (pe->flags & PE_LONGEST) {
                emit(f, "%%");
            } else {
                emit_char(f, '%');
            }
        } else {
            if (pe->flags & PE_LONGEST) {
                emit(f, "##");
            } else {
                emit_char(f, '#');
            }
        }
        if (pe->pattern != NULL) {
            format_word(f, pe->pattern);
        }
        break;
    case PE_REPLACE:
        if (pe->flags & PE_GLOBAL) {
            emit(f, "//");
        } else {
            emit_char(f, '/');
        }
        if (pe->pattern != NULL) {
            format_word(f, pe->pattern);
        }
        if (pe->replacement != NULL) {
            emit_char(f, '/');
            format_word(f, pe->replacement);
        }
        break;
    }

    emit_char(f, '}');
}

/* Emit a single word unit (without quote wrapping) */
static void format_word_unit(fmt_t *f, const word_part_t *w)
{
    switch (w->type) {
    case WP_LITERAL:
        if (w->part.string != NULL) {
            emit(f, w->part.string);
        }
        break;
    case WP_PARAM:
        format_param_exp(f, w->part.param);
        break;
    case WP_CMDSUB:
        /* Always emit $(...) form, normalizing backticks to modern syntax */
        if (w->part.cmdsub.is_preparsed) {
            emit(f, "$(");
            format_sh_list(f, w->part.cmdsub.u.preparsed);
        } else {
            emit(f, "$(");
            if (w->part.cmdsub.u.unparsed != NULL) {
                emit(f, w->part.cmdsub.u.unparsed);
            }
        }
        emit_char(f, ')');
        break;
    case WP_ARITH:
        /*
         * The parser stores arithmetic content with one trailing ')' from
         * the closing '))', so we only add a single ')' here.
         */
        emit(f, "$((");
        format_word(f, w->part.arith);
        emit_char(f, ')');
        break;
    }
}

static void format_word(fmt_t *f, const word_part_t *w)
{
    bool in_quote = false;

    while (w != NULL) {
        if (w->quoted && !in_quote) {
            emit_char(f, '"');
            in_quote = true;
        } else if (!w->quoted && in_quote) {
            emit_char(f, '"');
            in_quote = false;
        }
        format_word_unit(f, w);
        w = w->next;
    }

    if (in_quote) {
        emit_char(f, '"');
    }
}

/*
 * Redirection formatting
 */
static void format_redir(fmt_t *f, const io_redir_t *r)
{
    while (r != NULL) {
        emit_char(f, ' ');

        /* Print fd number if non-default */
        bool print_fd = false;
        switch (r->type) {
        case REDIR_IN:
        case REDIR_RDWR:
        case REDIR_DUPIN:
        case REDIR_HEREDOC:
        case REDIR_HEREDOC_STRIP:
        case REDIR_HERESTR:
            print_fd = (r->fd != 0);
            break;
        case REDIR_OUT:
        case REDIR_APPEND:
        case REDIR_CLOBBER:
        case REDIR_DUPOUT:
            print_fd = (r->fd != 1);
            break;
        case REDIR_CLOSE:
            print_fd = true;
            break;
        }
        if (print_fd) {
            strbuf_append_printf(f->out, "%d", r->fd);
        }

        switch (r->type) {
        case REDIR_IN:
            emit(f, "< ");
            format_word(f, r->target);
            break;
        case REDIR_OUT:
            emit(f, "> ");
            format_word(f, r->target);
            break;
        case REDIR_APPEND:
            emit(f, ">> ");
            format_word(f, r->target);
            break;
        case REDIR_CLOBBER:
            emit(f, ">| ");
            format_word(f, r->target);
            break;
        case REDIR_RDWR:
            emit(f, "<> ");
            format_word(f, r->target);
            break;
        case REDIR_DUPIN:
            emit(f, "<&");
            format_word(f, r->target);
            break;
        case REDIR_DUPOUT:
            emit(f, ">&");
            format_word(f, r->target);
            break;
        case REDIR_CLOSE:
            /* fd tells us which direction */
            if (r->fd == 0) {
                emit(f, "<&-");
            } else {
                emit(f, ">&-");
            }
            break;
        case REDIR_HEREDOC:
            emit(f, "<< ");
            format_word(f, r->target);
            break;
        case REDIR_HEREDOC_STRIP:
            emit(f, "<<- ");
            format_word(f, r->target);
            break;
        case REDIR_HERESTR:
            emit(f, "<<< ");
            format_word(f, r->target);
            break;
        }
        r = r->next;
    }
}

/*
 * Double-bracket expression formatting
 */
static void format_cond_expr(fmt_t *f, const cond_expr_t *d)
{
    if (d == NULL) {
        return;
    }
    switch (d->type) {
    case COND_AND:
        format_cond_expr(f, d->u.andor.left);
        emit(f, " && ");
        format_cond_expr(f, d->u.andor.right);
        break;
    case COND_OR:
        format_cond_expr(f, d->u.andor.left);
        emit(f, " || ");
        format_cond_expr(f, d->u.andor.right);
        break;
    case COND_NOT:
        emit(f, "! ");
        format_cond_expr(f, d->u.not.child);
        break;
    case COND_UNARY:
        emit(f, d->u.unary.op);
        emit_char(f, ' ');
        format_word(f, d->u.unary.arg);
        break;
    case COND_BINARY:
        format_word(f, d->u.binary.left);
        emit_char(f, ' ');
        emit(f, d->u.binary.op);
        emit_char(f, ' ');
        format_word(f, d->u.binary.right);
        break;
    case COND_STRING:
        format_word(f, d->u.string.word);
        break;
    }
}

/*
 * Command formatting
 */
static void format_simple_command(fmt_t *f, const command_t *cmd)
{
    bool first = true;
    size_t i;

    /* Assignments */
    for (i = 0; i < cmd->u.simple.assigns.length; i++) {
        if (!first) {
            emit_char(f, ' ');
        }
        format_word(f, plist_get(&cmd->u.simple.assigns, i));
        first = false;
    }

    /* Command words */
    for (i = 0; i < cmd->u.simple.words.length; i++) {
        if (!first) {
            emit_char(f, ' ');
        }
        format_word(f, plist_get(&cmd->u.simple.words, i));
        first = false;
    }

    format_redir(f, cmd->redirs);
}

/* Format a body (list of and-or commands) at the current indentation + 1. */
static void format_body(fmt_t *f, const sh_list_t *body)
{
    f->depth++;
    const sh_list_t *ao = body;
    while (ao != NULL) {
        unsigned int line = sh_list_lineno(ao);
        if (line > 0) {
            emit_comments_before(f, line);
        }
        emit_indent(f);
        format_sh_list(f, ao);
        if (line > 0) {
            emit_trailing_comment(f, line);
        }
        emit_newline(f);
        ao = ao->next;
    }
    f->depth--;
}

static void format_if_command(fmt_t *f, const command_t *cmd)
{
    const if_clause_t *ic = cmd->u.if_clause.clauses;
    bool first = true;

    while (ic != NULL) {
        if (ic->condition != NULL) {
            if (first) {
                emit(f, "if ");
                first = false;
            } else {
                emit_indent(f);
                emit(f, "elif ");
            }
            format_sh_list(f, ic->condition);
            emit(f, "; then");
            emit_newline(f);
        } else {
            emit_indent(f);
            emit(f, "else");
            emit_newline(f);
        }
        format_body(f, ic->body);
        ic = ic->next;
    }
    emit_indent(f);
    emit(f, "fi");
    format_redir(f, cmd->redirs);
}

static void format_for_command(fmt_t *f, const command_t *cmd)
{
    emit(f, "for ");
    emit(f, cmd->u.for_clause.varname);

    if (cmd->u.for_clause.wordlist.length > 0) {
        emit(f, " in");
        size_t i;
        for (i = 0; i < cmd->u.for_clause.wordlist.length; i++) {
            emit_char(f, ' ');
            format_word(f, plist_get(&cmd->u.for_clause.wordlist, i));
        }
    }

    emit(f, "; do");
    emit_newline(f);
    format_body(f, cmd->u.for_clause.body);
    emit_indent(f);
    emit(f, "done");
    format_redir(f, cmd->redirs);
}

static void format_while_command(fmt_t *f, const command_t *cmd, const char *keyword)
{
    emit(f, keyword);
    emit_char(f, ' ');
    format_sh_list(f, cmd->u.while_clause.condition);
    emit(f, "; do");
    emit_newline(f);
    format_body(f, cmd->u.while_clause.body);
    emit_indent(f);
    emit(f, "done");
    format_redir(f, cmd->redirs);
}

static void format_case_command(fmt_t *f, const command_t *cmd)
{
    emit(f, "case ");
    format_word(f, cmd->u.case_clause.subject);
    emit(f, " in");
    emit_newline(f);

    const case_item_t *ci = cmd->u.case_clause.items;
    while (ci != NULL) {
        emit_indent(f);
        emit_char(f, '(');
        size_t i;
        for (i = 0; i < ci->patterns.length; i++) {
            if (i > 0) {
                emit(f, " | ");
            }
            format_word(f, plist_get(&ci->patterns, i));
        }
        emit_char(f, ')');
        emit_newline(f);

        if (ci->body != NULL) {
            format_body(f, ci->body);
        }

        emit_indent(f);
        switch (ci->terminator) {
        case CASE_BREAK:
            emit(f, ";;");
            break;
        case CASE_FALLTHROUGH:
            emit(f, ";&");
            break;
        case CASE_CONTINUE:
            emit(f, ";|");
            break;
        }
        emit_newline(f);
        ci = ci->next;
    }
    emit_indent(f);
    emit(f, "esac");
    format_redir(f, cmd->redirs);
}

static void format_funcdef(fmt_t *f, const command_t *cmd)
{
    emit(f, cmd->u.func_def.name);
    emit(f, "() ");

    /* The function body is typically a group command */
    format_command(f, cmd->u.func_def.body);
    format_redir(f, cmd->redirs);
}

static void format_group(fmt_t *f, const command_t *cmd)
{
    emit_char(f, '{');
    emit_newline(f);
    format_body(f, cmd->u.group.body);
    emit_indent(f);
    emit_char(f, '}');
    format_redir(f, cmd->redirs);
}

static void format_subshell(fmt_t *f, const command_t *cmd)
{
    emit_char(f, '(');
    emit_newline(f);
    format_body(f, cmd->u.group.body);
    emit_indent(f);
    emit_char(f, ')');
    format_redir(f, cmd->redirs);
}

static void format_bracket(fmt_t *f, const command_t *cmd)
{
    emit(f, "[[ ");
    format_cond_expr(f, cmd->u.cond.expr);
    emit(f, " ]]");
    format_redir(f, cmd->redirs);
}

static void format_command(fmt_t *f, const command_t *cmd)
{
    switch (cmd->type) {
    case CT_SIMPLE:
        format_simple_command(f, cmd);
        break;
    case CT_GROUP:
        format_group(f, cmd);
        break;
    case CT_SUBSHELL:
        format_subshell(f, cmd);
        break;
    case CT_IF:
        format_if_command(f, cmd);
        break;
    case CT_FOR:
        format_for_command(f, cmd);
        break;
    case CT_WHILE:
        format_while_command(f, cmd, "while");
        break;
    case CT_UNTIL:
        format_while_command(f, cmd, "until");
        break;
    case CT_CASE:
        format_case_command(f, cmd);
        break;
    case CT_FUNCDEF:
        format_funcdef(f, cmd);
        break;
    case CT_BRACKET:
        format_bracket(f, cmd);
        break;
    }
}

/*
 * Pipeline formatting
 */
static void format_and_or(fmt_t *f, const and_or_t *pl)
{
    if (pl->negated) {
        emit(f, "! ");
    }

    const command_t *cmd = pl->commands;
    bool first = true;
    while (cmd != NULL) {
        if (!first) {
            emit(f, " | ");
        }
        format_command(f, cmd);
        first = false;
        cmd = cmd->next;
    }
}

/*
 * And-or list formatting (single sh_list_t node — does NOT follow ->next)
 */
static void format_sh_list(fmt_t *f, const sh_list_t *ao)
{
    if (ao == NULL) {
        return;
    }

    const and_or_t *pl = ao->pipelines;
    while (pl != NULL) {
        format_and_or(f, pl);
        if (pl->next != NULL) {
            /* connector on this node is the connector to the next: true=&&, false=|| */
            if (pl->connector) {
                emit(f, " && ");
            } else {
                emit(f, " || ");
            }
        }
        pl = pl->next;
    }

    if (ao->background) {
        emit(f, " &");
    }
}

/*
 * Public API
 */
void format_ast(strbuf_t *out, const sh_list_t *ast, const comment_t *comments,
                const format_options_t *opts)
{
    fmt_t f;
    f.out = out;
    f.opts = opts;
    f.depth = 0;
    f.next_comment = comments;

    const sh_list_t *ao = ast;
    while (ao != NULL) {
        unsigned int line = sh_list_lineno(ao);
        if (line > 0) {
            emit_comments_before(&f, line);
        }
        format_sh_list(&f, ao);
        if (line > 0) {
            emit_trailing_comment(&f, line);
        }
        emit_newline(&f);
        ao = ao->next;
    }

    /* Emit any trailing comments (after all commands) */
    emit_remaining_comments(&f);
}

/*
 * CLI helpers
 */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = xmalloc((size_t)len + 1);
    size_t nread = fread(buf, 1, (size_t)len, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

static char *read_stdin(void)
{
    strbuf_t buf;
    strbuf_init(&buf);
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), stdin)) > 0) {
        strbuf_append_bytes(&buf, tmp, n);
    }
    return strbuf_detach(&buf);
}

/* Format a single source string. Returns formatted string (caller frees), or NULL on parse error.
 */
static char *format_source(const char *source, const char *filename, const format_options_t *opts)
{
    parser_t p;
    parser_init(&p, source, filename);
    sh_list_t *ast = parser_parse(&p);

    if (parser_error_count(&p) > 0) {
        fprintf(stderr, "opsh format: parse errors in %s\n", filename);
        sh_list_free(ast);
        parser_destroy(&p);
        return NULL;
    }

    comment_t *comments = parser_take_comments(&p);

    strbuf_t out;
    strbuf_init(&out);
    format_ast(&out, ast, comments, opts);

    sh_list_free(ast);
    comment_free(comments);
    parser_destroy(&p);

    return strbuf_detach(&out);
}

static void format_usage(void)
{
    fprintf(stderr, "usage: opsh format [options] [file...]\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  -w          write result to file instead of stdout\n");
    fprintf(stderr, "  -d          show diff when formatting differs\n");
    fprintf(stderr, "  -l          list files whose formatting differs\n");
    fprintf(stderr, "  -i N        indent width (0=tabs, default: 4)\n");
}

/* Process a single file. Returns 0 if unchanged/success, 1 if changed, 2 on error. */
static int process_file(const char *path, const char *source, const format_options_t *opts,
                        bool write_inplace, bool show_diff, bool list_only)
{
    char *formatted = format_source(source, path, opts);
    if (formatted == NULL) {
        return 2;
    }

    bool changed = (strcmp(source, formatted) != 0);

    if (list_only) {
        if (changed) {
            printf("%s\n", path);
        }
        free(formatted);
        return changed ? 1 : 0;
    }

    if (show_diff) {
        if (changed) {
            /* Write original and formatted to temp files, run diff */
            const char *tmpdir = getenv("TMPDIR");
            if (tmpdir == NULL) {
                tmpdir = "/tmp";
            }
            char orig_path[4096];
            char new_path[4096];
            snprintf(orig_path, sizeof(orig_path), "%s/opsh-fmt-orig-XXXXXX", tmpdir);
            snprintf(new_path, sizeof(new_path), "%s/opsh-fmt-new-XXXXXX", tmpdir);

            int orig_fd = mkstemp(orig_path);
            int new_fd = mkstemp(new_path);
            if (orig_fd < 0 || new_fd < 0) {
                fprintf(stderr, "opsh format: cannot create temp files\n");
                if (orig_fd >= 0) {
                    close(orig_fd);
                    unlink(orig_path);
                }
                if (new_fd >= 0) {
                    close(new_fd);
                    unlink(new_path);
                }
                free(formatted);
                return 2;
            }

            FILE *of = fdopen(orig_fd, "w");
            FILE *nf = fdopen(new_fd, "w");
            if (of != NULL) {
                fputs(source, of);
                fclose(of);
            }
            if (nf != NULL) {
                fputs(formatted, nf);
                fclose(nf);
            }

            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                     "diff -u --label '%s (original)' --label '%s (formatted)'"
                     " '%s' '%s'",
                     path, path, orig_path, new_path);
            (void)system(cmd);

            unlink(orig_path);
            unlink(new_path);
        }
        free(formatted);
        return changed ? 1 : 0;
    }

    if (write_inplace) {
        if (changed) {
            FILE *f = fopen(path, "w");
            if (f == NULL) {
                fprintf(stderr, "opsh format: cannot write %s\n", path);
                free(formatted);
                return 2;
            }
            fputs(formatted, f);
            fclose(f);
        }
        free(formatted);
        return changed ? 1 : 0;
    }

    /* Default: write to stdout */
    fputs(formatted, stdout);
    free(formatted);
    return 0;
}

int format_main(int argc, char *argv[])
{
    format_options_t opts = {.indent_width = 4};
    bool write_inplace = false;
    bool show_diff = false;
    bool list_only = false;
    int i;
    int file_start = -1;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) {
            write_inplace = true;
        } else if (strcmp(argv[i], "-d") == 0) {
            show_diff = true;
        } else if (strcmp(argv[i], "-l") == 0) {
            list_only = true;
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            opts.indent_width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            format_usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "opsh format: unknown option: %s\n", argv[i]);
            format_usage();
            return 1;
        } else {
            file_start = i;
            break;
        }
    }

    /* No files: read from stdin */
    if (file_start < 0) {
        if (write_inplace) {
            fprintf(stderr, "opsh format: -w requires file arguments\n");
            return 1;
        }
        char *source = read_stdin();
        int rc = process_file("<stdin>", source, &opts, false, show_diff, list_only);
        free(source);
        return rc >= 2 ? rc : 0;
    }

    int exit_code = 0;
    for (i = file_start; i < argc; i++) {
        char *source = read_file(argv[i]);
        if (source == NULL) {
            fprintf(stderr, "opsh format: cannot read %s\n", argv[i]);
            exit_code = 2;
            continue;
        }
        int rc = process_file(argv[i], source, &opts, write_inplace, show_diff, list_only);
        free(source);
        if (rc == 2) {
            exit_code = 2;
        } else if (rc == 1 && exit_code == 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
