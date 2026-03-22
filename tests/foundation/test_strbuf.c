#include "../tap.h"
#include "foundation/strbuf.h"

#include <string.h>

int main(void)
{
    strbuf_t buf;

    tap_plan(16);

    /* Basic init */
    strbuf_init(&buf);
    tap_is_int((long long)buf.length, 0, "init: length is 0");
    tap_is_str(buf.contents, "", "init: contents is empty string");

    /* Append byte */
    strbuf_append_byte(&buf, 'A');
    tap_is_int((long long)buf.length, 1, "append_byte: length is 1");
    tap_is_str(buf.contents, "A", "append_byte: contents is A");

    /* Append string */
    strbuf_append_str(&buf, "BCD");
    tap_is_int((long long)buf.length, 4, "append_str: length is 4");
    tap_is_str(buf.contents, "ABCD", "append_str: contents is ABCD");

    /* Append bytes */
    strbuf_append_bytes(&buf, "EFG", 2);
    tap_is_str(buf.contents, "ABCDEF", "append_bytes: partial append");

    /* Clear */
    strbuf_clear(&buf);
    tap_is_int((long long)buf.length, 0, "clear: length is 0");
    tap_is_str(buf.contents, "", "clear: contents is empty");
    tap_ok(buf.capacity > 0, "clear: capacity retained");

    /* Printf */
    strbuf_append_printf(&buf, "hello %d %s", 42, "world");
    tap_is_str(buf.contents, "hello 42 world", "printf: formatted correctly");

    /* Detach */
    strbuf_append_str(&buf, "!");
    {
        char *detached = strbuf_detach(&buf);
        tap_is_str(detached, "hello 42 world!", "detach: returns contents");
        tap_ok(buf.contents == NULL, "detach: buf is reset");
        free(detached);
    }

    /* UTF-8 codepoint counting */
    strbuf_init(&buf);
    strbuf_append_str(&buf, "hello");
    tap_is_int((long long)strbuf_utf8_len(&buf), 5, "utf8_len: ASCII");

    strbuf_clear(&buf);
    /* U+00E9 (e-acute) = 0xC3 0xA9 (2 bytes), U+2603 (snowman) = 0xE2 0x98 0x83 (3 bytes) */
    strbuf_append_str(&buf, "\xC3\xA9\xE2\x98\x83");
    tap_is_int((long long)strbuf_utf8_len(&buf), 2, "utf8_len: multi-byte");
    tap_is_int((long long)buf.length, 5, "utf8_len: byte length is 5");

    strbuf_destroy(&buf);

    return tap_done();
}
