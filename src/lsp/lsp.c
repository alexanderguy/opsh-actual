#include "lsp/lsp.h"

#include "builtins/builtins.h"
#include "foundation/json.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "lint/lint.h"
#include "parser/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Minimal JSON value extraction. Not a full parser -- just enough
 * to extract string/int values by key from a flat JSON object.
 */

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

/* Extract a string value for a given key. Returns malloc'd string or NULL. */
static char *json_get_string(const char *json, const char *key)
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
        /* Read key */
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

        bool match = (klen == strlen(key) && memcmp(ks, key, klen) == 0);

        if (match && *p == '"') {
            /* Extract string value */
            p++;
            strbuf_t val;
            strbuf_init(&val);
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    p++;
                    switch (*p) {
                    case 'n':
                        strbuf_append_byte(&val, '\n');
                        break;
                    case 't':
                        strbuf_append_byte(&val, '\t');
                        break;
                    case '\\':
                        strbuf_append_byte(&val, '\\');
                        break;
                    case '"':
                        strbuf_append_byte(&val, '"');
                        break;
                    default:
                        strbuf_append_byte(&val, *p);
                        break;
                    }
                } else {
                    strbuf_append_byte(&val, *p);
                }
                p++;
            }
            return strbuf_detach(&val);
        }

        /* Skip this value */
        p = json_skip_value(p);
    }
    return NULL;
}

/* Extract an integer value for a given key. Returns the value or -1. */
static int64_t json_get_int(const char *json, const char *key)
{
    const char *p = json_skip_ws(json);
    if (*p != '{') {
        return -1;
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

        bool match = (klen == strlen(key) && memcmp(ks, key, klen) == 0);

        if (match && (*p == '-' || (*p >= '0' && *p <= '9'))) {
            return strtoll(p, NULL, 10);
        }

        p = json_skip_value(p);
    }
    return -1;
}

/*
 * Find a string value for a key anywhere in the JSON (brute-force scan).
 * Returns malloc'd string or NULL. Handles JSON string escapes.
 */
static char *json_find_nested_string(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (pos == NULL) {
        return NULL;
    }
    /* Skip past the key and colon */
    pos += strlen(search);
    pos = json_skip_ws(pos);
    if (*pos == ':') {
        pos++;
    }
    pos = json_skip_ws(pos);
    if (*pos != '"') {
        return NULL;
    }
    /* Extract the string value */
    pos++;
    strbuf_t val;
    strbuf_init(&val);
    while (*pos && *pos != '"') {
        if (*pos == '\\' && pos[1]) {
            pos++;
            switch (*pos) {
            case 'n':
                strbuf_append_byte(&val, '\n');
                break;
            case 't':
                strbuf_append_byte(&val, '\t');
                break;
            case '\\':
                strbuf_append_byte(&val, '\\');
                break;
            case '"':
                strbuf_append_byte(&val, '"');
                break;
            case '/':
                strbuf_append_byte(&val, '/');
                break;
            default:
                strbuf_append_byte(&val, *pos);
                break;
            }
        } else {
            strbuf_append_byte(&val, *pos);
        }
        pos++;
    }
    return strbuf_detach(&val);
}

/* Read a Content-Length framed message from stdin. Returns malloc'd body or NULL on EOF. */
static char *read_message(void)
{
    /* Read headers until blank line */
    int content_length = -1;
    char line[256];

    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            return NULL;
        }
        if (line[0] == '\r' || line[0] == '\n') {
            break; /* blank line = end of headers */
        }
        if (strncmp(line, "Content-Length:", 15) == 0) {
            content_length = atoi(line + 15);
        }
    }

    if (content_length <= 0) {
        return NULL;
    }

    char *body = xmalloc((size_t)content_length + 1);
    size_t nread = fread(body, 1, (size_t)content_length, stdin);
    body[nread] = '\0';

    if ((int)nread != content_length) {
        free(body);
        return NULL;
    }

    return body;
}

/* Write a Content-Length framed JSON response to stdout */
static void send_response(const char *json)
{
    size_t len = strlen(json);
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", len);
    fwrite(json, 1, len, stdout);
    fflush(stdout);
}

/* Build and send a JSON-RPC response */
static void send_result(int64_t id, const char *result_json)
{
    strbuf_t buf;
    strbuf_init(&buf);
    json_begin_object(&buf);
    json_key_string(&buf, "jsonrpc", "2.0");
    json_key_int(&buf, "id", id);
    strbuf_append_str(&buf, ",\"result\":");
    strbuf_append_str(&buf, result_json);
    json_end_object(&buf);
    send_response(buf.contents);
    strbuf_destroy(&buf);
}

/* Map lint severity to LSP severity (1=Error, 2=Warning, 3=Info, 4=Hint) */
static int lint_severity_to_lsp(lint_severity_t sev)
{
    switch (sev) {
    case LINT_ERROR:
        return 1;
    case LINT_WARNING:
        return 2;
    case LINT_INFO:
        return 3;
    case LINT_STYLE:
        return 4;
    }
    return 3;
}

/* Publish diagnostics for a document */
static void publish_diagnostics(const char *uri, const char *text)
{
    parser_t p;
    parser_init(&p, text, uri);
    sh_list_t *ast = parser_parse(&p);

    strbuf_t buf;
    strbuf_init(&buf);
    json_begin_object(&buf);
    json_key_string(&buf, "jsonrpc", "2.0");
    json_key_string(&buf, "method", "textDocument/publishDiagnostics");
    strbuf_append_str(&buf, ",\"params\":{");
    json_key_string(&buf, "uri", uri);
    strbuf_append_str(&buf, ",\"diagnostics\":[");

    bool has_diag = false;

    /* Parser errors */
    {
        int i;
        int n = p.error_count;
        if (n > MAX_PARSE_ERRORS) {
            n = MAX_PARSE_ERRORS;
        }
        for (i = 0; i < n; i++) {
            if (has_diag) {
                strbuf_append_byte(&buf, ',');
            }
            has_diag = true;
            parse_error_t *e = &p.errors[i];
            unsigned int line = e->lineno > 0 ? e->lineno - 1 : 0;
            strbuf_append_byte(&buf, '{');
            strbuf_append_str(&buf, "\"range\":{\"start\":{");
            strbuf_append_printf(
                &buf, "\"line\":%u,\"character\":0},\"end\":{\"line\":%u,\"character\":999}}", line,
                line);
            json_key_string(&buf, "message", e->message);
            json_key_int(&buf, "severity", 1);
            strbuf_append_byte(&buf, '}');
        }
    }

    /* Lint diagnostics (only if parse succeeded) */
    if (parser_error_count(&p) == 0 && ast != NULL) {
        lint_diag_t *diags = lint_check(ast, uri);
        const lint_diag_t *d = diags;
        while (d != NULL) {
            if (has_diag) {
                strbuf_append_byte(&buf, ',');
            }
            has_diag = true;
            unsigned int line = d->lineno > 0 ? d->lineno - 1 : 0;
            strbuf_append_byte(&buf, '{');
            strbuf_append_str(&buf, "\"range\":{\"start\":{");
            strbuf_append_printf(
                &buf, "\"line\":%u,\"character\":0},\"end\":{\"line\":%u,\"character\":999}}", line,
                line);
            /* Include SC code in message for easy identification */
            {
                char msg_buf[600];
                snprintf(msg_buf, sizeof(msg_buf), "SC%04d: %s", d->code, d->message);
                json_key_string(&buf, "message", msg_buf);
            }
            json_key_int(&buf, "severity", lint_severity_to_lsp(d->severity));
            strbuf_append_byte(&buf, '}');
            d = d->next;
        }
        lint_diag_free(diags);
    }

    sh_list_free(ast);
    strbuf_append_str(&buf, "]}}");
    send_response(buf.contents);
    strbuf_destroy(&buf);
    parser_destroy(&p);
}

/* Build completion items */
static void handle_completion(int64_t id)
{
    strbuf_t items;
    strbuf_init(&items);
    strbuf_append_byte(&items, '[');

    /* Add builtins */
    {
        int i;
        bool first = true;
        for (i = 0; builtin_table[i].name != NULL; i++) {
            if (!first) {
                strbuf_append_byte(&items, ',');
            }
            first = false;
            strbuf_append_byte(&items, '{');
            json_key_string(&items, "label", builtin_table[i].name);
            json_key_int(&items, "kind", 3); /* Function */
            strbuf_append_byte(&items, '}');
        }
    }

    strbuf_append_byte(&items, ']');
    send_result(id, items.contents);
    strbuf_destroy(&items);
}

int lsp_handle_message(const char *msg)
{
    char *method = json_get_string(msg, "method");
    int64_t id = json_get_int(msg, "id");

    if (method == NULL) {
        return 1; /* continue */
    }

    if (strcmp(method, "initialize") == 0) {
        strbuf_t caps;
        strbuf_init(&caps);
        strbuf_append_str(&caps, "{\"capabilities\":{");
        strbuf_append_str(&caps, "\"textDocumentSync\":1");
        strbuf_append_str(&caps, ",\"completionProvider\":{}");
        strbuf_append_str(&caps, "}}");
        send_result(id, caps.contents);
        strbuf_destroy(&caps);
    } else if (strcmp(method, "initialized") == 0) {
        /* No-op notification */
    } else if (strcmp(method, "shutdown") == 0) {
        send_result(id, "null");
    } else if (strcmp(method, "exit") == 0) {
        free(method);
        return 0; /* stop */
    } else if (strcmp(method, "textDocument/didOpen") == 0 ||
               strcmp(method, "textDocument/didChange") == 0) {
        char *uri = json_find_nested_string(msg, "uri");
        char *text = json_find_nested_string(msg, "text");
        if (uri != NULL && text != NULL) {
            publish_diagnostics(uri, text);
        }
        free(uri);
        free(text);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        handle_completion(id);
    }

    free(method);
    return 1; /* continue */
}

int lsp_main(void)
{
    for (;;) {
        char *msg = read_message();
        if (msg == NULL) {
            break;
        }
        int cont = lsp_handle_message(msg);
        free(msg);
        if (!cont) {
            break;
        }
    }
    return 0;
}
