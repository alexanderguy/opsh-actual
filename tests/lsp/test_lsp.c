#include "../tap.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * LSP integration tests: spawn opsh lsp, exchange JSON-RPC messages.
 */

#define LSP_TIMEOUT_MS 3000

typedef struct {
    pid_t pid;
    int to_fd;   /* write to LSP stdin */
    int from_fd; /* read from LSP stdout */
} lsp_proc_t;

static lsp_proc_t lsp_start(void)
{
    int to_child[2], from_child[2];
    pipe(to_child);
    pipe(from_child);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: exec opsh lsp */
        close(to_child[1]);
        close(from_child[0]);
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);
        close(from_child[1]);
        /* Redirect stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl("build/opsh", "opsh", "lsp", NULL);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);

    lsp_proc_t proc;
    proc.pid = pid;
    proc.to_fd = to_child[1];
    proc.from_fd = from_child[0];
    return proc;
}

static void lsp_stop(lsp_proc_t *proc)
{
    close(proc->to_fd);
    close(proc->from_fd);
    int status;
    waitpid(proc->pid, &status, 0);
}

static void lsp_send(int fd, const char *json)
{
    size_t len = strlen(json);
    char header[64];
    snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
    write(fd, header, strlen(header));
    write(fd, json, len);
}

/* Read a Content-Length framed message with poll timeout.
 * Returns malloc'd body or NULL on timeout/error. */
static char *lsp_recv(int fd)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};

    /* Read headers character by character to find Content-Length */
    int content_length = -1;
    char line[256];
    int li = 0;

    for (;;) {
        if (poll(&pfd, 1, LSP_TIMEOUT_MS) <= 0) {
            return NULL; /* timeout */
        }
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) {
            return NULL;
        }
        if (c == '\n') {
            line[li] = '\0';
            if (li == 0 || (li == 1 && line[0] == '\r')) {
                break; /* blank line = end of headers */
            }
            if (strncmp(line, "Content-Length:", 15) == 0) {
                content_length = atoi(line + 15);
            }
            li = 0;
        } else {
            if (li < (int)sizeof(line) - 1) {
                line[li++] = c;
            }
        }
    }

    if (content_length <= 0) {
        return NULL;
    }

    char *body = malloc((size_t)content_length + 1);
    size_t total = 0;
    while (total < (size_t)content_length) {
        if (poll(&pfd, 1, LSP_TIMEOUT_MS) <= 0) {
            free(body);
            return NULL;
        }
        ssize_t n = read(fd, body + total, (size_t)content_length - total);
        if (n <= 0) {
            free(body);
            return NULL;
        }
        total += (size_t)n;
    }
    body[total] = '\0';
    return body;
}

/* Send initialize + initialized, return the initialize response */
static char *lsp_handshake(lsp_proc_t *proc)
{
    lsp_send(proc->to_fd, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                          "\"params\":{\"capabilities\":{}}}");
    char *resp = lsp_recv(proc->from_fd);

    lsp_send(proc->to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");

    return resp;
}

static bool json_has(const char *json, const char *substr)
{
    return json != NULL && strstr(json, substr) != NULL;
}

/* ---- Tests ---- */

static void test_initialize(void)
{
    lsp_proc_t proc = lsp_start();
    char *resp = lsp_handshake(&proc);

    tap_ok(resp != NULL, "initialize: got response");
    tap_ok(json_has(resp, "\"textDocumentSync\""), "initialize: has textDocumentSync");
    tap_ok(json_has(resp, "\"completionProvider\""), "initialize: has completionProvider");

    free(resp);

    /* Shutdown */
    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":{}}");
    char *shut = lsp_recv(proc.from_fd);
    tap_ok(shut != NULL, "shutdown: got response");
    free(shut);

    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":{}}");
    lsp_stop(&proc);
}

static void test_diagnostics_clean(void)
{
    lsp_proc_t proc = lsp_start();
    char *init = lsp_handshake(&proc);
    free(init);

    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                         "\"params\":{\"textDocument\":{\"uri\":\"file:///test.opsh\","
                         "\"languageId\":\"opsh\",\"version\":1,"
                         "\"text\":\"echo hello\\n\"}}}");

    char *diag = lsp_recv(proc.from_fd);
    tap_ok(diag != NULL, "clean diagnostics: got notification");
    tap_ok(json_has(diag, "publishDiagnostics"), "clean diagnostics: is publishDiagnostics");
    tap_ok(json_has(diag, "\"diagnostics\":[]"), "clean diagnostics: empty array");
    free(diag);

    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":{}}");
    lsp_stop(&proc);
}

static void test_diagnostics_error(void)
{
    lsp_proc_t proc = lsp_start();
    char *init = lsp_handshake(&proc);
    free(init);

    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                         "\"params\":{\"textDocument\":{\"uri\":\"file:///test.opsh\","
                         "\"languageId\":\"opsh\",\"version\":1,"
                         "\"text\":\"if; then\\n\"}}}");

    char *diag = lsp_recv(proc.from_fd);
    tap_ok(diag != NULL, "error diagnostics: got notification");
    tap_ok(json_has(diag, "publishDiagnostics"), "error diagnostics: is publishDiagnostics");
    tap_ok(!json_has(diag, "\"diagnostics\":[]"), "error diagnostics: non-empty");
    tap_ok(json_has(diag, "\"severity\":1"), "error diagnostics: severity is error");
    free(diag);

    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":{}}");
    lsp_stop(&proc);
}

static void test_diagnostics_lint(void)
{
    lsp_proc_t proc = lsp_start();
    char *init = lsp_handshake(&proc);
    free(init);

    /* SC2086: unquoted variable */
    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                         "\"params\":{\"textDocument\":{\"uri\":\"file:///test.opsh\","
                         "\"languageId\":\"opsh\",\"version\":1,"
                         "\"text\":\"x=hello\\necho $x\\n\"}}}");

    char *diag = lsp_recv(proc.from_fd);
    tap_ok(diag != NULL, "lint diagnostics: got notification");
    tap_ok(json_has(diag, "SC2086"), "lint diagnostics: has SC2086");
    free(diag);

    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":{}}");
    lsp_stop(&proc);
}

static void test_completion(void)
{
    lsp_proc_t proc = lsp_start();
    char *init = lsp_handshake(&proc);
    free(init);

    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"textDocument/completion\","
                         "\"params\":{\"textDocument\":{\"uri\":\"file:///test.opsh\"},"
                         "\"position\":{\"line\":0,\"character\":0}}}");

    char *resp = lsp_recv(proc.from_fd);
    tap_ok(resp != NULL, "completion: got response");
    tap_ok(json_has(resp, "\"echo\""), "completion: has echo");
    tap_ok(json_has(resp, "\"eval\""), "completion: has eval");
    tap_ok(json_has(resp, "\"set\""), "completion: has set");
    free(resp);

    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":{}}");
    lsp_stop(&proc);
}

static void test_did_change(void)
{
    lsp_proc_t proc = lsp_start();
    char *init = lsp_handshake(&proc);
    free(init);

    /* Open with valid code */
    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                         "\"params\":{\"textDocument\":{\"uri\":\"file:///test.opsh\","
                         "\"languageId\":\"opsh\",\"version\":1,"
                         "\"text\":\"echo hello\\n\"}}}");

    char *diag1 = lsp_recv(proc.from_fd);
    tap_ok(json_has(diag1, "\"diagnostics\":[]"), "didChange: initially clean");
    free(diag1);

    /* Change to broken code */
    lsp_send(proc.to_fd,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
             "\"params\":{\"textDocument\":{\"uri\":\"file:///test.opsh\",\"version\":2},"
             "\"contentChanges\":[{\"text\":\"if; then\\n\"}]}}");

    char *diag2 = lsp_recv(proc.from_fd);
    tap_ok(diag2 != NULL, "didChange: got updated diagnostics");
    tap_ok(!json_has(diag2, "\"diagnostics\":[]"), "didChange: now has errors");
    free(diag2);

    lsp_send(proc.to_fd, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":{}}");
    lsp_stop(&proc);
}

int main(void)
{
    tap_plan(20);

    test_initialize();
    test_diagnostics_clean();
    test_diagnostics_error();
    test_diagnostics_lint();
    test_completion();
    test_did_change();

    tap_done();
    return 0;
}
