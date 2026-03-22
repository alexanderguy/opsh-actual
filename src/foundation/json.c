#include "foundation/json.h"

#include <inttypes.h>
#include <stdio.h>

/* Track whether we need a comma before the next key/value */
static void json_maybe_comma(strbuf_t *buf)
{
    if (buf->length > 0) {
        char last = buf->contents[buf->length - 1];
        if (last != '{' && last != '[' && last != ':') {
            strbuf_append_byte(buf, ',');
        }
    }
}

void json_write_string(strbuf_t *buf, const char *s)
{
    strbuf_append_byte(buf, '"');
    while (*s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':
            strbuf_append_str(buf, "\\\"");
            break;
        case '\\':
            strbuf_append_str(buf, "\\\\");
            break;
        case '\b':
            strbuf_append_str(buf, "\\b");
            break;
        case '\f':
            strbuf_append_str(buf, "\\f");
            break;
        case '\n':
            strbuf_append_str(buf, "\\n");
            break;
        case '\r':
            strbuf_append_str(buf, "\\r");
            break;
        case '\t':
            strbuf_append_str(buf, "\\t");
            break;
        default:
            if (c < 0x20) {
                strbuf_append_printf(buf, "\\u%04x", c);
            } else {
                strbuf_append_byte(buf, (char)c);
            }
            break;
        }
        s++;
    }
    strbuf_append_byte(buf, '"');
}

void json_begin_object(strbuf_t *buf)
{
    json_maybe_comma(buf);
    strbuf_append_byte(buf, '{');
}

void json_end_object(strbuf_t *buf)
{
    strbuf_append_byte(buf, '}');
}

void json_key_string(strbuf_t *buf, const char *key, const char *value)
{
    json_maybe_comma(buf);
    json_write_string(buf, key);
    strbuf_append_byte(buf, ':');
    json_write_string(buf, value);
}

void json_key_int(strbuf_t *buf, const char *key, int64_t value)
{
    json_maybe_comma(buf);
    json_write_string(buf, key);
    strbuf_append_printf(buf, ":%" PRId64, value);
}

void json_key_bool(strbuf_t *buf, const char *key, int value)
{
    json_maybe_comma(buf);
    json_write_string(buf, key);
    strbuf_append_str(buf, value ? ":true" : ":false");
}

void json_key_null(strbuf_t *buf, const char *key)
{
    json_maybe_comma(buf);
    json_write_string(buf, key);
    strbuf_append_str(buf, ":null");
}

void json_begin_array(strbuf_t *buf)
{
    json_maybe_comma(buf);
    strbuf_append_byte(buf, '[');
}

void json_end_array(strbuf_t *buf)
{
    strbuf_append_byte(buf, ']');
}
