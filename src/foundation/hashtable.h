#ifndef OPSH_FOUNDATION_HASHTABLE_H
#define OPSH_FOUNDATION_HASHTABLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct ht_entry {
    const char *key;
    void *value;
    uint32_t hash;
} ht_entry_t;

/*
 * Open-addressing hash table with FNV-1a hashing.
 *
 * Keys are borrowed (not copied). Caller must ensure keys outlive the table.
 * Values are opaque pointers; the table does not own them.
 */
typedef struct {
    ht_entry_t *entries;
    size_t capacity;
    size_t count;
} hashtable_t;

/* Initialize with default capacity */
void ht_init(hashtable_t *ht);

/* Free internal storage (does NOT free keys or values) */
void ht_destroy(hashtable_t *ht);

/* Insert or update. Returns the previous value if key existed, else NULL. */
void *ht_set(hashtable_t *ht, const char *key, void *value);

/* Look up a key. Returns the value, or NULL if not found. */
void *ht_get(const hashtable_t *ht, const char *key);

/* Remove a key. Returns the value, or NULL if not found. */
void *ht_remove(hashtable_t *ht, const char *key);

/* Number of entries */
static inline size_t ht_count(const hashtable_t *ht)
{
    return ht->count;
}

/* Iteration: call `fn` for each entry. If `fn` returns non-zero, stop early. */
void ht_foreach(const hashtable_t *ht, int (*fn)(const char *key, void *value, void *ctx),
                void *ctx);

/* FNV-1a hash (exposed for testing) */
uint32_t ht_fnv1a(const char *key);

#endif /* OPSH_FOUNDATION_HASHTABLE_H */
