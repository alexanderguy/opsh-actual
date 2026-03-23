/*
 * libfuzzer harness for foundation data structures.
 *
 * Exercises strbuf, hashtable, and plist with arbitrary input.
 */
#include "foundation/hashtable.h"
#include "foundation/plist.h"
#include "foundation/strbuf.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;

    /* Use first byte to select operation mix */
    uint8_t mode = data[0];
    const uint8_t *payload = data + 1;
    size_t plen = size - 1;

    /* strbuf: append arbitrary bytes, then detach */
    {
        strbuf_t sb;
        strbuf_init(&sb);
        strbuf_append_bytes(&sb, (const char *)payload, plen);
        if (sb.length > 0) {
            /* Exercise printf path */
            strbuf_append_printf(&sb, "%zu", sb.length);
        }
        char *s = strbuf_detach(&sb);
        free(s);
    }

    /* hashtable: insert, lookup, remove with fuzz-derived keys */
    if (mode & 1) {
        hashtable_t ht;
        ht_init(&ht);
        /* Track keys for cleanup since ht_destroy doesn't free them */
        char *owned_keys[128];
        int nkeys = 0;

        size_t i = 0;
        while (i + 2 <= plen && nkeys < 128) {
            uint8_t klen = payload[i];
            i++;
            if (klen == 0)
                klen = 1;
            if (i + klen > plen)
                break;

            char *key = malloc((size_t)klen + 1);
            memcpy(key, payload + i, klen);
            key[klen] = '\0';
            i += klen;

            if (i < plen && payload[i] & 1) {
                ht_set(&ht, key, (void *)(uintptr_t)(i + 1));
                owned_keys[nkeys++] = key;
            } else {
                ht_get(&ht, key);
                ht_remove(&ht, key);
                free(key);
            }
        }

        ht_destroy(&ht);
        {
            int ki;
            for (ki = 0; ki < nkeys; ki++) {
                free(owned_keys[ki]);
            }
        }
    }

    /* plist: add and access with fuzz-derived indices */
    if (mode & 2) {
        plist_t pl;
        plist_init(&pl);

        size_t i;
        for (i = 0; i < plen && i < 256; i++) {
            plist_add(&pl, (void *)(uintptr_t)(payload[i] + 1));
        }
        for (i = 0; i < pl.length; i++) {
            (void)plist_get(&pl, i);
        }

        plist_destroy(&pl);
    }

    return 0;
}
