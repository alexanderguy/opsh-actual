#include "../tap.h"

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Integration tests for opsh serve (JSON-RPC 2.0 over Content-Length framing).
 */

#define SERVE_TIMEOUT_MS 3000

typedef struct {
    pid_t pid;
    int to_fd;   /* write to child stdin */
    int from_fd; /* read from child stdout */
} serve_proc_t;

static serve_proc_t serve_start(void)
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
        execl("build/opsh", "opsh", "serve", NULL);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);

    serve_proc_t s;
    s.pid = pid;
    s.to_fd = to_child[1];
    s.from_fd = from_child[0];
    return s;
}

static void serve_stop(serve_proc_t *s)
{
    close(s->to_fd);
    close(s->from_fd);
    int status;
    waitpid(s->pid, &status, 0);
}

/* Send a Content-Length framed JSON-RPC message */
static void serve_send(serve_proc_t *s, const char *json)
{
    char header[64];
    int hlen = snprintf(header, sizeof(header),
                        "Content-Length: %zu\r\n\r\n", strlen(json));
    write(s->to_fd, header, (size_t)hlen);
    write(s->to_fd, json, strlen(json));
}

/* Read a Content-Length framed response. Returns malloc'd body or NULL. */
static char *serve_recv(serve_proc_t *s)
{
    struct pollfd pfd = {.fd = s->from_fd, .events = POLLIN};
    if (poll(&pfd, 1, SERVE_TIMEOUT_MS) <= 0) {
        return NULL;
    }

    /* Read headers byte-by-byte until \r\n\r\n */
    char buf[4096];
    size_t pos = 0;
    int content_length = -1;

    while (pos < sizeof(buf) - 1) {
        ssize_t n = read(s->from_fd, buf + pos, 1);
        if (n <= 0)
            return NULL;
        pos++;
        if (pos >= 4 && memcmp(buf + pos - 4, "\r\n\r\n", 4) == 0) {
            buf[pos] = '\0';
            break;
        }
    }

    char *cl = strstr(buf, "Content-Length:");
    if (cl == NULL)
        return NULL;
    content_length = atoi(cl + 15);
    if (content_length <= 0)
        return NULL;

    char *body = malloc((size_t)content_length + 1);
    size_t total = 0;
    while ((int)total < content_length) {
        ssize_t n = read(s->from_fd, body + total,
                         (size_t)content_length - total);
        if (n <= 0) {
            free(body);
            return NULL;
        }
        total += (size_t)n;
    }
    body[total] = '\0';
    return body;
}

/* Helper: send shutdown and clean up */
static void serve_shutdown(serve_proc_t *s)
{
    serve_send(s, "{\"jsonrpc\":\"2.0\",\"id\":999,\"method\":\"shutdown\"}");
    char *resp = serve_recv(s);
    free(resp);
    serve_stop(s);
}

/* ---- Tests ---- */

static void test_initialize(void)
{
    serve_proc_t s = serve_start();

    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    char *resp = serve_recv(&s);
    tap_ok(resp != NULL, "initialize: got response");
    tap_ok(resp != NULL && strstr(resp, "\"version\"") != NULL,
           "initialize: has version");
    tap_ok(resp != NULL && strstr(resp, "\"methods\"") != NULL,
           "initialize: has methods list");
    tap_ok(resp != NULL && strstr(resp, "session/create") != NULL,
           "initialize: lists session/create");
    free(resp);

    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\"}");
    resp = serve_recv(&s);
    tap_ok(resp != NULL, "shutdown: got response");
    tap_ok(resp != NULL && strstr(resp, "\"result\":null") != NULL,
           "shutdown: result is null");
    free(resp);

    serve_stop(&s);
}

static void test_unknown_method(void)
{
    serve_proc_t s = serve_start();

    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bogus\"}");
    char *resp = serve_recv(&s);
    tap_ok(resp != NULL, "unknown: got response");
    tap_ok(resp != NULL && strstr(resp, "\"error\"") != NULL,
           "unknown: has error");
    tap_ok(resp != NULL && strstr(resp, "-32601") != NULL,
           "unknown: method not found code");
    free(resp);

    serve_shutdown(&s);
}

static void test_missing_method(void)
{
    serve_proc_t s = serve_start();

    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":1}");
    char *resp = serve_recv(&s);
    tap_ok(resp != NULL, "no method: got response");
    tap_ok(resp != NULL && strstr(resp, "\"error\"") != NULL,
           "no method: has error");
    tap_ok(resp != NULL && strstr(resp, "-32600") != NULL,
           "no method: invalid request code");
    free(resp);

    serve_shutdown(&s);
}

static void test_stdin_eof(void)
{
    serve_proc_t s = serve_start();

    close(s.to_fd);
    s.to_fd = -1;

    int status;
    waitpid(s.pid, &status, 0);
    tap_ok(WIFEXITED(status), "eof: server exited");
    tap_ok(WEXITSTATUS(status) == 0, "eof: clean exit");

    close(s.from_fd);
}

static void test_session_create_destroy(void)
{
    serve_proc_t s = serve_start();

    /* Create a session */
    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"session/create\"}");
    char *resp = serve_recv(&s);
    tap_ok(resp != NULL, "create: got response");
    tap_ok(resp != NULL && strstr(resp, "\"session_id\"") != NULL,
           "create: has session_id");
    free(resp);

    /* List sessions — should have 1 */
    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"session/list\"}");
    resp = serve_recv(&s);
    tap_ok(resp != NULL, "list: got response");
    tap_ok(resp != NULL && strstr(resp, "\"session_id\":1") != NULL,
           "list: session 1 present");
    free(resp);

    /* Destroy the session */
    serve_send(&s,
               "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"session/destroy\","
               "\"params\":{\"session_id\":1}}");
    resp = serve_recv(&s);
    tap_ok(resp != NULL, "destroy: got response");
    tap_ok(resp != NULL && strstr(resp, "\"result\":null") != NULL,
           "destroy: result is null");
    free(resp);

    /* List sessions — should be empty */
    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"session/list\"}");
    resp = serve_recv(&s);
    tap_ok(resp != NULL && strstr(resp, "\"sessions\":[]") != NULL,
           "list after destroy: empty");
    free(resp);

    serve_shutdown(&s);
}

static void test_multiple_sessions(void)
{
    serve_proc_t s = serve_start();

    /* Create two sessions */
    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"session/create\"}");
    char *resp = serve_recv(&s);
    tap_ok(resp != NULL && strstr(resp, "\"session_id\"") != NULL,
           "multi: first session created");
    free(resp);

    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"session/create\"}");
    resp = serve_recv(&s);
    tap_ok(resp != NULL && strstr(resp, "\"session_id\"") != NULL,
           "multi: second session created");
    free(resp);

    /* List should have 2 */
    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"session/list\"}");
    resp = serve_recv(&s);
    tap_ok(resp != NULL && strstr(resp, "\"session_id\":") != NULL,
           "multi: list has sessions");
    /* Count occurrences of session_id */
    int count = 0;
    if (resp != NULL) {
        const char *p = resp;
        while ((p = strstr(p, "\"session_id\"")) != NULL) {
            count++;
            p++;
        }
    }
    tap_is_int(count, 2, "multi: list has 2 sessions");
    free(resp);

    /* Shutdown cleans up both */
    serve_shutdown(&s);
}

static void test_destroy_nonexistent(void)
{
    serve_proc_t s = serve_start();

    serve_send(&s,
               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"session/destroy\","
               "\"params\":{\"session_id\":999}}");
    char *resp = serve_recv(&s);
    tap_ok(resp != NULL && strstr(resp, "\"error\"") != NULL,
           "destroy nonexistent: error");
    free(resp);

    serve_shutdown(&s);
}

static void test_empty_list(void)
{
    serve_proc_t s = serve_start();

    serve_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"session/list\"}");
    char *resp = serve_recv(&s);
    tap_ok(resp != NULL && strstr(resp, "\"sessions\":[]") != NULL,
           "empty list: no sessions");
    free(resp);

    serve_shutdown(&s);
}

int main(void)
{
    tap_plan(27);

    test_initialize();
    test_unknown_method();
    test_missing_method();
    test_stdin_eof();
    test_session_create_destroy();
    test_multiple_sessions();
    test_destroy_nonexistent();
    test_empty_list();

    tap_done();
    return 0;
}
