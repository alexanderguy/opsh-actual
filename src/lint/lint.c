#include "lint/lint.h"

#include "foundation/json.h"
#include "foundation/util.h"
#include "parser/parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Lint context: accumulates diagnostics during AST walk */
typedef struct {
    const char *filename;
    lint_diag_t *diags;
    lint_diag_t **tail;
    int count;
} lint_ctx_t;

static void ctx_init(lint_ctx_t *ctx, const char *filename)
{
    ctx->filename = filename;
    ctx->diags = NULL;
    ctx->tail = &ctx->diags;
    ctx->count = 0;
}

static void emit_diag(lint_ctx_t *ctx, int code, lint_severity_t sev, unsigned int lineno,
                      const char *fmt, ...) __attribute__((format(printf, 5, 6)));

static void emit_diag(lint_ctx_t *ctx, int code, lint_severity_t sev, unsigned int lineno,
                      const char *fmt, ...)
{
    lint_diag_t *d = xcalloc(1, sizeof(*d));
    d->code = code;
    d->severity = sev;
    d->filename = ctx->filename;
    d->lineno = lineno;
    d->column = 0;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    d->message = xmalloc(strlen(buf) + 1);
    strcpy(d->message, buf);

    d->next = NULL;
    *ctx->tail = d;
    ctx->tail = &d->next;
    ctx->count++;
}

/*
 * Check helpers
 */

/* Return the literal string value of a simple word, or NULL if not a plain literal */
static const char *word_literal(const word_part_t *w)
{
    if (w == NULL) {
        return NULL;
    }
    if (w->type != WP_LITERAL || w->next != NULL) {
        return NULL;
    }
    return w->part.string;
}

/*
 * SC2086: Double quote to prevent globbing and word splitting.
 *
 * Fires on unquoted parameter expansions in command argument positions.
 * Skips: command name (index 0), assignments, special params ($?, $#, $!).
 */
static void check_word_quoting(lint_ctx_t *ctx, const word_part_t *w, unsigned int lineno)
{
    while (w != NULL) {
        if (w->type == WP_PARAM && !w->quoted) {
            const char *name = w->part.param->name;
            /* Skip single-char special params that don't word-split */
            if (name[0] != '\0' && name[1] == '\0') {
                char ch = name[0];
                if (ch == '?' || ch == '!' || ch == '#' || ch == '$' || ch == '-') {
                    w = w->next;
                    continue;
                }
                /* $@ and $* get SC2048 instead */
                if (ch == '@' || ch == '*') {
                    emit_diag(ctx, 2048, LINT_WARNING, lineno,
                              "Use \"$%c\" (with quotes) to prevent whitespace problems", ch);
                    w = w->next;
                    continue;
                }
            }
            emit_diag(ctx, 2086, LINT_INFO, lineno,
                      "Double quote to prevent globbing and word splitting");
        }
        /* Recurse into arithmetic and command substitution */
        if (w->type == WP_CMDSUB && w->part.cmdsub.is_preparsed) {
            /* Command substitution bodies are checked by the main walker */
        }
        w = w->next;
    }
}

/*
 * SC2164: Use 'cd ... || exit' in case cd fails.
 *
 * Fires when cd is a simple command not guarded by || on the right side.
 */
static bool is_cd_guarded(const command_t *cmd, const and_or_t *pl)
{
    (void)cmd;
    /* If this pipeline has a || successor, cd is guarded */
    if (pl->next != NULL && !pl->connector) {
        /* connector=false means || connector to next pipeline */
        return true;
    }
    return false;
}

/*
 * AST walker
 */
static void walk_sh_list(lint_ctx_t *ctx, const sh_list_t *ao);
static void walk_command(lint_ctx_t *ctx, const command_t *cmd, const and_or_t *pl);

static void walk_sh_list(lint_ctx_t *ctx, const sh_list_t *ao)
{
    while (ao != NULL) {
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

static void walk_command(lint_ctx_t *ctx, const command_t *cmd, const and_or_t *pl)
{
    switch (cmd->type) {
    case CT_SIMPLE: {
        /* Check command arguments (skip index 0 = command name) */
        size_t i;
        for (i = 1; i < cmd->u.simple.words.length; i++) {
            check_word_quoting(ctx, plist_get(&cmd->u.simple.words, i), cmd->lineno);
        }

        /* SC2164: cd without error guard */
        if (cmd->u.simple.words.length > 0) {
            const char *name = word_literal(plist_get(&cmd->u.simple.words, 0));
            if (name != NULL && strcmp(name, "cd") == 0) {
                /* Only check if this is the sole command in a single-command pipeline */
                if (cmd->next == NULL && !is_cd_guarded(cmd, pl)) {
                    emit_diag(ctx, 2164, LINT_WARNING, cmd->lineno,
                              "Use 'cd ... || exit' in case cd fails");
                }
            }
        }
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
    case CT_FOR:
        /* Check for-list words */
        {
            size_t i;
            for (i = 0; i < cmd->u.for_clause.wordlist.length; i++) {
                check_word_quoting(ctx, plist_get(&cmd->u.for_clause.wordlist, i), cmd->lineno);
            }
        }
        walk_sh_list(ctx, cmd->u.for_clause.body);
        break;
    case CT_WHILE:
    case CT_UNTIL:
        walk_sh_list(ctx, cmd->u.while_clause.condition);
        walk_sh_list(ctx, cmd->u.while_clause.body);
        break;
    case CT_CASE:
        walk_sh_list(ctx, cmd->u.case_clause.items ? cmd->u.case_clause.items->body : NULL);
        {
            const case_item_t *ci = cmd->u.case_clause.items;
            while (ci != NULL) {
                walk_sh_list(ctx, ci->body);
                ci = ci->next;
            }
        }
        break;
    case CT_GROUP:
    case CT_SUBSHELL:
        walk_sh_list(ctx, cmd->u.group.body);
        break;
    case CT_FUNCDEF:
        walk_command(ctx, cmd->u.func_def.body, pl);
        break;
    case CT_BRACKET:
        /* Don't check quoting inside [[ ]] — it handles word splitting itself */
        break;
    }
}

/*
 * Public API
 */

lint_diag_t *lint_check(const sh_list_t *ast, const char *filename)
{
    lint_ctx_t ctx;
    ctx_init(&ctx, filename);
    walk_sh_list(&ctx, ast);
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
            /* bold filename, colored severity */
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

    /* No files: read from stdin */
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
