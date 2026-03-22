#ifndef OPSH_FOUNDATION_RCSTR_H
#define OPSH_FOUNDATION_RCSTR_H

#include <stddef.h>

/*
 * Reference-counted string.
 *
 * The refcount is stored in a hidden header before the string data.
 * The returned pointer is a valid null-terminated char* that can be
 * passed to strlen, strcmp, printf, etc. Only creation, retain, and
 * release need to know about the header.
 *
 * Strings created with rcstr are immutable by convention. Do not
 * modify the contents through the returned pointer if the refcount
 * may be greater than 1.
 */

/* Create a new refcounted string (copies src). Refcount starts at 1. */
char *rcstr_new(const char *src);

/* Create from a buffer of known length (copies len bytes + NUL). */
char *rcstr_from_buf(const char *buf, size_t len);

/* Increment the refcount and return the same pointer. */
char *rcstr_retain(char *s);

/* Decrement the refcount. Frees when it reaches zero. */
void rcstr_release(char *s);

#endif /* OPSH_FOUNDATION_RCSTR_H */
