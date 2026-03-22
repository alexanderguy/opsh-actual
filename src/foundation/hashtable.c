#include "foundation/hashtable.h"

#include "foundation/util.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HT_INITIAL_CAP 16
#define HT_LOAD_FACTOR_NUM 3
#define HT_LOAD_FACTOR_DEN 4

uint32_t ht_fnv1a(const char *key)
{
    uint32_t hash = 2166136261u;
    while (*key) {
        hash ^= (uint8_t)*key++;
        hash *= 16777619u;
    }
    return hash;
}

void ht_init(hashtable_t *ht)
{
    ht->capacity = HT_INITIAL_CAP;
    ht->entries = xcalloc(ht->capacity, sizeof(ht_entry_t));
    ht->count = 0;
}

void ht_destroy(hashtable_t *ht)
{
    free(ht->entries);
    ht->entries = NULL;
    ht->capacity = 0;
    ht->count = 0;
}

static void ht_resize(hashtable_t *ht)
{
    size_t old_cap = ht->capacity;
    ht_entry_t *old_entries = ht->entries;
    size_t i;

    if (checked_mul(old_cap, 2, &ht->capacity) != 0) {
        fprintf(stderr, "opsh: hashtable capacity overflow\n");
        abort();
    }
    ht->entries = xcalloc(ht->capacity, sizeof(ht_entry_t));
    ht->count = 0;

    for (i = 0; i < old_cap; i++) {
        if (old_entries[i].key != NULL) {
            ht_set(ht, old_entries[i].key, old_entries[i].value);
        }
    }

    free(old_entries);
}

void *ht_set(hashtable_t *ht, const char *key, void *value)
{
    uint32_t hash;
    size_t mask;
    size_t idx;
    ht_entry_t *e;

    if (ht->count * HT_LOAD_FACTOR_DEN >= ht->capacity * HT_LOAD_FACTOR_NUM) {
        ht_resize(ht);
    }

    hash = ht_fnv1a(key);
    mask = ht->capacity - 1;
    idx = hash & mask;

    for (;;) {
        e = &ht->entries[idx];
        if (e->key == NULL) {
            e->key = key;
            e->value = value;
            e->hash = hash;
            ht->count++;
            return NULL;
        }
        if (e->hash == hash && strcmp(e->key, key) == 0) {
            void *old_value = e->value;
            e->value = value;
            return old_value;
        }
        idx = (idx + 1) & mask;
    }
}

void *ht_get(const hashtable_t *ht, const char *key)
{
    uint32_t hash = ht_fnv1a(key);
    size_t mask = ht->capacity - 1;
    size_t idx = hash & mask;

    for (;;) {
        ht_entry_t *e = &ht->entries[idx];
        if (e->key == NULL) {
            return NULL;
        }
        if (e->hash == hash && strcmp(e->key, key) == 0) {
            return e->value;
        }
        idx = (idx + 1) & mask;
    }
}

/*
 * Backward-shift deletion: after removing an entry, shift subsequent
 * entries backward to fill the gap, keeping probe chains intact.
 * This eliminates tombstones entirely.
 */
void *ht_remove(hashtable_t *ht, const char *key)
{
    uint32_t hash = ht_fnv1a(key);
    size_t mask = ht->capacity - 1;
    size_t idx = hash & mask;

    for (;;) {
        ht_entry_t *e = &ht->entries[idx];
        if (e->key == NULL) {
            return NULL;
        }
        if (e->hash == hash && strcmp(e->key, key) == 0) {
            void *value = e->value;
            ht->count--;

            /* Backward-shift: scan forward from the hole, shifting
             * displaced entries back to maintain probe chain integrity.
             * The scan continues until an empty slot is reached. */
            {
                size_t scan = (idx + 1) & mask;
                for (;;) {
                    ht_entry_t *ne = &ht->entries[scan];
                    if (ne->key == NULL) {
                        break;
                    }
                    size_t natural = ne->hash & mask;
                    /* Should this entry be shifted into the hole?
                     * Yes if its natural slot is "at or before" the hole
                     * in the circular probe order from scan's perspective. */
                    bool shift;
                    if (scan > idx) {
                        /* No wraparound between hole and scan */
                        shift = (natural <= idx) || (natural > scan);
                    } else {
                        /* Wraparound: hole is ahead of scan circularly */
                        shift = (natural > scan) && (natural <= idx);
                    }
                    if (shift) {
                        ht->entries[idx] = *ne;
                        idx = scan;
                    }
                    scan = (scan + 1) & mask;
                }
            }
            ht->entries[idx].key = NULL;
            ht->entries[idx].value = NULL;
            return value;
        }
        idx = (idx + 1) & mask;
    }
}

void ht_foreach(const hashtable_t *ht, int (*fn)(const char *key, void *value, void *ctx),
                void *ctx)
{
    size_t i;
    for (i = 0; i < ht->capacity; i++) {
        ht_entry_t *e = &ht->entries[i];
        if (e->key != NULL) {
            if (fn(e->key, e->value, ctx) != 0) {
                return;
            }
        }
    }
}
