#include "foundation/strbuf.h"

#include "foundation/util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRBUF_INITIAL_CAP 64

void strbuf_init(strbuf_t *buf)
{
    strbuf_init_with_capacity(buf, STRBUF_INITIAL_CAP);
}

void strbuf_init_with_capacity(strbuf_t *buf, size_t cap)
{
    if (cap < 16) {
        cap = 16;
    }
    buf->contents = xmalloc(cap);
    buf->contents[0] = '\0';
    buf->length = 0;
    buf->capacity = cap;
}

void strbuf_destroy(strbuf_t *buf)
{
    free(buf->contents);
    buf->contents = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

void strbuf_ensure_capacity(strbuf_t *buf, size_t additional)
{
    size_t needed;
    size_t new_cap;

    if (checked_add(buf->length, additional, &needed) != 0) {
        fprintf(stderr, "opsh: strbuf overflow\n");
        abort();
    }
    /* +1 for null terminator */
    if (checked_add(needed, 1, &needed) != 0) {
        fprintf(stderr, "opsh: strbuf overflow\n");
        abort();
    }

    if (needed <= buf->capacity) {
        return;
    }

    new_cap = buf->capacity;
    while (new_cap < needed) {
        if (checked_mul(new_cap, 2, &new_cap) != 0) {
            new_cap = needed;
            break;
        }
    }

    buf->contents = xrealloc(buf->contents, new_cap);
    buf->capacity = new_cap;
}

void strbuf_append_byte(strbuf_t *buf, char c)
{
    strbuf_ensure_capacity(buf, 1);
    buf->contents[buf->length++] = c;
    buf->contents[buf->length] = '\0';
}

void strbuf_append_str(strbuf_t *buf, const char *s)
{
    size_t len = strlen(s);
    strbuf_append_bytes(buf, s, len);
}

void strbuf_append_bytes(strbuf_t *buf, const char *s, size_t n)
{
    strbuf_ensure_capacity(buf, n);
    memcpy(buf->contents + buf->length, s, n);
    buf->length += n;
    buf->contents[buf->length] = '\0';
}

void strbuf_append_printf(strbuf_t *buf, const char *fmt, ...)
{
    va_list ap, ap2;
    int needed;

    va_start(ap, fmt);
    va_copy(ap2, ap);

    needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (needed < 0) {
        va_end(ap2);
        return;
    }

    strbuf_ensure_capacity(buf, (size_t)needed);
    vsnprintf(buf->contents + buf->length, (size_t)needed + 1, fmt, ap2);
    buf->length += (size_t)needed;
    va_end(ap2);
}

void strbuf_clear(strbuf_t *buf)
{
    buf->length = 0;
    buf->contents[0] = '\0';
}

char *strbuf_detach(strbuf_t *buf)
{
    char *result = buf->contents;
    buf->contents = NULL;
    buf->length = 0;
    buf->capacity = 0;
    return result;
}

size_t utf8_strlen(const char *s)
{
    size_t count = 0;
    while (*s) {
        /* Count bytes that are NOT continuation bytes (10xxxxxx) */
        if ((*s & 0xC0) != 0x80) {
            count++;
        }
        s++;
    }
    return count;
}

size_t strbuf_utf8_len(const strbuf_t *buf)
{
    return utf8_strlen(buf->contents);
}
