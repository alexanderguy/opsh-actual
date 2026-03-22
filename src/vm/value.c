#include "opsh/value.h"

#include "foundation/rcstr.h"
#include "foundation/util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void value_destroy(value_t *v)
{
    int i;

    switch (v->type) {
    case VT_STRING:
        rcstr_release(v->data.string);
        v->data.string = NULL;
        break;
    case VT_ARRAY:
        for (i = 0; i < v->data.array.count; i++) {
            rcstr_release(v->data.array.elements[i]);
        }
        free(v->data.array.elements);
        v->data.array.elements = NULL;
        v->data.array.count = 0;
        break;
    case VT_ITERATOR:
        for (i = 0; i < v->data.iterator.count; i++) {
            rcstr_release(v->data.iterator.elements[i]);
        }
        free(v->data.iterator.elements);
        v->data.iterator.elements = NULL;
        v->data.iterator.count = 0;
        break;
    case VT_NONE:
    case VT_INTEGER:
        break;
    }
    v->type = VT_NONE;
}

value_t value_clone(const value_t *v)
{
    value_t result;
    int i;

    result.type = v->type;

    switch (v->type) {
    case VT_STRING:
        result.data.string = rcstr_retain(v->data.string);
        break;
    case VT_INTEGER:
        result.data.integer = v->data.integer;
        break;
    case VT_ARRAY:
        result.data.array.count = v->data.array.count;
        result.data.array.elements = xmalloc((size_t)v->data.array.count * sizeof(char *));
        for (i = 0; i < v->data.array.count; i++) {
            result.data.array.elements[i] = rcstr_retain(v->data.array.elements[i]);
        }
        break;
    case VT_ITERATOR:
        result.data.iterator.count = v->data.iterator.count;
        result.data.iterator.position = v->data.iterator.position;
        result.data.iterator.elements = xmalloc((size_t)v->data.iterator.count * sizeof(char *));
        for (i = 0; i < v->data.iterator.count; i++) {
            result.data.iterator.elements[i] = rcstr_retain(v->data.iterator.elements[i]);
        }
        break;
    case VT_NONE:
        break;
    }

    return result;
}

char *value_to_string(const value_t *v)
{
    char buf[32];

    switch (v->type) {
    case VT_STRING:
        return rcstr_retain(v->data.string);
    case VT_INTEGER:
        snprintf(buf, sizeof(buf), "%" PRId64, v->data.integer);
        return rcstr_new(buf);
    case VT_NONE:
    case VT_ARRAY:
    case VT_ITERATOR:
        return rcstr_new("");
    }
    return rcstr_new("");
}

int64_t value_to_integer(const value_t *v)
{
    switch (v->type) {
    case VT_INTEGER:
        return v->data.integer;
    case VT_STRING:
        return strtoll(v->data.string, NULL, 10);
    default:
        return 0;
    }
}
