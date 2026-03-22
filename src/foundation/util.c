#include "foundation/util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t size)
{
    void *p = malloc(size);
    if (p == NULL && size > 0) {
        fprintf(stderr, "opsh: out of memory (malloc %zu bytes)\n", size);
        abort();
    }
    return p;
}

void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (p == NULL && size > 0) {
        fprintf(stderr, "opsh: out of memory (realloc %zu bytes)\n", size);
        abort();
    }
    return p;
}

void *xcalloc(size_t count, size_t size)
{
    void *p = calloc(count, size);
    if (p == NULL && count > 0 && size > 0) {
        fprintf(stderr, "opsh: out of memory (calloc %zu * %zu bytes)\n", count, size);
        abort();
    }
    return p;
}

int checked_add(size_t a, size_t b, size_t *result)
{
    if (a > SIZE_MAX - b) {
        return -1;
    }
    *result = a + b;
    return 0;
}

int checked_mul(size_t a, size_t b, size_t *result)
{
    if (a != 0 && b > SIZE_MAX / a) {
        return -1;
    }
    *result = a * b;
    return 0;
}
