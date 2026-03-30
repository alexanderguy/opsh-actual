#include "../tap.h"

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MCP_TIMEOUT_MS 3000

typedef struct {
    pid_t pid;
    int to_fd;
    int from_fd;
} mcp_proc_t;

static mcp_proc_t mcp_start(void)
{
    int to_child[2], from_child[2];
    pipe(to_child);
    pipe(from_child);

    pid_t pid = fork();
    if (pid == 0) {
        close(to_child[1]);
        close(from_child[0]);
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        if (to_child[0] > 2)
            close(to_child[0]);
        if (from_child[1] > 2)
            close(from_child[1]);
        execl("build/opsh", "opsh", "mcp", NULL);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);

    mcp_proc_t m;
    m.pid = pid;
    m.to_fd = to_child[1];
    m.from_fd = from_child[0];
    return m;
}

static void mcp_stop(mcp_proc_t *m)
{
    close(m->to_fd);
    close(m->from_fd);
    int status;
    waitpid(m->pid, &status, 0);
}

/* MCP stdio transport: newline-delimited JSON */
static void mcp_send(mcp_proc_t *m, const char *json)
{
    write(m->to_fd, json, strlen(json));
    write(m->to_fd, "\n", 1);
}

static char *mcp_recv(mcp_proc_t *m)
{
    struct pollfd pfd = {.fd = m->from_fd, .events = POLLIN};
    if (poll(&pfd, 1, MCP_TIMEOUT_MS) <= 0) {
        return NULL;
    }

    char buf[16384];
    size_t pos = 0;

    while (pos < sizeof(buf) - 1) {
        ssize_t n = read(m->from_fd, buf + pos, 1);
        if (n <= 0)
            return NULL;
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            break;
        }
        pos++;
    }

    if (pos == 0)
        return NULL;

    char *body = malloc(pos + 1);
    memcpy(body, buf, pos + 1);
    return body;
}

/* Check no response arrives within a short window */
static int mcp_no_response(mcp_proc_t *m)
{
    struct pollfd pfd = {.fd = m->from_fd, .events = POLLIN};
    return poll(&pfd, 1, 200) == 0;
}

/* Send initialize + notifications/initialized handshake */
static void mcp_handshake(mcp_proc_t *m)
{
    mcp_send(m, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                "\"params\":{}}");
    char *resp = mcp_recv(m);
    free(resp);
    mcp_send(m, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
}

/* ---- Tests ---- */

static void test_initialize(void)
{
    mcp_proc_t m = mcp_start();

    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                 "\"params\":{}}");
    char *resp = mcp_recv(&m);
    tap_ok(resp != NULL, "initialize: got response");
    tap_ok(resp != NULL && strstr(resp, "\"protocolVersion\"") != NULL,
           "initialize: has protocolVersion");
    tap_ok(resp != NULL && strstr(resp, "2025-11-25") != NULL, "initialize: correct version");
    tap_ok(resp != NULL && strstr(resp, "\"capabilities\"") != NULL,
           "initialize: has capabilities");
    tap_ok(resp != NULL && strstr(resp, "\"tools\"") != NULL, "initialize: has tools capability");
    tap_ok(resp != NULL && strstr(resp, "claude/channel") != NULL,
           "initialize: has channel capability");
    tap_ok(resp != NULL && strstr(resp, "\"serverInfo\"") != NULL, "initialize: has serverInfo");
    tap_ok(resp != NULL && strstr(resp, "\"opsh\"") != NULL, "initialize: server name is opsh");
    free(resp);

    mcp_stop(&m);
}

static void test_ping(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}");
    char *resp = mcp_recv(&m);
    tap_ok(resp != NULL, "ping: got response");
    tap_ok(resp != NULL && strstr(resp, "\"result\":{}") != NULL, "ping: result is empty object");
    free(resp);

    mcp_stop(&m);
}

static void test_tools_list(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    char *resp = mcp_recv(&m);
    tap_ok(resp != NULL, "tools/list: got response");
    tap_ok(resp != NULL && strstr(resp, "opsh_session_create") != NULL,
           "tools/list: has opsh_session_create");
    tap_ok(resp != NULL && strstr(resp, "opsh_session_eval") != NULL,
           "tools/list: has opsh_session_eval");
    tap_ok(resp != NULL && strstr(resp, "opsh_session_signal") != NULL,
           "tools/list: has opsh_session_signal");
    tap_ok(resp != NULL && strstr(resp, "opsh_session_destroy") != NULL,
           "tools/list: has opsh_session_destroy");
    tap_ok(resp != NULL && strstr(resp, "opsh_session_list") != NULL,
           "tools/list: has opsh_session_list");
    tap_ok(resp != NULL && strstr(resp, "\"inputSchema\"") != NULL, "tools/list: has inputSchema");
    tap_ok(resp != NULL && strstr(resp, "\"required\"") != NULL, "tools/list: has required fields");
    free(resp);

    mcp_stop(&m);
}

static void test_session_create_eval(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    /* Create session */
    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_create\",\"arguments\":{}}}");
    char *resp = mcp_recv(&m);
    tap_ok(resp != NULL, "create: got response");
    tap_ok(resp != NULL && strstr(resp, "session_id") != NULL, "create: has session_id in content");
    tap_ok(resp != NULL && strstr(resp, "\"type\":\"text\"") != NULL,
           "create: has text content type");
    free(resp);

    /* Eval echo */
    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_eval\","
                 "\"arguments\":{\"session_id\":1,\"source\":\"echo hello\"}}}");
    resp = mcp_recv(&m);
    tap_ok(resp != NULL, "eval: got response");
    tap_ok(resp != NULL && strstr(resp, "exit_status") != NULL, "eval: has exit_status");
    tap_ok(resp != NULL && strstr(resp, "hello") != NULL, "eval: output contains hello");
    free(resp);

    mcp_stop(&m);
}

static void test_eval_nonexistent_session(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_eval\","
                 "\"arguments\":{\"session_id\":999,\"source\":\"echo hi\"}}}");
    char *resp = mcp_recv(&m);
    tap_ok(resp != NULL, "eval nonexistent: got response");
    tap_ok(resp != NULL && strstr(resp, "\"isError\":true") != NULL,
           "eval nonexistent: isError is true");
    tap_ok(resp != NULL && strstr(resp, "Session not found") != NULL,
           "eval nonexistent: error message");
    free(resp);

    mcp_stop(&m);
}

static void test_unknown_tool(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"bogus_tool\",\"arguments\":{}}}");
    char *resp = mcp_recv(&m);
    tap_ok(resp != NULL, "unknown tool: got response");
    tap_ok(resp != NULL && strstr(resp, "\"isError\":true") != NULL,
           "unknown tool: isError is true");
    tap_ok(resp != NULL && strstr(resp, "Unknown tool") != NULL, "unknown tool: error message");
    free(resp);

    mcp_stop(&m);
}

static void test_notifications_no_response(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    /* Send a notification (no id) — should get no response */
    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
                 "\"params\":{}}");
    tap_ok(mcp_no_response(&m), "notification: no response sent");

    /* Server should still be alive — send a ping to verify */
    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"ping\"}");
    char *resp = mcp_recv(&m);
    tap_ok(resp != NULL && strstr(resp, "\"result\":{}") != NULL,
           "notification: server still alive after notification");
    free(resp);

    mcp_stop(&m);
}

static void test_stdin_eof(void)
{
    mcp_proc_t m = mcp_start();

    close(m.to_fd);
    m.to_fd = -1;

    int status;
    waitpid(m.pid, &status, 0);
    tap_ok(WIFEXITED(status), "eof: server exited");
    tap_ok(WEXITSTATUS(status) == 0, "eof: clean exit");

    close(m.from_fd);
}

static void test_signal_name_mapping(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    /* Create a session */
    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_create\",\"arguments\":{}}}");
    char *resp = mcp_recv(&m);
    free(resp);

    /* Send SIGUSR1 to the session */
    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_signal\","
                 "\"arguments\":{\"session_id\":1,\"signal\":\"SIGUSR1\"}}}");
    resp = mcp_recv(&m);
    tap_ok(resp != NULL, "signal: got response");
    tap_ok(resp != NULL && strstr(resp, "Signal sent") != NULL, "signal: success message");
    tap_ok(resp != NULL && strstr(resp, "\"isError\"") == NULL, "signal: no error flag");
    free(resp);

    /* Try an invalid signal name */
    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_signal\","
                 "\"arguments\":{\"session_id\":1,\"signal\":\"SIGBOGUS\"}}}");
    resp = mcp_recv(&m);
    tap_ok(resp != NULL && strstr(resp, "\"isError\":true") != NULL,
           "signal: unknown signal is error");
    tap_ok(resp != NULL && strstr(resp, "Unknown signal") != NULL,
           "signal: unknown signal message");
    free(resp);

    mcp_stop(&m);
}

static void test_unknown_method(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"bogus/method\"}");
    char *resp = mcp_recv(&m);
    tap_ok(resp != NULL, "unknown method: got response");
    tap_ok(resp != NULL && strstr(resp, "\"error\"") != NULL, "unknown method: has error");
    tap_ok(resp != NULL && strstr(resp, "-32601") != NULL, "unknown method: method not found code");
    free(resp);

    mcp_stop(&m);
}

/* ---- Streaming helpers ---- */

#define MAX_RECV_MSGS 64

typedef struct {
    char *msgs[MAX_RECV_MSGS];
    int count;
    int result_idx;
} mcp_recv_all_t;

static mcp_recv_all_t mcp_recv_all(mcp_proc_t *m)
{
    mcp_recv_all_t ra;
    memset(&ra, 0, sizeof(ra));
    ra.result_idx = -1;

    for (int i = 0; i < MAX_RECV_MSGS; i++) {
        char *msg = mcp_recv(m);
        if (msg == NULL) {
            break;
        }
        ra.msgs[ra.count] = msg;
        ra.count++;
        if (strstr(msg, "\"method\"") == NULL) {
            ra.result_idx = ra.count - 1;
            break;
        }
    }
    return ra;
}

static void mcp_recv_all_free(mcp_recv_all_t *ra)
{
    for (int i = 0; i < ra->count; i++) {
        free(ra->msgs[i]);
        ra->msgs[i] = NULL;
    }
    ra->count = 0;
    ra->result_idx = -1;
}

/* ---- Streaming tests ---- */

static void test_eval_stream_basic(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    /* Create session */
    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_create\",\"arguments\":{}}}");
    char *resp = mcp_recv(&m);
    free(resp);

    /* Eval with streaming */
    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_eval\","
                 "\"arguments\":{\"session_id\":1,\"source\":\"echo hello\","
                 "\"stream\":true}}}");
    mcp_recv_all_t ra = mcp_recv_all(&m);

    tap_ok(ra.count >= 2, "mcp stream: got notifications + result");
    tap_ok(ra.result_idx >= 0, "mcp stream: got final result");

    int found_channel = 0;
    int has_content = 0;
    int has_meta_stream = 0;
    int has_meta_sid = 0;
    for (int i = 0; i < ra.count; i++) {
        if (i == ra.result_idx)
            continue;
        if (strstr(ra.msgs[i], "notifications/claude/channel") != NULL) {
            found_channel = 1;
            if (strstr(ra.msgs[i], "\"content\":") != NULL && strstr(ra.msgs[i], "hello") != NULL)
                has_content = 1;
            if (strstr(ra.msgs[i], "\"meta\"") != NULL &&
                strstr(ra.msgs[i], "\"stream\":\"stdout\"") != NULL)
                has_meta_stream = 1;
            if (strstr(ra.msgs[i], "\"session_id\":\"1\"") != NULL)
                has_meta_sid = 1;
        }
    }
    tap_ok(found_channel, "mcp stream: uses notifications/claude/channel");
    tap_ok(has_content, "mcp stream: content field has output");
    tap_ok(has_meta_stream, "mcp stream: meta has stream type");
    tap_ok(has_meta_sid, "mcp stream: meta has session_id");

    if (ra.result_idx >= 0) {
        tap_ok(strstr(ra.msgs[ra.result_idx], "stdout") == NULL, "mcp stream: result omits stdout");
        tap_ok(strstr(ra.msgs[ra.result_idx], "exit_status") != NULL,
               "mcp stream: has exit status");
    } else {
        tap_ok(0, "mcp stream: result omits stdout");
        tap_ok(0, "mcp stream: has exit status");
    }

    mcp_recv_all_free(&ra);
    mcp_stop(&m);
}

static void test_eval_stream_disabled(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_create\",\"arguments\":{}}}");
    char *resp = mcp_recv(&m);
    free(resp);

    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_eval\","
                 "\"arguments\":{\"session_id\":1,\"source\":\"echo nostream\"}}}");
    resp = mcp_recv(&m);
    tap_ok(resp != NULL && strstr(resp, "nostream") != NULL, "mcp no-stream: has output in result");
    tap_ok(resp != NULL && strstr(resp, "stdout") != NULL, "mcp no-stream: result has stdout key");
    free(resp);

    mcp_stop(&m);
}

static void test_eval_stream_stderr(void)
{
    mcp_proc_t m = mcp_start();
    mcp_handshake(&m);

    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_create\",\"arguments\":{}}}");
    char *resp = mcp_recv(&m);
    free(resp);

    mcp_send(&m, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"opsh_session_eval\","
                 "\"arguments\":{\"session_id\":1,\"source\":\"echo err >&2\","
                 "\"stream\":true}}}");
    mcp_recv_all_t ra = mcp_recv_all(&m);

    int found_stderr = 0;
    for (int i = 0; i < ra.count; i++) {
        if (i == ra.result_idx)
            continue;
        if (strstr(ra.msgs[i], "notifications/claude/channel") != NULL &&
            strstr(ra.msgs[i], "\"content\":") != NULL && strstr(ra.msgs[i], "err\\n") != NULL &&
            strstr(ra.msgs[i], "\"stream\":\"stderr\"") != NULL) {
            found_stderr = 1;
        }
    }
    tap_ok(found_stderr, "mcp stream stderr: notification has stderr data");

    mcp_recv_all_free(&ra);
    mcp_stop(&m);
}

int main(void)
{
    tap_plan(53);

    test_initialize();
    test_ping();
    test_tools_list();
    test_session_create_eval();
    test_eval_nonexistent_session();
    test_unknown_tool();
    test_notifications_no_response();
    test_stdin_eof();
    test_signal_name_mapping();
    test_unknown_method();
    test_eval_stream_basic();
    test_eval_stream_disabled();
    test_eval_stream_stderr();

    tap_done();
    return 0;
}
