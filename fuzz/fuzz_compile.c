/*
 * libfuzzer harness for the opsh parser + compiler pipeline.
 *
 * Parses arbitrary input and, if it produces an AST, compiles it to bytecode.
 * Does NOT execute the bytecode (the VM can fork/exec, which is hostile to fuzzing).
 */
#include "compiler/compiler.h"
#include "parser/parser.h"
#include "vm/vm.h"

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
    if (ast && parser_error_count(&p) == 0) {
        bytecode_image_t *img = compile(ast, "<fuzz>");
        if (img)
            image_free(img);
    }

    if (ast)
        sh_list_free(ast);

    parser_destroy(&p);
    free(input);
    return 0;
}
