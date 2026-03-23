/*
 * libfuzzer harness for the JSON read helpers.
 *
 * Exercises json_get_string, json_get_int, and json_find_nested_string
 * with arbitrary input to find crashes in the hand-rolled JSON parser.
 */
#include "foundation/json.h"

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

    /* Exercise all read helpers with various keys */
    char *s;

    s = json_get_string(input, "method");
    free(s);
    s = json_get_string(input, "id");
    free(s);
    s = json_get_string(input, "");
    free(s);

    json_get_int(input, "id");
    json_get_int(input, "session_id");
    json_get_int(input, "");

    s = json_find_nested_string(input, "uri");
    free(s);
    s = json_find_nested_string(input, "text");
    free(s);
    s = json_find_nested_string(input, "source");
    free(s);

    free(input);
    return 0;
}
