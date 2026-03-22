#include "lint/checks.h"

#include "foundation/hashtable.h"
#include "foundation/util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Infrastructure
 */

void lint_ctx_init(lint_ctx_t *ctx, const char *filename)
{
    ctx->filename = filename;
    ctx->diags = NULL;
    ctx->tail = &ctx->diags;
    ctx->count = 0;
    ctx->in_function = false;
    ctx->loop_depth = 0;
}

void lint_emit(lint_ctx_t *ctx, int code, lint_severity_t sev, unsigned int lineno, const char *fmt,
               ...)
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
    d->message = xstrdup(buf);

    d->next = NULL;
    *ctx->tail = d;
    ctx->tail = &d->next;
    ctx->count++;
}

const char *lint_word_literal(const word_part_t *w)
{
    if (w == NULL || w->type != WP_LITERAL || w->next != NULL) {
        return NULL;
    }
    return w->part.string;
}

bool lint_word_has_cmdsub(const word_part_t *w)
{
    while (w != NULL) {
        if (w->type == WP_CMDSUB) {
            return true;
        }
        w = w->next;
    }
    return false;
}

/* Check if a word is entirely a single command substitution */
static bool word_is_single_cmdsub(const word_part_t *w)
{
    return w != NULL && w->type == WP_CMDSUB && w->next == NULL;
}

/* Check if a word is entirely a single parameter expansion */
static bool word_is_single_param(const word_part_t *w)
{
    return w != NULL && w->type == WP_PARAM && w->next == NULL;
}

/* Get the command name of a simple command, or NULL */
static const char *simple_cmd_name(const command_t *cmd)
{
    if (cmd->type != CT_SIMPLE || cmd->u.simple.words.length == 0) {
        return NULL;
    }
    return lint_word_literal(plist_get(&cmd->u.simple.words, 0));
}

/* Check if a pipeline has a single command (not piped) */
static bool is_single_cmd_pipeline(const command_t *cmd)
{
    return cmd->next == NULL;
}

/*
 * SC2006: Use $(...) notation instead of legacy backticks
 */
void checks_on_word_unit(lint_ctx_t *ctx, const word_part_t *w, unsigned int lineno)
{
    while (w != NULL) {
        if (w->type == WP_CMDSUB && w->part.cmdsub.is_backtick) {
            lint_emit(ctx, 2006, LINT_STYLE, lineno,
                      "Use $(...) notation instead of legacy backticked `...`");
        }
        w = w->next;
    }
}

/*
 * Word-level checks: called for each argument word in a simple command
 */
void checks_on_word(lint_ctx_t *ctx, const word_part_t *w, unsigned int lineno)
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
                /* SC2048: unquoted $@ or $* */
                if (ch == '@' || ch == '*') {
                    lint_emit(ctx, 2048, LINT_WARNING, lineno,
                              "Use \"$%c\" (with quotes) to prevent whitespace problems", ch);
                    w = w->next;
                    continue;
                }
            }

            /* SC2086: unquoted variable expansion */
            lint_emit(ctx, 2086, LINT_INFO, lineno,
                      "Double quote to prevent globbing and word splitting");
        }

        /* SC2046: unquoted command substitution */
        if (w->type == WP_CMDSUB && !w->quoted) {
            lint_emit(ctx, 2046, LINT_WARNING, lineno, "Quote this to prevent word splitting");
        }

        w = w->next;
    }
}

/*
 * Simple command checks
 */
void checks_on_simple_command(lint_ctx_t *ctx, const command_t *cmd, const and_or_t *pl)
{
    const char *name = simple_cmd_name(cmd);
    size_t argc = cmd->u.simple.words.length;

    /* SC2164: cd without error guard */
    if (name != NULL && strcmp(name, "cd") == 0 && is_single_cmd_pipeline(cmd)) {
        bool guarded = false;
        if (pl->next != NULL && !pl->connector) {
            guarded = true; /* has || successor */
        }
        if (!guarded) {
            lint_emit(ctx, 2164, LINT_WARNING, cmd->lineno,
                      "Use 'cd ... || exit' in case cd fails");
        }
    }

    /* SC2162: read without -r */
    if (name != NULL && strcmp(name, "read") == 0) {
        bool has_r = false;
        size_t i;
        for (i = 1; i < argc; i++) {
            const char *arg = lint_word_literal(plist_get(&cmd->u.simple.words, i));
            if (arg != NULL && arg[0] == '-') {
                const char *c;
                for (c = arg + 1; *c != '\0'; c++) {
                    if (*c == 'r') {
                        has_r = true;
                    }
                }
            }
        }
        if (!has_r) {
            lint_emit(ctx, 2162, LINT_WARNING, cmd->lineno,
                      "read without -r will mangle backslashes");
        }
    }

    /* SC2230: which → command -v */
    if (name != NULL && strcmp(name, "which") == 0) {
        lint_emit(ctx, 2230, LINT_INFO, cmd->lineno,
                  "'which' is non-standard. Use builtin 'command -v' instead");
    }

    /* SC2002: useless cat — cat file | cmd */
    if (name != NULL && strcmp(name, "cat") == 0 && argc == 2 && cmd->next != NULL) {
        lint_emit(ctx, 2002, LINT_STYLE, cmd->lineno, "Useless cat. Consider 'cmd < file' instead");
    }

    /* SC2008: piping to echo */
    if (name != NULL && strcmp(name, "echo") == 0 && cmd->next == NULL) {
        /* Check if this is NOT the first command in the pipeline (i.e. something pipes to us).
         * We detect this by checking if there's a preceding command in the pipeline list. */
        const command_t *first = pl->commands;
        if (first != cmd) {
            lint_emit(ctx, 2008, LINT_STYLE, cmd->lineno,
                      "echo doesn't read from stdin. Are you sure you should be piping to it?");
        }
    }

    /* SC2216: piping to rm */
    if (name != NULL && strcmp(name, "rm") == 0 && cmd->next == NULL) {
        const command_t *first = pl->commands;
        if (first != cmd) {
            lint_emit(ctx, 2216, LINT_WARNING, cmd->lineno,
                      "Piping to 'rm', a command that doesn't read stdin");
        }
    }

    /* SC2123: assigning to PATH */
    {
        size_t i;
        for (i = 0; i < cmd->u.simple.assigns.length; i++) {
            const word_part_t *aw = plist_get(&cmd->u.simple.assigns, i);
            const char *alit = lint_word_literal(aw);
            if (alit != NULL && strncmp(alit, "PATH=", 5) == 0 && argc > 0) {
                lint_emit(ctx, 2123, LINT_WARNING, cmd->lineno,
                          "PATH is the shell search path. Use another name");
            }
        }
    }

    /* SC2155: local/export var=$(cmd) masks return value */
    if (name != NULL && (strcmp(name, "local") == 0 || strcmp(name, "export") == 0)) {
        size_t i;
        for (i = 1; i < argc; i++) {
            const word_part_t *arg = plist_get(&cmd->u.simple.words, i);
            if (arg != NULL && lint_word_has_cmdsub(arg)) {
                lint_emit(ctx, 2155, LINT_WARNING, cmd->lineno,
                          "Declare and assign separately to avoid masking return values");
                break;
            }
        }
    }

    /* SC2005: useless echo — echo $(cmd) */
    if (name != NULL && strcmp(name, "echo") == 0 && argc == 2 && is_single_cmd_pipeline(cmd)) {
        const word_part_t *arg = plist_get(&cmd->u.simple.words, 1);
        if (word_is_single_cmdsub(arg)) {
            lint_emit(ctx, 2005, LINT_STYLE, cmd->lineno,
                      "Useless echo? Instead of 'echo $(cmd)', just use 'cmd'");
        }
    }

    /* SC2091: $(cmd) used as a bare command name */
    if (argc > 0) {
        const word_part_t *cmd_word = plist_get(&cmd->u.simple.words, 0);
        if (word_is_single_cmdsub(cmd_word) && is_single_cmd_pipeline(cmd)) {
            lint_emit(ctx, 2091, LINT_WARNING, cmd->lineno,
                      "Remove surrounding $() to avoid executing output");
        }
    }

    /* SC2059: variable in printf format string */
    if (name != NULL && strcmp(name, "printf") == 0 && argc >= 2) {
        const word_part_t *fmt_arg = plist_get(&cmd->u.simple.words, 1);
        if (fmt_arg != NULL && word_is_single_param(fmt_arg)) {
            lint_emit(ctx, 2059, LINT_INFO, cmd->lineno,
                      "Don't use variables in the printf format string. Use printf '..%%s..' "
                      "\"$foo\"");
        }
    }

    /* SC2163: export $var instead of export var */
    if (name != NULL && (strcmp(name, "export") == 0 || strcmp(name, "readonly") == 0)) {
        size_t i;
        for (i = 1; i < argc; i++) {
            const word_part_t *arg = plist_get(&cmd->u.simple.words, i);
            if (word_is_single_param(arg) && arg->part.param->type == PE_NONE) {
                lint_emit(ctx, 2163, LINT_WARNING, cmd->lineno,
                          "This does not %s the variable. Remove $/${} for that", name);
                break;
            }
        }
    }

    /* SC2184: unquoted arguments to unset */
    if (name != NULL && strcmp(name, "unset") == 0) {
        size_t i;
        for (i = 1; i < argc; i++) {
            const word_part_t *arg = plist_get(&cmd->u.simple.words, i);
            if (arg != NULL && arg->type == WP_PARAM && !arg->quoted) {
                lint_emit(ctx, 2184, LINT_WARNING, cmd->lineno,
                          "Quote arguments to unset so they're not glob expanded");
                break;
            }
        }
    }

    /* SC2269: self-assignment — var=$var */
    if (argc == 0 && cmd->u.simple.assigns.length == 1) {
        const word_part_t *aw = plist_get(&cmd->u.simple.assigns, 0);
        if (aw != NULL && aw->type == WP_LITERAL && aw->next != NULL &&
            aw->next->type == WP_PARAM && aw->next->next == NULL) {
            const char *assign_text = aw->part.string;
            const char *eq = strchr(assign_text, '=');
            if (eq != NULL) {
                size_t name_len = (size_t)(eq - assign_text);
                const char *param_name = aw->next->part.param->name;
                if (aw->next->part.param->type == PE_NONE && strlen(param_name) == name_len &&
                    strncmp(assign_text, param_name, name_len) == 0) {
                    lint_emit(
                        ctx, 2269, LINT_WARNING, cmd->lineno,
                        "This variable is assigned to itself, so the assignment does nothing");
                }
            }
        }
    }

    /* SC2172: trapping signals by number */
    if (name != NULL && strcmp(name, "trap") == 0 && argc >= 3) {
        size_t i;
        for (i = 2; i < argc; i++) {
            const char *arg = lint_word_literal(plist_get(&cmd->u.simple.words, i));
            if (arg != NULL && arg[0] >= '0' && arg[0] <= '9') {
                lint_emit(ctx, 2172, LINT_WARNING, cmd->lineno,
                          "Trapping signals by number is not well defined. Prefer signal names");
                break;
            }
        }
    }
}

/*
 * Compound command checks
 */
void checks_on_compound_command(lint_ctx_t *ctx, const command_t *cmd)
{
    switch (cmd->type) {
    case CT_FOR: {
        if (cmd->u.for_clause.wordlist.length == 1) {
            const word_part_t *w = plist_get(&cmd->u.for_clause.wordlist, 0);
            /* SC2043: for loop with single constant value */
            if (lint_word_literal(w) != NULL) {
                lint_emit(ctx, 2043, LINT_WARNING, cmd->lineno,
                          "This loop will only ever run once for a constant value");
            } else if (w != NULL && w->quoted) {
                /* SC2066: double-quoted single word (only if not already a literal constant) */
                lint_emit(ctx, 2066, LINT_WARNING, cmd->lineno,
                          "Since you double quoted this, it will not word split, and the loop will "
                          "only run once");
            }
        }
        break;
    }

    case CT_CASE: {
        /* SC2249: missing default *) case */
        const case_item_t *ci = cmd->u.case_clause.items;
        bool has_default = false;
        while (ci != NULL) {
            size_t i;
            for (i = 0; i < ci->patterns.length; i++) {
                const char *pat = lint_word_literal(plist_get(&ci->patterns, i));
                if (pat != NULL && strcmp(pat, "*") == 0) {
                    has_default = true;
                }
            }
            ci = ci->next;
        }
        if (!has_default && cmd->u.case_clause.items != NULL) {
            lint_emit(ctx, 2249, LINT_INFO, cmd->lineno,
                      "Consider adding a default *) case, even if it just exits with error");
        }
        break;
    }

    case CT_FUNCDEF: {
        /* SC2112: function keyword — we can detect this from the parser but
         * the AST doesn't track which syntax was used. Skip for now. */
        break;
    }

    default:
        break;
    }

    /* SC2168: local outside function */
    if (cmd->type == CT_SIMPLE && !ctx->in_function) {
        const char *name = simple_cmd_name(cmd);
        if (name != NULL && strcmp(name, "local") == 0) {
            lint_emit(ctx, 2168, LINT_ERROR, cmd->lineno, "'local' is only valid in functions");
        }
    }

    /* SC2104: break in function but not in loop */
    if (cmd->type == CT_SIMPLE && ctx->in_function && ctx->loop_depth == 0) {
        const char *name = simple_cmd_name(cmd);
        if (name != NULL && strcmp(name, "break") == 0) {
            lint_emit(ctx, 2104, LINT_ERROR, cmd->lineno,
                      "In functions, use return instead of break");
        }
    }

    /* SC2105: break outside loop */
    if (cmd->type == CT_SIMPLE && !ctx->in_function && ctx->loop_depth == 0) {
        const char *name = simple_cmd_name(cmd);
        if (name != NULL && strcmp(name, "break") == 0) {
            lint_emit(ctx, 2105, LINT_ERROR, cmd->lineno, "break is only valid in loops");
        }
        if (name != NULL && strcmp(name, "continue") == 0) {
            lint_emit(ctx, 2105, LINT_ERROR, cmd->lineno, "continue is only valid in loops");
        }
    }
}

/*
 * And-or list checks
 */
void checks_on_sh_list(lint_ctx_t *ctx, const sh_list_t *ao)
{
    /* SC2015: A && B || C is not if-then-else */
    const and_or_t *pl = ao->pipelines;
    while (pl != NULL && pl->next != NULL) {
        /* Look for pattern: pl is && connector, pl->next is || connector */
        if (pl->connector && pl->next->next != NULL && !pl->next->connector) {
            unsigned int lineno = 0;
            if (pl->next->next->commands != NULL) {
                lineno = pl->next->next->commands->lineno;
            }
            if (lineno == 0 && pl->commands != NULL) {
                lineno = pl->commands->lineno;
            }
            lint_emit(ctx, 2015, LINT_INFO, lineno,
                      "Note that A && B || C is not if-then-else. C may run when A is true");
        }
        pl = pl->next;
    }
}

/*
 * Variable tracking
 */

/* Collect variable assignments and references from the AST */
static void track_vars_word(const word_part_t *w, hashtable_t *refs)
{
    while (w != NULL) {
        if (w->type == WP_PARAM && w->part.param->name[0] != '\0') {
            const char *name = w->part.param->name;
            /* Skip special params */
            if (name[1] == '\0') {
                char ch = name[0];
                if (ch == '?' || ch == '!' || ch == '#' || ch == '$' || ch == '-' || ch == '@' ||
                    ch == '*' || (ch >= '0' && ch <= '9')) {
                    w = w->next;
                    continue;
                }
            }
            if (ht_get(refs, name) == NULL) {
                char *key = xstrdup(name);
                ht_set(refs, key, (void *)1);
            }
        }
        if (w->type == WP_ARITH) {
            track_vars_word(w->part.arith, refs);
        }
        w = w->next;
    }
}

static void track_vars_command(const command_t *cmd, hashtable_t *assigns, hashtable_t *refs,
                               hashtable_t *funcs_defined);
static void track_vars_sh_list(const sh_list_t *ao, hashtable_t *assigns, hashtable_t *refs,
                               hashtable_t *funcs_defined);

static void track_vars_sh_list(const sh_list_t *ao, hashtable_t *assigns, hashtable_t *refs,
                               hashtable_t *funcs_defined)
{
    while (ao != NULL) {
        const and_or_t *pl = ao->pipelines;
        while (pl != NULL) {
            const command_t *cmd = pl->commands;
            while (cmd != NULL) {
                track_vars_command(cmd, assigns, refs, funcs_defined);
                cmd = cmd->next;
            }
            pl = pl->next;
        }
        ao = ao->next;
    }
}

/* Track variable references in redirections */
static void track_vars_io_redirs(const io_redir_t *r, hashtable_t *refs)
{
    while (r != NULL) {
        if (r->target != NULL) {
            track_vars_word(r->target, refs);
        }
        r = r->next;
    }
}

/* Track variables assigned by `read` command */
static void track_read_assigns(const command_t *cmd, hashtable_t *assigns)
{
    const char *name = simple_cmd_name(cmd);
    if (name == NULL || strcmp(name, "read") != 0) {
        return;
    }
    size_t i;
    for (i = 1; i < cmd->u.simple.words.length; i++) {
        const char *arg = lint_word_literal(plist_get(&cmd->u.simple.words, i));
        if (arg == NULL) {
            continue;
        }
        /* Skip flags */
        if (arg[0] == '-') {
            continue;
        }
        /* This is a variable name that read assigns to */
        if (ht_get(assigns, arg) == NULL) {
            char *key = xstrdup(arg);
            ht_set(assigns, key, (void *)1);
        }
    }
}

static void track_vars_command(const command_t *cmd, hashtable_t *assigns, hashtable_t *refs,
                               hashtable_t *funcs_defined)
{
    size_t i;

    /* Track references in redirections for all command types */
    track_vars_io_redirs(cmd->redirs, refs);

    switch (cmd->type) {
    case CT_SIMPLE: {
        /* Track assignments */
        for (i = 0; i < cmd->u.simple.assigns.length; i++) {
            const word_part_t *aw = plist_get(&cmd->u.simple.assigns, i);
            if (aw != NULL && aw->type == WP_LITERAL && aw->part.string != NULL) {
                const char *eq = strchr(aw->part.string, '=');
                if (eq != NULL) {
                    size_t name_len = (size_t)(eq - aw->part.string);
                    if (name_len > 0) {
                        char *name = xmalloc(name_len + 1);
                        memcpy(name, aw->part.string, name_len);
                        name[name_len] = '\0';
                        if (ht_get(assigns, name) == NULL) {
                            ht_set(assigns, name, (void *)1);
                        } else {
                            free(name);
                        }
                    }
                }
            }
            /* Track references in assignment value */
            const word_part_t *val = aw;
            if (val != NULL && val->type == WP_LITERAL) {
                val = val->next; /* skip past "name=" string */
            }
            track_vars_word(val, refs);
        }

        /* Track references in command words */
        for (i = 0; i < cmd->u.simple.words.length; i++) {
            track_vars_word(plist_get(&cmd->u.simple.words, i), refs);
        }

        /* Track local/export/readonly assignments */
        const char *cname = simple_cmd_name(cmd);
        if (cname != NULL && (strcmp(cname, "local") == 0 || strcmp(cname, "export") == 0 ||
                              strcmp(cname, "readonly") == 0)) {
            for (i = 1; i < cmd->u.simple.words.length; i++) {
                const word_part_t *arg = plist_get(&cmd->u.simple.words, i);
                if (arg != NULL && arg->type == WP_LITERAL && arg->part.string != NULL) {
                    const char *eq = strchr(arg->part.string, '=');
                    if (eq != NULL) {
                        size_t name_len = (size_t)(eq - arg->part.string);
                        if (name_len > 0) {
                            char *name = xmalloc(name_len + 1);
                            memcpy(name, arg->part.string, name_len);
                            name[name_len] = '\0';
                            if (ht_get(assigns, name) == NULL) {
                                ht_set(assigns, name, (void *)1);
                            } else {
                                free(name);
                            }
                        }
                    }
                }
            }
        }

        /* Track read assignments */
        track_read_assigns(cmd, assigns);
        break;
    }
    case CT_IF: {
        const if_clause_t *ic = cmd->u.if_clause.clauses;
        while (ic != NULL) {
            if (ic->condition != NULL) {
                track_vars_sh_list(ic->condition, assigns, refs, funcs_defined);
            }
            track_vars_sh_list(ic->body, assigns, refs, funcs_defined);
            ic = ic->next;
        }
        break;
    }
    case CT_FOR:
        /* The loop variable is assigned */
        if (cmd->u.for_clause.varname != NULL) {
            const char *name = cmd->u.for_clause.varname;
            if (ht_get(assigns, name) == NULL) {
                char *key = xstrdup(name);
                ht_set(assigns, key, (void *)1);
            }
        }
        for (i = 0; i < cmd->u.for_clause.wordlist.length; i++) {
            track_vars_word(plist_get(&cmd->u.for_clause.wordlist, i), refs);
        }
        track_vars_sh_list(cmd->u.for_clause.body, assigns, refs, funcs_defined);
        break;
    case CT_WHILE:
    case CT_UNTIL:
        track_vars_sh_list(cmd->u.while_clause.condition, assigns, refs, funcs_defined);
        track_vars_sh_list(cmd->u.while_clause.body, assigns, refs, funcs_defined);
        break;
    case CT_CASE:
        track_vars_word(cmd->u.case_clause.subject, refs);
        {
            const case_item_t *ci = cmd->u.case_clause.items;
            while (ci != NULL) {
                track_vars_sh_list(ci->body, assigns, refs, funcs_defined);
                ci = ci->next;
            }
        }
        break;
    case CT_GROUP:
    case CT_SUBSHELL:
        track_vars_sh_list(cmd->u.group.body, assigns, refs, funcs_defined);
        break;
    case CT_FUNCDEF:
        if (funcs_defined != NULL && cmd->u.func_def.name != NULL) {
            const char *fname = cmd->u.func_def.name;
            if (ht_get(funcs_defined, fname) == NULL) {
                char *key = xstrdup(fname);
                ht_set(funcs_defined, key, (void *)1);
            }
        }
        track_vars_command(cmd->u.func_def.body, assigns, refs, funcs_defined);
        break;
    case CT_BRACKET:
        break;
    }
}

/* Callback for ht_foreach to free keys */
static int free_key_cb(const char *key, void *value, void *user)
{
    (void)value;
    (void)user;
    free((void *)key);
    return 0;
}

/* Check for function calls in the AST — collect command names that match defined functions */
static void collect_func_calls(const sh_list_t *ao, const hashtable_t *funcs_defined,
                               hashtable_t *funcs_called);

static void collect_func_calls_cmd(const command_t *cmd, const hashtable_t *funcs_defined,
                                   hashtable_t *funcs_called)
{
    switch (cmd->type) {
    case CT_SIMPLE: {
        const char *name = simple_cmd_name(cmd);
        if (name != NULL && ht_get(funcs_defined, name) != NULL) {
            if (ht_get(funcs_called, name) == NULL) {
                char *key = xstrdup(name);
                ht_set(funcs_called, key, (void *)1);
            }
        }
        break;
    }
    case CT_IF: {
        const if_clause_t *ic = cmd->u.if_clause.clauses;
        while (ic != NULL) {
            if (ic->condition != NULL) {
                collect_func_calls(ic->condition, funcs_defined, funcs_called);
            }
            collect_func_calls(ic->body, funcs_defined, funcs_called);
            ic = ic->next;
        }
        break;
    }
    case CT_FOR:
        collect_func_calls(cmd->u.for_clause.body, funcs_defined, funcs_called);
        break;
    case CT_WHILE:
    case CT_UNTIL:
        collect_func_calls(cmd->u.while_clause.condition, funcs_defined, funcs_called);
        collect_func_calls(cmd->u.while_clause.body, funcs_defined, funcs_called);
        break;
    case CT_CASE: {
        const case_item_t *ci = cmd->u.case_clause.items;
        while (ci != NULL) {
            collect_func_calls(ci->body, funcs_defined, funcs_called);
            ci = ci->next;
        }
        break;
    }
    case CT_GROUP:
    case CT_SUBSHELL:
        collect_func_calls(cmd->u.group.body, funcs_defined, funcs_called);
        break;
    case CT_FUNCDEF:
        collect_func_calls_cmd(cmd->u.func_def.body, funcs_defined, funcs_called);
        break;
    case CT_BRACKET:
        break;
    }
}

static void collect_func_calls(const sh_list_t *ao, const hashtable_t *funcs_defined,
                               hashtable_t *funcs_called)
{
    while (ao != NULL) {
        const and_or_t *pl = ao->pipelines;
        while (pl != NULL) {
            const command_t *cmd = pl->commands;
            while (cmd != NULL) {
                collect_func_calls_cmd(cmd, funcs_defined, funcs_called);
                cmd = cmd->next;
            }
            pl = pl->next;
        }
        ao = ao->next;
    }
}

typedef struct {
    lint_ctx_t *ctx;
    const hashtable_t *other;
    int code;
    const char *msg_suffix;
    unsigned int lineno;
} var_check_ctx_t;

static int check_unused_var(const char *key, void *value, void *user)
{
    (void)value;
    var_check_ctx_t *vctx = user;
    if (ht_get(vctx->other, key) == NULL) {
        /* Skip well-known environment/shell variables */
        static const char *const known[] = {
            "IFS",     "PATH",   "HOME",    "PWD",   "OLDPWD", "USER",    "SHELL",
            "TERM",    "EDITOR", "VISUAL",  "LANG",  "LC_ALL", "DISPLAY", "HOSTNAME",
            "LOGNAME", "TMPDIR", "COLUMNS", "LINES", "REPLY",  "OPTARG",  "OPTIND",
            "RANDOM",  "LINENO", "SECONDS", "PPID",  NULL};
        {
            const char *const *p;
            for (p = known; *p != NULL; p++) {
                if (strcmp(key, *p) == 0) {
                    return 0;
                }
            }
        }
        lint_emit(vctx->ctx, vctx->code, LINT_WARNING, 0, "%s %s", key, vctx->msg_suffix);
    }
    return 0;
}

static int check_unused_func(const char *key, void *value, void *user)
{
    (void)value;
    var_check_ctx_t *vctx = user;
    if (ht_get(vctx->other, key) == NULL) {
        lint_emit(vctx->ctx, 2329, LINT_INFO, 0, "Function '%s' is never invoked", key);
    }
    return 0;
}

void checks_variable_tracking(lint_ctx_t *ctx, const sh_list_t *ast)
{
    hashtable_t assigns;
    hashtable_t refs;
    hashtable_t funcs_defined;
    hashtable_t funcs_called;

    ht_init(&assigns);
    ht_init(&refs);
    ht_init(&funcs_defined);
    ht_init(&funcs_called);

    track_vars_sh_list(ast, &assigns, &refs, &funcs_defined);
    collect_func_calls(ast, &funcs_defined, &funcs_called);

    /* SC2034: assigned but unused */
    {
        var_check_ctx_t vctx = {ctx, &refs, 2034, "appears unused. Verify it or export it", 0};
        ht_foreach(&assigns, check_unused_var, &vctx);
    }

    /* SC2154: referenced but not assigned */
    {
        var_check_ctx_t vctx = {ctx, &assigns, 2154, "is referenced but not assigned", 0};
        ht_foreach(&refs, check_unused_var, &vctx);
    }

    /* SC2329: function never invoked */
    {
        var_check_ctx_t vctx = {ctx, &funcs_called, 2329, NULL, 0};
        ht_foreach(&funcs_defined, check_unused_func, &vctx);
    }

    ht_foreach(&assigns, free_key_cb, NULL);
    ht_foreach(&refs, free_key_cb, NULL);
    ht_foreach(&funcs_defined, free_key_cb, NULL);
    ht_foreach(&funcs_called, free_key_cb, NULL);
    ht_destroy(&assigns);
    ht_destroy(&refs);
    ht_destroy(&funcs_defined);
    ht_destroy(&funcs_called);
}
