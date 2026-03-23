#include "serve/serve.h"

#include "foundation/json.h"
#include "foundation/jsonrpc.h"
#include "foundation/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* JSON-RPC 2.0 error codes */
#define JSONRPC_PARSE_ERROR     (-32700)
#define JSONRPC_INVALID_REQUEST (-32600)
#define JSONRPC_METHOD_NOT_FOUND (-32601)

static void handle_initialize(int64_t id)
{
    strbuf_t result;
    strbuf_init(&result);
    json_begin_object(&result);
    json_key_string(&result, "version", "0.1.0");
    strbuf_append_str(&result, ",\"methods\":[");
    json_write_string(&result, "initialize");
    strbuf_append_byte(&result, ',');
    json_write_string(&result, "shutdown");
    strbuf_append_byte(&result, ',');
    json_write_string(&result, "session/create");
    strbuf_append_byte(&result, ',');
    json_write_string(&result, "session/eval");
    strbuf_append_byte(&result, ',');
    json_write_string(&result, "session/signal");
    strbuf_append_byte(&result, ',');
    json_write_string(&result, "session/destroy");
    strbuf_append_byte(&result, ',');
    json_write_string(&result, "session/list");
    json_end_array(&result);
    json_end_object(&result);
    jsonrpc_send_result(stdout, id, result.contents);
    strbuf_destroy(&result);
}

int serve_handle_message(const char *msg)
{
    char *method = json_get_string(msg, "method");
    int64_t id = json_get_int(msg, "id");

    if (method == NULL) {
        if (id >= 0) {
            jsonrpc_send_error(stdout, id, JSONRPC_INVALID_REQUEST,
                               "Missing method field");
        }
        return 1;
    }

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(id);
    } else if (strcmp(method, "shutdown") == 0) {
        jsonrpc_send_result(stdout, id, "null");
        free(method);
        return 0;
    } else {
        if (id >= 0) {
            jsonrpc_send_error(stdout, id, JSONRPC_METHOD_NOT_FOUND,
                               method);
        }
    }

    free(method);
    return 1;
}

int serve_main(void)
{
    for (;;) {
        char *msg = jsonrpc_read_message(stdin);
        if (msg == NULL) {
            break;
        }
        int cont = serve_handle_message(msg);
        free(msg);
        if (!cont) {
            break;
        }
    }
    return 0;
}
