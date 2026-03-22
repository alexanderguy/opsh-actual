#include "foundation/arena.h"

#include "foundation/util.h"

#include <stdlib.h>
#include <string.h>

static arena_block_t *arena_new_block(size_t min_size)
{
    size_t cap = ARENA_BLOCK_SIZE;
    if (min_size > cap) {
        cap = min_size;
    }
    arena_block_t *b = xmalloc(sizeof(arena_block_t) + cap);
    b->next = NULL;
    b->used = 0;
    b->capacity = cap;
    return b;
}

void arena_init(arena_t *a)
{
    a->head = arena_new_block(0);
    a->current = a->head;
}

void arena_destroy(arena_t *a)
{
    arena_block_t *b = a->head;
    while (b != NULL) {
        arena_block_t *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
    a->current = NULL;
}

void *arena_alloc(arena_t *a, size_t size)
{
    void *ptr;
    arena_block_t *b;

    /* 8-byte alignment */
    size = (size + 7) & ~(size_t)7;

    b = a->current;
    if (b->used + size > b->capacity) {
        arena_block_t *nb = arena_new_block(size);
        b->next = nb;
        a->current = nb;
        b = nb;
    }

    ptr = b->data + b->used;
    b->used += size;
    return ptr;
}

void *arena_calloc(arena_t *a, size_t size)
{
    void *ptr = arena_alloc(a, size);
    memset(ptr, 0, size);
    return ptr;
}

char *arena_strdup(arena_t *a, const char *s)
{
    size_t len = strlen(s);
    return arena_strndup(a, s, len);
}

char *arena_strndup(arena_t *a, const char *s, size_t len)
{
    char *copy = arena_alloc(a, len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}
