#ifndef OPSH_FOUNDATION_ARENA_H
#define OPSH_FOUNDATION_ARENA_H

#include <stddef.h>

/*
 * Bump-pointer arena allocator.
 *
 * All allocations come from contiguous blocks. Individual allocations
 * cannot be freed; the entire arena is freed at once with arena_destroy.
 * Ideal for parse trees that are built and then discarded as a unit.
 */

#define ARENA_BLOCK_SIZE 4096

typedef struct arena_block {
    struct arena_block *next;
    size_t used;
    size_t capacity;
    char data[];
} arena_block_t;

typedef struct {
    arena_block_t *current;
    arena_block_t *head; /* first block, for iteration during destroy */
} arena_t;

/* Initialize an arena (allocates the first block) */
void arena_init(arena_t *a);

/* Destroy an arena and free all blocks */
void arena_destroy(arena_t *a);

/* Allocate `size` bytes from the arena (8-byte aligned). Never fails (aborts on OOM). */
void *arena_alloc(arena_t *a, size_t size);

/* Allocate `size` zero-initialized bytes from the arena. */
void *arena_calloc(arena_t *a, size_t size);

/* Duplicate a string into the arena */
char *arena_strdup(arena_t *a, const char *s);

/* Duplicate a buffer of known length into the arena (adds NUL terminator) */
char *arena_strndup(arena_t *a, const char *s, size_t len);

#endif /* OPSH_FOUNDATION_ARENA_H */
