#include "../tap.h"
#include "foundation/util.h"

#include <limits.h>
#include <stdlib.h>

int main(void)
{
    tap_plan(11);

    /* xmalloc */
    {
        void *p = xmalloc(128);
        tap_ok(p != NULL, "xmalloc: returns non-NULL");
        free(p);
    }

    /* xcalloc */
    {
        char *p = xcalloc(16, 1);
        tap_ok(p != NULL, "xcalloc: returns non-NULL");
        tap_is_int(p[0], 0, "xcalloc: memory is zeroed");
        free(p);
    }

    /* xrealloc */
    {
        void *p = xmalloc(16);
        p = xrealloc(p, 256);
        tap_ok(p != NULL, "xrealloc: returns non-NULL");
        free(p);
    }

    /* checked_add: normal case */
    {
        size_t result;
        tap_is_int(checked_add(10, 20, &result), 0, "checked_add: no overflow");
        tap_is_int((long long)result, 30, "checked_add: correct result");
    }

    /* checked_add: overflow */
    {
        size_t result;
        tap_is_int(checked_add(SIZE_MAX, 1, &result), -1, "checked_add: overflow detected");
    }

    /* checked_mul: normal case */
    {
        size_t result;
        tap_is_int(checked_mul(100, 200, &result), 0, "checked_mul: no overflow");
        tap_is_int((long long)result, 20000, "checked_mul: correct result");
    }

    /* checked_mul: overflow */
    {
        size_t result;
        tap_is_int(checked_mul(SIZE_MAX, 2, &result), -1, "checked_mul: overflow detected");
    }

    /* refcount */
    {
        refcount_t rc;
        refcount_init(&rc);
        tap_is_int(refcount_get(&rc), 1, "refcount: init to 1");
        /* inc/dec tested implicitly -- they're trivial inlines */
    }

    return tap_done();
}
