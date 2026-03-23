/*
 * libfuzzer harness for the opsh VM (fork-disabled).
 *
 * Full pipeline: parse -> compile -> execute with vm.no_fork = true.
 * Fork-dependent opcodes (EXEC_SIMPLE external commands, PIPELINE,
 * CMD_SUBST, SUBSHELL, BACKGROUND) skip the fork and return status 127.
 * All non-forking VM paths are exercised: stack ops, variables, arithmetic,
 * parameter expansion, field splitting, test operations, control flow.
 */
#include "compiler/compiler.h"
#include "parser/parser.h"
#include "vm/vm.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 512)
        return 0;

    char *input = malloc(size + 1);
    if (!input)
        return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    parser_t p;
    parser_init(&p, input, "<fuzz>");
    sh_list_t *ast = parser_parse(&p);

    if (ast != NULL && parser_error_count(&p) == 0) {
        bytecode_image_t *img = compile(ast, "<fuzz>");
        if (img != NULL) {
            int saved_out = dup(STDOUT_FILENO);
            int saved_err = dup(STDERR_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }

            vm_t vm;
            vm_init(&vm, img);
            vm.no_fork = true;
            vm_run(&vm);
            vm_destroy(&vm);
            image_free(img);

            if (saved_out >= 0) {
                dup2(saved_out, STDOUT_FILENO);
                close(saved_out);
            }
            if (saved_err >= 0) {
                dup2(saved_err, STDERR_FILENO);
                close(saved_err);
            }
        }
    }

    sh_list_free(ast);
    parser_destroy(&p);
    free(input);
    return 0;
}
