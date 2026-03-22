#ifndef OPSH_FOUNDATION_JSON_H
#define OPSH_FOUNDATION_JSON_H

#include "foundation/strbuf.h"

#include <stdint.h>

/* Write a JSON-escaped string (with surrounding quotes) */
void json_write_string(strbuf_t *buf, const char *s);

/* Object construction helpers */
void json_begin_object(strbuf_t *buf);
void json_end_object(strbuf_t *buf);
void json_key_string(strbuf_t *buf, const char *key, const char *value);
void json_key_int(strbuf_t *buf, const char *key, int64_t value);
void json_key_bool(strbuf_t *buf, const char *key, int value);
void json_key_null(strbuf_t *buf, const char *key);

/* Array construction helpers */
void json_begin_array(strbuf_t *buf);
void json_end_array(strbuf_t *buf);

#endif /* OPSH_FOUNDATION_JSON_H */
