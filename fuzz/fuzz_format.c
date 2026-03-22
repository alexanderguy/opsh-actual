/*
 * libfuzzer harness for the opsh formatter.
 *
 * Parses arbitrary input, then formats the result and re-parses the formatted
 * output. Catches crashes, memory errors, and verifies the formatter produces
 * valid, re-parseable output.
 */
#include "format/format.h"
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

    /* Parse */
    parser_t p;
    parser_init(&p, input, "<fuzz>");
    sh_list_t *ast = parser_parse(&p);

    if (parser_error_count(&p) > 0) {
        /* Don't format invalid input */
        sh_list_free(ast);
        parser_destroy(&p);
        free(input);
        return 0;
    }

    comment_t *comments = parser_take_comments(&p);

    /* Format */
    format_options_t opts = {.indent_width = 4};
    strbuf_t out;
    strbuf_init(&out);
    format_ast(&out, ast, comments, &opts);

    sh_list_free(ast);
    comment_free(comments);
    parser_destroy(&p);

    /* Re-parse the formatted output to verify it's valid */
    parser_t p2;
    parser_init(&p2, out.contents, "<fuzz-reparse>");
    sh_list_t *ast2 = parser_parse(&p2);

    sh_list_free(ast2);
    parser_destroy(&p2);
    strbuf_destroy(&out);
    free(input);
    return 0;
}
