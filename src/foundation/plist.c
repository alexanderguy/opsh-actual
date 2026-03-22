#include "foundation/plist.h"

#include "foundation/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLIST_INITIAL_CAP 16

void plist_init(plist_t *list)
{
    list->capacity = PLIST_INITIAL_CAP;
    list->contents = xmalloc(list->capacity * sizeof(void *));
    list->length = 0;
}

void plist_destroy(plist_t *list)
{
    free(list->contents);
    list->contents = NULL;
    list->length = 0;
    list->capacity = 0;
}

static void plist_grow(plist_t *list)
{
    size_t new_cap;
    if (checked_mul(list->capacity, 2, &new_cap) != 0) {
        fprintf(stderr, "opsh: plist overflow\n");
        abort();
    }
    size_t byte_size;
    if (checked_mul(new_cap, sizeof(void *), &byte_size) != 0) {
        fprintf(stderr, "opsh: plist overflow\n");
        abort();
    }
    list->contents = xrealloc(list->contents, byte_size);
    list->capacity = new_cap;
}

void plist_add(plist_t *list, void *ptr)
{
    if (list->length >= list->capacity) {
        plist_grow(list);
    }
    list->contents[list->length++] = ptr;
}

void *plist_remove(plist_t *list, size_t index)
{
    void *removed = list->contents[index];
    size_t tail = list->length - index - 1;
    if (tail > 0) {
        memmove(&list->contents[index], &list->contents[index + 1], tail * sizeof(void *));
    }
    list->length--;
    return removed;
}

void plist_clear(plist_t *list)
{
    list->length = 0;
}
