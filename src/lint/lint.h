#ifndef OPSH_LINT_LINT_H
#define OPSH_LINT_LINT_H

#include "foundation/strbuf.h"
#include "parser/ast.h"

typedef enum {
    LINT_FMT_GCC,   /* file:line:col: severity: message [SCcode] */
    LINT_FMT_TTY,   /* same as gcc but with color when on a terminal */
    LINT_FMT_JSON1, /* JSON array of diagnostics */
    LINT_FMT_QUIET, /* no output, exit code only */
} lint_format_t;

typedef enum {
    LINT_ERROR,
    LINT_WARNING,
    LINT_INFO,
    LINT_STYLE,
} lint_severity_t;

typedef struct lint_diag {
    int code; /* shellcheck-compatible code, e.g. 2086 */
    lint_severity_t severity;
    const char *filename; /* borrowed */
    unsigned int lineno;
    unsigned int column; /* 0 if unknown */
    char *message;       /* owned */
    struct lint_diag *next;
} lint_diag_t;

/* Run all lint checks on a parsed AST. Caller owns the returned list. */
lint_diag_t *lint_check(const sh_list_t *ast, const char *filename);

/* Free a diagnostic list */
void lint_diag_free(lint_diag_t *d);

/* Format diagnostics to a string buffer */
void lint_format_diags(strbuf_t *out, const lint_diag_t *diags, lint_format_t fmt);

/* Entry point for `opsh lint` subcommand. Returns exit code. */
int lint_main(int argc, char *argv[]);

#endif /* OPSH_LINT_LINT_H */
