/*
 * libfuzzer harness for the opsh LSP server's JSON parsing.
 *
 * Calls lsp_handle_message with arbitrary input to exercise the
 * hand-rolled JSON parser, method dispatch, and response generation.
 */
#include "lsp/lsp.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *input = malloc(size + 1);
    if (!input)
        return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    /* Redirect stdout to /dev/null so response writes don't interfere */
    int saved = dup(STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        close(devnull);
    }

    lsp_handle_message(input);

    /* Restore stdout */
    if (saved >= 0) {
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }

    free(input);
    return 0;
}
