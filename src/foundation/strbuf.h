#ifndef OPSH_FOUNDATION_STRBUF_H
#define OPSH_FOUNDATION_STRBUF_H

#include <stddef.h>

/*
 * Growable UTF-8 string buffer.
 *
 * The buffer is always null-terminated. `length` does not include the
 * null terminator. An empty strbuf has length 0 and contents "\0".
 */
typedef struct {
    char *contents;
    size_t length;
    size_t capacity;
} strbuf_t;

/* Initialize with default capacity */
void strbuf_init(strbuf_t *buf);

/* Initialize with at least `cap` bytes of capacity */
void strbuf_init_with_capacity(strbuf_t *buf, size_t cap);

/* Free internal buffer */
void strbuf_destroy(strbuf_t *buf);

/* Append a single byte */
void strbuf_append_byte(strbuf_t *buf, char c);

/* Append a null-terminated string */
void strbuf_append_str(strbuf_t *buf, const char *s);

/* Append `n` bytes from `s` (which need not be null-terminated) */
void strbuf_append_bytes(strbuf_t *buf, const char *s, size_t n);

/* Append a formatted string */
void strbuf_append_printf(strbuf_t *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Clear contents (does not free; capacity is retained) */
void strbuf_clear(strbuf_t *buf);

/* Detach and return the internal buffer; caller owns it. Resets the strbuf. */
char *strbuf_detach(strbuf_t *buf);

/* Ensure at least `additional` more bytes can be appended without realloc */
void strbuf_ensure_capacity(strbuf_t *buf, size_t additional);

/* Count UTF-8 codepoints in the buffer */
size_t strbuf_utf8_len(const strbuf_t *buf);

/* Count UTF-8 codepoints in a plain string */
size_t utf8_strlen(const char *s);

#endif /* OPSH_FOUNDATION_STRBUF_H */
