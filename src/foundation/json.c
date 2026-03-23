#include "foundation/json.h"
#include "foundation/strbuf.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* --- JSON read helpers --- */

static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

static const char *json_skip_value(const char *p)
{
    p = json_skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\') {
                p++;
            }
            if (*p) {
                p++;
            }
        }
        if (*p == '"') {
            p++;
        }
        return p;
    }
    if (*p == '{') {
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '{') {
                depth++;
            } else if (*p == '}') {
                depth--;
            } else if (*p == '"') {
                p = json_skip_value(p);
                continue;
            }
            p++;
        }
        return p;
    }
    if (*p == '[') {
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '[') {
                depth++;
            } else if (*p == ']') {
                depth--;
            } else if (*p == '"') {
                p = json_skip_value(p);
                continue;
            }
            p++;
        }
        return p;
    }
    /* number, bool, null */
    while (*p && *p != ',' && *p != '}' && *p != ']') {
        p++;
    }
    return p;
}

/* Decode a JSON string escape into buf. p points after the backslash. */
static void json_decode_escape(strbuf_t *buf, char c)
{
    switch (c) {
    case 'n':
        strbuf_append_byte(buf, '\n');
        break;
    case 't':
        strbuf_append_byte(buf, '\t');
        break;
    case '\\':
        strbuf_append_byte(buf, '\\');
        break;
    case '"':
        strbuf_append_byte(buf, '"');
        break;
    case '/':
        strbuf_append_byte(buf, '/');
        break;
    default:
        strbuf_append_byte(buf, c);
        break;
    }
}

/* Walk to a key's value position in a flat JSON object.
 * Returns pointer to the value start, or NULL if not found. */
static const char *json_find_key(const char *json, const char *key)
{
    const char *p = json_skip_ws(json);
    if (*p != '{') {
        return NULL;
    }
    p++;
    while (*p) {
        p = json_skip_ws(p);
        if (*p == '}') {
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') {
            break;
        }
        p++;
        const char *ks = p;
        while (*p && *p != '"') {
            if (*p == '\\') {
                p++;
            }
            if (*p) {
                p++;
            }
        }
        size_t klen = (size_t)(p - ks);
        if (*p == '"') {
            p++;
        }
        p = json_skip_ws(p);
        if (*p == ':') {
            p++;
        }
        p = json_skip_ws(p);

        if (klen == strlen(key) && memcmp(ks, key, klen) == 0) {
            return p;
        }

        p = json_skip_value(p);
    }
    return NULL;
}

char *json_get_string(const char *json, const char *key)
{
    const char *p = json_find_key(json, key);
    if (p == NULL || *p != '"') {
        return NULL;
    }
    p++;
    strbuf_t val;
    strbuf_init(&val);
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            json_decode_escape(&val, *p);
        } else {
            strbuf_append_byte(&val, *p);
        }
        p++;
    }
    return strbuf_detach(&val);
}

int64_t json_get_int(const char *json, const char *key)
{
    const char *p = json_find_key(json, key);
    if (p == NULL || (*p != '-' && (*p < '0' || *p > '9'))) {
        return -1;
    }
    return strtoll(p, NULL, 10);
}

char *json_find_nested_string(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (pos == NULL) {
        return NULL;
    }
    pos += strlen(search);
    pos = json_skip_ws(pos);
    if (*pos == ':') {
        pos++;
    }
    pos = json_skip_ws(pos);
    if (*pos != '"') {
        return NULL;
    }
    pos++;
    strbuf_t val;
    strbuf_init(&val);
    while (*pos && *pos != '"') {
        if (*pos == '\\' && pos[1]) {
            pos++;
            json_decode_escape(&val, *pos);
        } else {
            strbuf_append_byte(&val, *pos);
        }
        pos++;
    }
    return strbuf_detach(&val);
}
