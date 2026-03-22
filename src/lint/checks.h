#ifndef OPSH_LINT_CHECKS_H
#define OPSH_LINT_CHECKS_H

#include "lint/lint.h"
#include "parser/ast.h"

#include <stdbool.h>

/* Walker context passed to all checks */
typedef struct {
    const char *filename;
    lint_diag_t *diags;
    lint_diag_t **tail;
    int count;
    bool in_function;
    int loop_depth;
} lint_ctx_t;

void lint_ctx_init(lint_ctx_t *ctx, const char *filename);

/* Emit a diagnostic. Format string interface. */
void lint_emit(lint_ctx_t *ctx, int code, lint_severity_t sev, unsigned int lineno, const char *fmt,
               ...) __attribute__((format(printf, 5, 6)));

/* Return the literal string value of a simple word, or NULL if not a plain literal */
const char *lint_word_literal(const word_part_t *w);

/* Check if a word contains a command substitution */
bool lint_word_has_cmdsub(const word_part_t *w);

/*
 * Check entry points — called by the walker at specific AST positions.
 */

/* Called for each word in a command's argument list (not command name, not assignments) */
void checks_on_word(lint_ctx_t *ctx, const word_part_t *w, unsigned int lineno);

/* Called for each simple command */
void checks_on_simple_command(lint_ctx_t *ctx, const command_t *cmd, const and_or_t *pl);

/* Called for each compound command */
void checks_on_compound_command(lint_ctx_t *ctx, const command_t *cmd);

/* Called once per sh-list to check pipeline-level patterns */
void checks_on_sh_list(lint_ctx_t *ctx, const sh_list_t *ao);

/* Called for each word unit to check for backtick usage */
void checks_on_word_unit(lint_ctx_t *ctx, const word_part_t *w, unsigned int lineno);

/* Variable tracking pass — runs after the main walk */
void checks_variable_tracking(lint_ctx_t *ctx, const sh_list_t *ast);

#endif /* OPSH_LINT_CHECKS_H */
