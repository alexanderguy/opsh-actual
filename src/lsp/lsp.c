#include "lsp/lsp.h"

#include "builtins/builtins.h"
#include "foundation/json.h"
#include "foundation/jsonrpc.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "lint/lint.h"
#include "parser/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    jsonrpc_send(stdout, buf.contents);
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
    jsonrpc_send_result(stdout, id, items.contents);
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
        jsonrpc_send_result(stdout, id, caps.contents);
        strbuf_destroy(&caps);
    } else if (strcmp(method, "initialized") == 0) {
        /* No-op notification */
    } else if (strcmp(method, "shutdown") == 0) {
        jsonrpc_send_result(stdout, id, "null");
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
        char *msg = jsonrpc_read_message(stdin);
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
