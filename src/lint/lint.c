#include "lint/lint.h"

#include "foundation/json.h"
#include "foundation/util.h"
#include "lint/checks.h"
#include "parser/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * AST walker — traverses the tree and calls into checks at each node.
 */
static void walk_sh_list(lint_ctx_t *ctx, const sh_list_t *ao);
static void walk_command(lint_ctx_t *ctx, const command_t *cmd, const and_or_t *pl);

static void walk_sh_list(lint_ctx_t *ctx, const sh_list_t *ao)
{
    while (ao != NULL) {
        checks_on_sh_list(ctx, ao);
        const and_or_t *pl = ao->pipelines;
        while (pl != NULL) {
            const command_t *cmd = pl->commands;
            while (cmd != NULL) {
                walk_command(ctx, cmd, pl);
                cmd = cmd->next;
            }
            pl = pl->next;
        }
        ao = ao->next;
    }
}

static void walk_word_parts(lint_ctx_t *ctx, const word_part_t *w, unsigned int lineno)
{
    checks_on_word_unit(ctx, w, lineno);
}

/* Walk redirection word units for quoting and backtick checks */
static void walk_io_redirs(lint_ctx_t *ctx, const io_redir_t *r, unsigned int lineno)
{
    while (r != NULL) {
        if (r->target != NULL) {
            walk_word_parts(ctx, r->target, lineno);
            checks_on_word(ctx, r->target, lineno);
        }
        r = r->next;
    }
}

static void walk_command(lint_ctx_t *ctx, const command_t *cmd, const and_or_t *pl)
{
    /* Run compound command checks on everything */
    checks_on_compound_command(ctx, cmd);

    /* Check redirections on all command types */
    walk_io_redirs(ctx, cmd->redirs, cmd->lineno);

    switch (cmd->type) {
    case CT_SIMPLE: {
        size_t i;
        /* Check all word units for backtick usage etc. */
        for (i = 0; i < cmd->u.simple.words.length; i++) {
            walk_word_parts(ctx, plist_get(&cmd->u.simple.words, i), cmd->lineno);
        }
        /* Check arguments (skip index 0 = command name) for quoting */
        for (i = 1; i < cmd->u.simple.words.length; i++) {
            checks_on_word(ctx, plist_get(&cmd->u.simple.words, i), cmd->lineno);
        }
        /* Run simple command checks */
        checks_on_simple_command(ctx, cmd, pl);
        break;
    }
    case CT_IF: {
        const if_clause_t *ic = cmd->u.if_clause.clauses;
        while (ic != NULL) {
            if (ic->condition != NULL) {
                walk_sh_list(ctx, ic->condition);
            }
            walk_sh_list(ctx, ic->body);
            ic = ic->next;
        }
        break;
    }
    case CT_FOR: {
        size_t i;
        for (i = 0; i < cmd->u.for_clause.wordlist.length; i++) {
            walk_word_parts(ctx, plist_get(&cmd->u.for_clause.wordlist, i), cmd->lineno);
            checks_on_word(ctx, plist_get(&cmd->u.for_clause.wordlist, i), cmd->lineno);
        }
        ctx->loop_depth++;
        walk_sh_list(ctx, cmd->u.for_clause.body);
        ctx->loop_depth--;
        break;
    }
    case CT_WHILE:
    case CT_UNTIL:
        walk_sh_list(ctx, cmd->u.while_clause.condition);
        ctx->loop_depth++;
        walk_sh_list(ctx, cmd->u.while_clause.body);
        ctx->loop_depth--;
        break;
    case CT_CASE: {
        const case_item_t *ci = cmd->u.case_clause.items;
        while (ci != NULL) {
            walk_sh_list(ctx, ci->body);
            ci = ci->next;
        }
        break;
    }
    case CT_GROUP:
    case CT_SUBSHELL:
        walk_sh_list(ctx, cmd->u.group.body);
        break;
    case CT_FUNCDEF: {
        bool was_in_function = ctx->in_function;
        int saved_loop_depth = ctx->loop_depth;
        ctx->in_function = true;
        ctx->loop_depth = 0;
        walk_command(ctx, cmd->u.func_def.body, pl);
        ctx->in_function = was_in_function;
        ctx->loop_depth = saved_loop_depth;
        break;
    }
    case CT_BRACKET:
        /* Don't check quoting inside [[ ]] */
        break;
    }
}

/*
 * Public API
 */

lint_diag_t *lint_check(const sh_list_t *ast, const char *filename)
{
    lint_ctx_t ctx;
    lint_ctx_init(&ctx, filename);
    walk_sh_list(&ctx, ast);
    checks_variable_tracking(&ctx, ast);
    return ctx.diags;
}

void lint_diag_free(lint_diag_t *d)
{
    while (d != NULL) {
        lint_diag_t *next = d->next;
        free(d->message);
        free(d);
        d = next;
    }
}

/*
 * Output formatters
 */

static const char *severity_str(lint_severity_t sev)
{
    switch (sev) {
    case LINT_ERROR:
        return "error";
    case LINT_WARNING:
        return "warning";
    case LINT_INFO:
    case LINT_STYLE:
        return "note";
    }
    return "note";
}

static const char *severity_str_json(lint_severity_t sev)
{
    switch (sev) {
    case LINT_ERROR:
        return "error";
    case LINT_WARNING:
        return "warning";
    case LINT_INFO:
        return "info";
    case LINT_STYLE:
        return "style";
    }
    return "info";
}

static void format_gcc(strbuf_t *out, const lint_diag_t *diags)
{
    const lint_diag_t *d = diags;
    while (d != NULL) {
        strbuf_append_printf(out, "%s:%u:%u: %s: %s [SC%04d]\n", d->filename, d->lineno, d->column,
                             severity_str(d->severity), d->message, d->code);
        d = d->next;
    }
}

static void format_tty(strbuf_t *out, const lint_diag_t *diags, bool color)
{
    const lint_diag_t *d = diags;
    while (d != NULL) {
        if (color) {
            strbuf_append_printf(out, "\033[1m%s:%u:%u:\033[0m ", d->filename, d->lineno,
                                 d->column);
            switch (d->severity) {
            case LINT_ERROR:
                strbuf_append_str(out, "\033[1;31merror:\033[0m ");
                break;
            case LINT_WARNING:
                strbuf_append_str(out, "\033[1;35mwarning:\033[0m ");
                break;
            case LINT_INFO:
            case LINT_STYLE:
                strbuf_append_str(out, "\033[1;36mnote:\033[0m ");
                break;
            }
            strbuf_append_printf(out, "%s \033[2m[SC%04d]\033[0m\n", d->message, d->code);
        } else {
            strbuf_append_printf(out, "%s:%u:%u: %s: %s [SC%04d]\n", d->filename, d->lineno,
                                 d->column, severity_str(d->severity), d->message, d->code);
        }
        d = d->next;
    }
}

static void format_json1(strbuf_t *out, const lint_diag_t *diags)
{
    json_begin_object(out);
    strbuf_append_str(out, "\"comments\":");
    json_begin_array(out);
    const lint_diag_t *d = diags;
    while (d != NULL) {
        json_begin_object(out);
        json_key_string(out, "file", d->filename);
        json_key_int(out, "line", d->lineno);
        json_key_int(out, "column", d->column);
        json_key_string(out, "level", severity_str_json(d->severity));
        json_key_int(out, "code", d->code);
        json_key_string(out, "message", d->message);
        json_end_object(out);
        d = d->next;
    }
    json_end_array(out);
    json_end_object(out);
    strbuf_append_byte(out, '\n');
}

void lint_format_diags(strbuf_t *out, const lint_diag_t *diags, lint_format_t fmt)
{
    switch (fmt) {
    case LINT_FMT_GCC:
        format_gcc(out, diags);
        break;
    case LINT_FMT_TTY:
        format_tty(out, diags, isatty(STDOUT_FILENO));
        break;
    case LINT_FMT_JSON1:
        format_json1(out, diags);
        break;
    case LINT_FMT_QUIET:
        break;
    }
}

/*
 * CLI
 */

static void lint_usage(void)
{
    fprintf(stderr, "usage: opsh lint [options] [file...]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Check opsh scripts for common issues. With no files, reads from stdin.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  -f <format>   output format: gcc (default), tty, json1, quiet\n");
    fprintf(stderr, "  -S <level>    minimum severity: error, warning, info, style (default)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "exit codes:\n");
    fprintf(stderr, "  0             no issues found\n");
    fprintf(stderr, "  1             issues found\n");
    fprintf(stderr, "  2             error (parse failure, cannot read file)\n");
}

static int process_lint_file(const char *path, const char *source, lint_format_t fmt,
                             lint_severity_t min_sev)
{
    parser_t p;
    parser_init(&p, source, path);
    sh_list_t *ast = parser_parse(&p);

    if (parser_error_count(&p) > 0) {
        fprintf(stderr, "opsh lint: parse errors in %s\n", path);
        sh_list_free(ast);
        parser_destroy(&p);
        return 2;
    }

    lint_diag_t *diags = lint_check(ast, path);
    sh_list_free(ast);
    parser_destroy(&p);

    /* Filter by severity */
    lint_diag_t *filtered = NULL;
    lint_diag_t **ftail = &filtered;
    lint_diag_t *d = diags;
    int found = 0;
    while (d != NULL) {
        if (d->severity <= min_sev) {
            found++;
            *ftail = d;
            ftail = &d->next;
            d = d->next;
        } else {
            lint_diag_t *skip = d;
            d = d->next;
            skip->next = NULL;
            lint_diag_free(skip);
        }
    }
    *ftail = NULL;

    strbuf_t out;
    strbuf_init(&out);
    lint_format_diags(&out, filtered, fmt);
    if (out.length > 0) {
        fwrite(out.contents, 1, out.length, stdout);
    }
    strbuf_destroy(&out);
    lint_diag_free(filtered);

    return found > 0 ? 1 : 0;
}

int lint_main(int argc, char *argv[])
{
    lint_format_t fmt = LINT_FMT_GCC;
    lint_severity_t min_sev = LINT_STYLE;
    int i;
    int file_start = -1;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "gcc") == 0) {
                fmt = LINT_FMT_GCC;
            } else if (strcmp(argv[i], "tty") == 0) {
                fmt = LINT_FMT_TTY;
            } else if (strcmp(argv[i], "json1") == 0) {
                fmt = LINT_FMT_JSON1;
            } else if (strcmp(argv[i], "quiet") == 0) {
                fmt = LINT_FMT_QUIET;
            } else {
                fprintf(stderr, "opsh lint: unknown format: %s\n", argv[i]);
                lint_usage();
                return 2;
            }
        } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "error") == 0) {
                min_sev = LINT_ERROR;
            } else if (strcmp(argv[i], "warning") == 0) {
                min_sev = LINT_WARNING;
            } else if (strcmp(argv[i], "info") == 0) {
                min_sev = LINT_INFO;
            } else if (strcmp(argv[i], "style") == 0) {
                min_sev = LINT_STYLE;
            } else {
                fprintf(stderr, "opsh lint: unknown severity: %s\n", argv[i]);
                lint_usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            lint_usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "opsh lint: unknown option: %s\n", argv[i]);
            lint_usage();
            return 2;
        } else {
            file_start = i;
            break;
        }
    }

    if (file_start < 0) {
        char *source = read_stdin();
        int rc = process_lint_file("<stdin>", source, fmt, min_sev);
        free(source);
        return rc;
    }

    int exit_code = 0;
    for (i = file_start; i < argc; i++) {
        char *source = read_file(argv[i]);
        if (source == NULL) {
            fprintf(stderr, "opsh lint: cannot read %s\n", argv[i]);
            exit_code = 2;
            continue;
        }
        int rc = process_lint_file(argv[i], source, fmt, min_sev);
        free(source);
        if (rc == 2) {
            exit_code = 2;
        } else if (rc == 1 && exit_code == 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
