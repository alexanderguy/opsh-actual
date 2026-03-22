/*
 * libfuzzer harness for the opsh linter.
 *
 * Parses arbitrary input, runs lint checks, and formats the output.
 * Catches crashes and memory errors in the AST walker and formatters.
 */
#include "lint/lint.h"
#include "parser/parser.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *input = malloc(size + 1);
    if (!input)
        return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    parser_t p;
    parser_init(&p, input, "<fuzz>");
    sh_list_t *ast = parser_parse(&p);

    if (parser_error_count(&p) > 0) {
        sh_list_free(ast);
        parser_destroy(&p);
        free(input);
        return 0;
    }

    /* Run lint checks */
    lint_diag_t *diags = lint_check(ast, "<fuzz>");

    /* Exercise all output formatters */
    strbuf_t out;
    strbuf_init(&out);

    lint_format_diags(&out, diags, LINT_FMT_GCC);
    strbuf_clear(&out);

    lint_format_diags(&out, diags, LINT_FMT_JSON1);
    strbuf_clear(&out);

    lint_format_diags(&out, diags, LINT_FMT_QUIET);

    strbuf_destroy(&out);
    lint_diag_free(diags);
    sh_list_free(ast);
    parser_destroy(&p);
    free(input);
    return 0;
}
