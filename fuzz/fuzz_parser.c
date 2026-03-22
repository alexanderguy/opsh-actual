/*
 * libfuzzer harness for the opsh parser.
 *
 * Feeds arbitrary byte sequences as shell source to the lexer and parser.
 * Catches crashes, memory errors (via ASan), and undefined behavior (via UBSan).
 */
#include "parser/parser.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Parser expects a null-terminated string */
    char *input = malloc(size + 1);
    if (!input)
        return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    parser_t p;
    parser_init(&p, input, "<fuzz>");

    sh_list_t *ast = parser_parse(&p);
    if (ast)
        sh_list_free(ast);

    parser_destroy(&p);
    free(input);
    return 0;
}
