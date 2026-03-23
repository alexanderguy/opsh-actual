/*
 * libfuzzer harness for the opsh arithmetic evaluator.
 *
 * Feeds arbitrary strings to arith_eval with a live environment.
 * Exercises operator precedence, variable resolution, assignment
 * operators, and recursive evaluation.
 */
#include "exec/variable.h"
#include "foundation/rcstr.h"
#include "vm/arith.h"

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

    environ_t *env = environ_new(NULL, false);

    /* Seed a few variables so variable resolution paths are exercised */
    environ_set(env, "x", value_string(rcstr_new("42")));
    environ_set(env, "y", value_string(rcstr_new("7")));
    environ_set(env, "empty", value_string(rcstr_new("")));

    arith_error_t err = ARITH_OK;
    (void)arith_eval(input, env, &err);

    environ_destroy(env);
    free(input);
    return 0;
}
