#include "serve/mcp.h"

#include "foundation/json.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "serve/session.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSONRPC_METHOD_NOT_FOUND (-32601)
#define JSONRPC_INVALID_PARAMS (-32602)

/*
 * MCP stdio transport: newline-delimited JSON.
 * Each message is a single line of JSON followed by '\n'.
 * No Content-Length headers (that's LSP, not MCP).
 */

static char *mcp_read_message(FILE *in)
{
    strbuf_t line;
    strbuf_init(&line);

    for (;;) {
        int ch = fgetc(in);
        if (ch == EOF) {
            strbuf_destroy(&line);
            return NULL;
        }
        if (ch == '\n') {
            break;
        }
        strbuf_append_byte(&line, (char)ch);
    }

    if (line.length == 0) {
        strbuf_destroy(&line);
        return NULL;
    }

    return strbuf_detach(&line);
}

static void mcp_send_json(FILE *out, const char *json)
{
    fputs(json, out);
    fputc('\n', out);
    fflush(out);
}

static void mcp_send_result(FILE *out, int64_t id, const char *result_json)
{
    strbuf_t buf;
    strbuf_init(&buf);
    json_begin_object(&buf);
    json_key_string(&buf, "jsonrpc", "2.0");
    json_key_int(&buf, "id", id);
    strbuf_append_str(&buf, ",\"result\":");
    strbuf_append_str(&buf, result_json);
    json_end_object(&buf);
    mcp_send_json(out, buf.contents);
    strbuf_destroy(&buf);
}

static void mcp_send_error(FILE *out, int64_t id, int code, const char *message)
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
    mcp_send_json(out, buf.contents);
    strbuf_destroy(&buf);
}

#define MCP_PROTOCOL_VERSION "2025-11-25"

/* Search for an integer value by key anywhere in the JSON string. */
static int64_t get_nested_int(const char *msg, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(msg, search);
    if (p == NULL) {
        return -1;
    }
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':') {
        p++;
    }
    if ((*p >= '0' && *p <= '9') || *p == '-') {
        return strtoll(p, NULL, 10);
    }
    return -1;
}

static const struct {
    const char *name;
    int signum;
} signal_table[] = {
    {"SIGHUP", SIGHUP},   {"SIGINT", SIGINT},   {"SIGQUIT", SIGQUIT}, {"SIGTERM", SIGTERM},
    {"SIGKILL", SIGKILL}, {"SIGUSR1", SIGUSR1}, {"SIGUSR2", SIGUSR2},
};

#define SIGNAL_TABLE_SIZE (sizeof(signal_table) / sizeof(signal_table[0]))

static int signal_name_to_num(const char *name)
{
    size_t i;
    for (i = 0; i < SIGNAL_TABLE_SIZE; i++) {
        if (strcmp(signal_table[i].name, name) == 0) {
            return signal_table[i].signum;
        }
    }
    return -1;
}

static void mcp_send_tool_result(int64_t id, const char *text, int is_error)
{
    strbuf_t result;
    strbuf_init(&result);
    json_begin_object(&result);
    strbuf_append_str(&result, "\"content\":[");
    json_begin_object(&result);
    json_key_string(&result, "type", "text");
    json_key_string(&result, "text", text);
    json_end_object(&result);
    json_end_array(&result);
    if (is_error) {
        json_key_bool(&result, "isError", 1);
    }
    json_end_object(&result);
    mcp_send_result(stdout, id, result.contents);
    strbuf_destroy(&result);
}

static int sessions_initialized;

static int ensure_sessions(void)
{
    if (sessions_initialized) {
        return 0;
    }
    if (session_init() != 0) {
        return -1;
    }
    sessions_initialized = 1;
    return 0;
}

static int msg_has_id(const char *msg)
{
    return json_has_key(msg, "id");
}

/* --- tools/list schema (hardcoded) --- */

/* clang-format off */
static const char tools_list_json[] =
    "{\"tools\":["

    "{\"name\":\"session_create\","
    "\"description\":\"Create a new shell session\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"cwd\":{\"type\":\"string\",\"description\":\"Working directory for the session\"}"
    "}}}"

    ",{\"name\":\"session_eval\","
    "\"description\":\"Evaluate a shell command in a session\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"session_id\":{\"type\":\"integer\",\"description\":\"Session ID\"},"
    "\"source\":{\"type\":\"string\",\"description\":\"Shell command to execute\"},"
    "\"timeout_ms\":{\"type\":\"integer\",\"description\":\"Timeout in milliseconds\",\"default\":30000}"
    "},\"required\":[\"session_id\",\"source\"]}}"

    ",{\"name\":\"session_signal\","
    "\"description\":\"Send a signal to a session process\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"session_id\":{\"type\":\"integer\",\"description\":\"Session ID\"},"
    "\"signal\":{\"type\":\"string\",\"description\":\"Signal name\","
    "\"enum\":[\"SIGHUP\",\"SIGINT\",\"SIGQUIT\",\"SIGTERM\",\"SIGKILL\",\"SIGUSR1\",\"SIGUSR2\"]}"
    "},\"required\":[\"session_id\",\"signal\"]}}"

    ",{\"name\":\"session_destroy\","
    "\"description\":\"Destroy a shell session\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"session_id\":{\"type\":\"integer\",\"description\":\"Session ID\"}"
    "},\"required\":[\"session_id\"]}}"

    ",{\"name\":\"session_list\","
    "\"description\":\"List active shell sessions\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"

    "]}";
/* clang-format on */

/* --- Method handlers --- */

static void handle_initialize(int64_t id)
{
    strbuf_t result;
    strbuf_init(&result);
    json_begin_object(&result);
    json_key_string(&result, "protocolVersion", MCP_PROTOCOL_VERSION);
    strbuf_append_str(&result, ",\"capabilities\":");
    json_begin_object(&result);
    strbuf_append_str(&result, "\"tools\":");
    json_begin_object(&result);
    json_key_bool(&result, "listChanged", 0);
    json_end_object(&result);
    json_end_object(&result);
    strbuf_append_str(&result, ",\"serverInfo\":");
    json_begin_object(&result);
    json_key_string(&result, "name", "opsh");
    json_key_string(&result, "version", "0.1.0");
    json_end_object(&result);
    json_end_object(&result);
    mcp_send_result(stdout, id, result.contents);
    strbuf_destroy(&result);
}

static void handle_tools_list(int64_t id)
{
    mcp_send_result(stdout, id, tools_list_json);
}

static void handle_tool_session_create(int64_t id, const char *msg)
{
    if (ensure_sessions() != 0) {
        mcp_send_tool_result(id, "Failed to initialize session subsystem", 1);
        return;
    }
    char *cwd = json_find_nested_string(msg, "cwd");
    session_create_result_t result;
    char *error = NULL;

    if (session_op_create(cwd, &result, &error) != 0) {
        free(cwd);
        mcp_send_tool_result(id, error, 1);
        free(error);
        return;
    }
    free(cwd);

    strbuf_t text;
    strbuf_init(&text);
    json_begin_object(&text);
    json_key_int(&text, "session_id", result.session_id);
    json_end_object(&text);
    mcp_send_tool_result(id, text.contents, 0);
    strbuf_destroy(&text);
}

static void handle_tool_session_eval(int64_t id, const char *msg)
{
    if (ensure_sessions() != 0) {
        mcp_send_tool_result(id, "Failed to initialize session subsystem", 1);
        return;
    }
    char *source = json_find_nested_string(msg, "source");
    if (source == NULL) {
        mcp_send_tool_result(id, "Missing required parameter: source", 1);
        return;
    }

    int64_t sid = get_nested_int(msg, "session_id");
    int64_t timeout_raw = get_nested_int(msg, "timeout_ms");

    session_eval_result_t result;
    char *error = NULL;

    int rc = session_op_eval((int)sid, source, (int)timeout_raw, NULL, NULL, &result, &error);
    free(source);
    if (rc != 0) {
        mcp_send_tool_result(id, error, 1);
        free(error);
        return;
    }

    strbuf_t text;
    strbuf_init(&text);
    json_begin_object(&text);
    json_key_int(&text, "exit_status", result.exit_status);
    json_key_string(&text, "stdout", result.out_buf);
    json_key_string(&text, "stderr", result.err_buf);
    if (result.truncated) {
        json_key_bool(&text, "truncated", 1);
    }
    json_end_object(&text);
    mcp_send_tool_result(id, text.contents, 0);
    strbuf_destroy(&text);
    session_eval_result_destroy(&result);
}

static void handle_tool_session_signal(int64_t id, const char *msg)
{
    if (ensure_sessions() != 0) {
        mcp_send_tool_result(id, "Failed to initialize session subsystem", 1);
        return;
    }
    int64_t sid = get_nested_int(msg, "session_id");

    char *signame = json_find_nested_string(msg, "signal");
    if (signame == NULL) {
        mcp_send_tool_result(id, "Missing required parameter: signal", 1);
        return;
    }

    int signum = signal_name_to_num(signame);
    if (signum < 0) {
        strbuf_t err;
        strbuf_init(&err);
        strbuf_append_str(&err, "Unknown signal: ");
        strbuf_append_str(&err, signame);
        mcp_send_tool_result(id, err.contents, 1);
        strbuf_destroy(&err);
        free(signame);
        return;
    }
    free(signame);

    char *error = NULL;
    if (session_op_signal((int)sid, signum, &error) != 0) {
        mcp_send_tool_result(id, error, 1);
        free(error);
        return;
    }

    mcp_send_tool_result(id, "Signal sent", 0);
}

static void handle_tool_session_destroy(int64_t id, const char *msg)
{
    if (ensure_sessions() != 0) {
        mcp_send_tool_result(id, "Failed to initialize session subsystem", 1);
        return;
    }
    int64_t sid = get_nested_int(msg, "session_id");

    char *error = NULL;
    if (session_op_destroy((int)sid, &error) != 0) {
        mcp_send_tool_result(id, error, 1);
        free(error);
        return;
    }

    mcp_send_tool_result(id, "Session destroyed", 0);
}

static void handle_tool_session_list(int64_t id)
{
    if (ensure_sessions() != 0) {
        mcp_send_tool_result(id, "Failed to initialize session subsystem", 1);
        return;
    }
    session_list_result_t list;
    char *error = NULL;

    if (session_op_list(&list, &error) != 0) {
        mcp_send_tool_result(id, error, 1);
        free(error);
        return;
    }

    strbuf_t text;
    strbuf_init(&text);
    strbuf_append_byte(&text, '[');
    int i;
    for (i = 0; i < list.count; i++) {
        if (i > 0) {
            strbuf_append_byte(&text, ',');
        }
        json_begin_object(&text);
        json_key_int(&text, "session_id", list.session_ids[i]);
        json_end_object(&text);
    }
    strbuf_append_byte(&text, ']');
    mcp_send_tool_result(id, text.contents, 0);
    strbuf_destroy(&text);
    free(list.session_ids);
}

static void handle_tools_call(int64_t id, const char *msg)
{
    char *name = json_find_nested_string(msg, "name");
    if (name == NULL) {
        mcp_send_error(stdout, id, JSONRPC_INVALID_PARAMS, "Missing tool name");
        return;
    }

    if (strcmp(name, "session_create") == 0) {
        free(name);
        handle_tool_session_create(id, msg);
    } else if (strcmp(name, "session_eval") == 0) {
        free(name);
        handle_tool_session_eval(id, msg);
    } else if (strcmp(name, "session_signal") == 0) {
        free(name);
        handle_tool_session_signal(id, msg);
    } else if (strcmp(name, "session_destroy") == 0) {
        free(name);
        handle_tool_session_destroy(id, msg);
    } else if (strcmp(name, "session_list") == 0) {
        free(name);
        handle_tool_session_list(id);
    } else {
        strbuf_t err;
        strbuf_init(&err);
        strbuf_append_str(&err, "Unknown tool: ");
        strbuf_append_str(&err, name);
        mcp_send_tool_result(id, err.contents, 1);
        strbuf_destroy(&err);
        free(name);
    }
}

/* Handle a single MCP message. Returns 1 to continue, 0 to stop. */
int mcp_handle_message(const char *msg)
{
    char *method = json_get_string(msg, "method");
    int has_id = msg_has_id(msg);
    int64_t id = has_id ? json_get_int(msg, "id") : -1;

    if (method == NULL) {
        if (has_id) {
            mcp_send_error(stdout, id, JSONRPC_METHOD_NOT_FOUND, "Missing method field");
        }
        return 1;
    }

    if (strcmp(method, "initialize") == 0) {
        if (has_id) {
            handle_initialize(id);
        }
    } else if (strcmp(method, "ping") == 0) {
        if (has_id) {
            mcp_send_result(stdout, id, "{}");
        }
    } else if (strcmp(method, "notifications/initialized") == 0 ||
               strcmp(method, "notifications/cancelled") == 0) {
        /* Notifications — no response */
    } else if (strcmp(method, "tools/list") == 0) {
        if (has_id) {
            handle_tools_list(id);
        }
    } else if (strcmp(method, "tools/call") == 0) {
        if (has_id) {
            handle_tools_call(id, msg);
        }
    } else {
        if (has_id) {
            mcp_send_error(stdout, id, JSONRPC_METHOD_NOT_FOUND, method);
        }
    }

    free(method);
    return 1;
}

int mcp_main(void)
{
    /* Disable buffering on stdin/stdout so messages reach the MCP client immediately */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);

    sessions_initialized = 0;

    for (;;) {
        char *msg = mcp_read_message(stdin);
        if (msg == NULL) {
            break;
        }
        int cont = mcp_handle_message(msg);
        free(msg);
        if (!cont) {
            break;
        }
    }

    if (sessions_initialized) {
        session_cleanup();
    }
    return 0;
}
