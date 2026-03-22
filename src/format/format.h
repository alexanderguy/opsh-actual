#ifndef OPSH_FORMAT_FORMAT_H
#define OPSH_FORMAT_FORMAT_H

#include "foundation/strbuf.h"
#include "parser/ast.h"

typedef struct {
    int indent_width; /* 0 = tabs, >0 = spaces */
} format_options_t;

/* Format an AST back to source text. Comments are interleaved by line number.
 * Caller must strbuf_destroy the result. */
void format_ast(strbuf_t *out, const sh_list_t *ast, const comment_t *comments,
                const format_options_t *opts);

/* Entry point for `opsh format` subcommand. Returns exit code. */
int format_main(int argc, char *argv[]);

#endif /* OPSH_FORMAT_FORMAT_H */
