#include "foundation/rcstr.h"

#include "foundation/util.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * Hidden header stored immediately before the string data.
 * The pointer returned to callers points to header->data.
 */
typedef struct {
    int refcount;
    char data[];
} rcstr_header_t;

static rcstr_header_t *rcstr_to_header(char *s)
{
    return (rcstr_header_t *)((char *)s - offsetof(rcstr_header_t, data));
}

char *rcstr_new(const char *src)
{
    size_t len = strlen(src);
    return rcstr_from_buf(src, len);
}

char *rcstr_from_buf(const char *buf, size_t len)
{
    rcstr_header_t *h = xmalloc(sizeof(rcstr_header_t) + len + 1);
    h->refcount = 1;
    memcpy(h->data, buf, len);
    h->data[len] = '\0';
    return h->data;
}

char *rcstr_retain(char *s)
{
    rcstr_header_t *h;
    if (s == NULL) {
        return NULL;
    }
    h = rcstr_to_header(s);
    h->refcount++;
    return s;
}

void rcstr_release(char *s)
{
    rcstr_header_t *h;
    if (s == NULL) {
        return;
    }
    h = rcstr_to_header(s);
    if (--h->refcount == 0) {
        free(h);
    }
}
