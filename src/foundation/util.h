#ifndef OPSH_FOUNDATION_UTIL_H
#define OPSH_FOUNDATION_UTIL_H

#include <stddef.h>

/* Allocators that abort on failure */
void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
void *xcalloc(size_t count, size_t size);
char *xstrdup(const char *s);

/* Read entire file into a malloc'd string. Returns NULL on failure. */
char *read_file(const char *path);

/* Read all of stdin into a malloc'd string. */
char *read_stdin(void);

/* Overflow-safe arithmetic -- returns 0 on success, -1 on overflow */
int checked_add(size_t a, size_t b, size_t *result);
int checked_mul(size_t a, size_t b, size_t *result);

/* Reference counting */
typedef struct {
    int count;
} refcount_t;

static inline void refcount_init(refcount_t *rc)
{
    rc->count = 1;
}

static inline void refcount_inc(refcount_t *rc)
{
    rc->count++;
}

/* Returns 1 if the count reached zero (caller should free) */
static inline int refcount_dec(refcount_t *rc)
{
    return --rc->count == 0;
}

static inline int refcount_get(const refcount_t *rc)
{
    return rc->count;
}

#endif /* OPSH_FOUNDATION_UTIL_H */
