#ifndef OPSH_FOUNDATION_PLIST_H
#define OPSH_FOUNDATION_PLIST_H

#include <stddef.h>

/*
 * Growable pointer array.
 *
 * `length` is the number of elements. Use `length` for iteration,
 * not a NULL sentinel.
 */
typedef struct {
    void **contents;
    size_t length;
    size_t capacity;
} plist_t;

/* Initialize with default capacity */
void plist_init(plist_t *list);

/* Free the array (does NOT free pointed-to elements) */
void plist_destroy(plist_t *list);

/* Append a pointer */
void plist_add(plist_t *list, void *ptr);

/* Remove element at index, shifting subsequent elements down. Returns removed pointer. */
void *plist_remove(plist_t *list, size_t index);

/* Get element at index (no bounds check) */
static inline void *plist_get(const plist_t *list, size_t index)
{
    return list->contents[index];
}

/* Clear all elements (does NOT free them) */
void plist_clear(plist_t *list);

#endif /* OPSH_FOUNDATION_PLIST_H */
