#include "foundation/jsonrpc.h"

#include "foundation/json.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *jsonrpc_read_message(FILE *in)
{
    int content_length = -1;
    char line[256];

    for (;;) {
        if (fgets(line, sizeof(line), in) == NULL) {
            return NULL;
        }
        if (line[0] == '\r' || line[0] == '\n') {
            break;
        }
        if (strncmp(line, "Content-Length:", 15) == 0) {
            long val = strtol(line + 15, NULL, 10);
            if (val > 0 && val <= 10 * 1024 * 1024) {
                content_length = (int)val;
            }
        }
    }

    if (content_length <= 0) {
        return NULL;
    }

    char *body = xmalloc((size_t)content_length + 1);
    size_t nread = fread(body, 1, (size_t)content_length, in);
    body[nread] = '\0';

    if ((int)nread != content_length) {
        free(body);
        return NULL;
    }

    return body;
}

void jsonrpc_send(FILE *out, const char *json)
{
    size_t len = strlen(json);
    fprintf(out, "Content-Length: %zu\r\n\r\n", len);
    fwrite(json, 1, len, out);
    fflush(out);
}

void jsonrpc_send_result(FILE *out, int64_t id, const char *result_json)
{
    strbuf_t buf;
    strbuf_init(&buf);
    json_begin_object(&buf);
    json_key_string(&buf, "jsonrpc", "2.0");
    json_key_int(&buf, "id", id);
    strbuf_append_str(&buf, ",\"result\":");
    strbuf_append_str(&buf, result_json);
    json_end_object(&buf);
    jsonrpc_send(out, buf.contents);
    strbuf_destroy(&buf);
}

void jsonrpc_send_error(FILE *out, int64_t id, int code, const char *message)
{
    strbuf_t buf;
    strbuf_init(&buf);
    json_begin_object(&buf);
    json_key_string(&buf, "jsonrpc", "2.0");
    json_key_int(&buf, "id", id);
    strbuf_append_str(&buf, ",\"error\":");
    json_begin_object(&buf);
    json_key_int(&buf, "code", code);
    json_key_string(&buf, "message", message);
    json_end_object(&buf);
    json_end_object(&buf);
    jsonrpc_send(out, buf.contents);
    strbuf_destroy(&buf);
}

void jsonrpc_send_notification(FILE *out, const char *method, const char *params_json)
{
    strbuf_t buf;
    strbuf_init(&buf);
    json_begin_object(&buf);
    json_key_string(&buf, "jsonrpc", "2.0");
    json_key_string(&buf, "method", method);
    strbuf_append_str(&buf, ",\"params\":");
    strbuf_append_str(&buf, params_json);
    json_end_object(&buf);
    jsonrpc_send(out, buf.contents);
    strbuf_destroy(&buf);
}
