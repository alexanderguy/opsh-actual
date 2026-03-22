#include "../tap.h"
#include "foundation/hashtable.h"

#include <stdio.h>

static int count_cb(const char *key, void *value, void *ctx)
{
    (void)key;
    (void)value;
    (*(int *)ctx)++;
    return 0;
}

int main(void)
{
    hashtable_t ht;
    int val1 = 1, val2 = 2, val3 = 3, val4 = 4;
    int count;

    tap_plan(19);

    /* FNV-1a smoke test: known strings should produce different hashes */
    {
        uint32_t h1 = ht_fnv1a("hello");
        uint32_t h2 = ht_fnv1a("world");
        uint32_t h3 = ht_fnv1a("");
        tap_ok(h1 != h2, "fnv1a: different strings different hashes");
        tap_ok(h3 != h1, "fnv1a: empty string has distinct hash");
        tap_is_int((long long)ht_fnv1a("hello"), (long long)h1, "fnv1a: deterministic");
    }

    ht_init(&ht);
    tap_is_int((long long)ht_count(&ht), 0, "init: count is 0");

    /* Insert */
    tap_ok(ht_set(&ht, "alpha", &val1) == NULL, "set: new key returns NULL");
    tap_ok(ht_set(&ht, "beta", &val2) == NULL, "set: another new key returns NULL");
    tap_is_int((long long)ht_count(&ht), 2, "set: count is 2");

    /* Get */
    tap_ok(ht_get(&ht, "alpha") == &val1, "get: alpha returns val1");
    tap_ok(ht_get(&ht, "beta") == &val2, "get: beta returns val2");
    tap_ok(ht_get(&ht, "gamma") == NULL, "get: missing key returns NULL");

    /* Update */
    tap_ok(ht_set(&ht, "alpha", &val3) == &val1, "set: update returns old value");
    tap_ok(ht_get(&ht, "alpha") == &val3, "set: update changes value");

    /* Remove */
    tap_ok(ht_remove(&ht, "beta") == &val2, "remove: returns value");
    tap_ok(ht_get(&ht, "beta") == NULL, "remove: key no longer found");
    tap_is_int((long long)ht_count(&ht), 1, "remove: count decremented");

    /* Foreach */
    ht_set(&ht, "delta", &val4);
    count = 0;
    ht_foreach(&ht, count_cb, &count);
    tap_is_int(count, 2, "foreach: visits all entries");

    ht_destroy(&ht);

    /* Tombstone stress test: heavy insert/remove cycles */
    {
        hashtable_t ht2;
        int i;
        char keys[64][8];
        int vals[64];

        ht_init(&ht2);

        /* Fill and remove repeatedly to accumulate tombstones */
        for (i = 0; i < 64; i++) {
            snprintf(keys[i], sizeof(keys[i]), "k%d", i);
            vals[i] = i;
        }

        /* Insert 10, remove 8, insert 10, remove 8 ... */
        for (i = 0; i < 40; i++) {
            ht_set(&ht2, keys[i], &vals[i]);
        }
        for (i = 0; i < 32; i++) {
            ht_remove(&ht2, keys[i]);
        }
        /* Table now has 8 live entries and 32 tombstones */

        /* Insert more -- this should trigger a resize that cleans tombstones */
        for (i = 0; i < 32; i++) {
            ht_set(&ht2, keys[i], &vals[i]);
        }

        tap_is_int((long long)ht_count(&ht2), 40, "tombstone: count correct after churn");

        /* Verify all keys are findable (would infinite-loop before fix) */
        {
            int found = 0;
            for (i = 0; i < 40; i++) {
                if (ht_get(&ht2, keys[i]) == &vals[i]) {
                    found++;
                }
            }
            tap_is_int(found, 40, "tombstone: all keys findable after churn");
        }

        /* Verify nonexistent key lookup terminates */
        tap_ok(ht_get(&ht2, "nonexistent") == NULL, "tombstone: missing key returns NULL");

        ht_destroy(&ht2);
    }

    return tap_done();
}
