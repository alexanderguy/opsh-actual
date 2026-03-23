#include "serve/serve.h"

#include "foundation/json.h"
#include "foundation/jsonrpc.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* JSON-RPC 2.0 error codes */
#define JSONRPC_PARSE_ERROR      (-32700)
#define JSONRPC_INVALID_REQUEST  (-32600)
#define JSONRPC_METHOD_NOT_FOUND (-32601)
#define JSONRPC_INTERNAL_ERROR   (-32603)

#define MAX_SESSIONS 16

typedef struct {
    int active;
    int session_id;
    pid_t pid;
    int cmd_fd;     /* write commands to child stdin */
    int out_fd;     /* read stdout from child */
    int err_fd;     /* read stderr from child */
    int control_fd; /* read completion signals from child */
} session_t;

static session_t sessions[MAX_SESSIONS];
static int next_session_id = 1;
static char *self_exe_path;

static session_t *find_session(int session_id)
{
    int i;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].session_id == session_id) {
            return &sessions[i];
        }
    }
    return NULL;
}

static session_t *alloc_session(void)
{
    int i;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
            return &sessions[i];
        }
    }
    return NULL;
}

static void session_close(session_t *s)
{
    if (!s->active) {
        return;
    }

    close(s->cmd_fd);
    close(s->out_fd);
    close(s->err_fd);
    close(s->control_fd);

    /* Try to reap; if child is still running, escalate gracefully */
    int status;
    pid_t ret = waitpid(s->pid, &status, WNOHANG);
    if (ret == 0) {
        /* Child still alive after pipe close — try SIGTERM first */
        kill(s->pid, SIGTERM);
        usleep(50000); /* 50ms */
        ret = waitpid(s->pid, &status, WNOHANG);
        if (ret == 0) {
            kill(s->pid, SIGKILL);
            waitpid(s->pid, &status, 0);
        }
    }

    s->active = 0;
}

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
    json_write_string(&result, "session/destroy");
    strbuf_append_byte(&result, ',');
    json_write_string(&result, "session/list");
    json_end_array(&result);
    json_end_object(&result);
    jsonrpc_send_result(stdout, id, result.contents);
    strbuf_destroy(&result);
}

static void handle_session_create(int64_t id, const char *msg)
{
    session_t *s = alloc_session();
    if (s == NULL) {
        jsonrpc_send_error(stdout, id, JSONRPC_INTERNAL_ERROR,
                           "Maximum sessions reached");
        return;
    }

    /* Optional cwd parameter */
    char *cwd = json_find_nested_string(msg, "cwd");

    int to_child[2], from_child[2], err_child[2], control[2];
    to_child[0] = to_child[1] = -1;
    from_child[0] = from_child[1] = -1;
    err_child[0] = err_child[1] = -1;
    control[0] = control[1] = -1;

    if (pipe(to_child) != 0 || pipe(from_child) != 0 ||
        pipe(err_child) != 0 || pipe(control) != 0) {
        int i;
        int *fds[] = {to_child, from_child, err_child, control};
        for (i = 0; i < 4; i++) {
            if (fds[i][0] >= 0) close(fds[i][0]);
            if (fds[i][1] >= 0) close(fds[i][1]);
        }
        free(cwd);
        jsonrpc_send_error(stdout, id, JSONRPC_INTERNAL_ERROR,
                           "Failed to create pipes");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        close(err_child[0]);
        close(err_child[1]);
        close(control[0]);
        close(control[1]);
        free(cwd);
        jsonrpc_send_error(stdout, id, JSONRPC_INTERNAL_ERROR,
                           "Fork failed");
        return;
    }

    if (pid == 0) {
        /* Child */
        close(to_child[1]);
        close(from_child[0]);
        close(err_child[0]);
        close(control[0]);

        if (cwd != NULL) {
            if (chdir(cwd) != 0) {
                _exit(126);
            }
        }

        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(err_child[1], STDERR_FILENO);
        if (control[1] != 3) {
            dup2(control[1], 3);
        }

        /* Close originals only if they're not 0-3 */
        if (to_child[0] > 3)
            close(to_child[0]);
        if (from_child[1] > 3)
            close(from_child[1]);
        if (err_child[1] > 3)
            close(err_child[1]);
        if (control[1] > 3)
            close(control[1]);

        execl(self_exe_path, "opsh", "--child-loop", NULL);
        _exit(127);
    }

    /* Parent */
    close(to_child[0]);
    close(from_child[1]);
    close(err_child[1]);
    close(control[1]);
    free(cwd);

    s->active = 1;
    s->session_id = next_session_id++;
    s->pid = pid;
    s->cmd_fd = to_child[1];
    s->out_fd = from_child[0];
    s->err_fd = err_child[0];
    s->control_fd = control[0];

    strbuf_t result;
    strbuf_init(&result);
    json_begin_object(&result);
    json_key_int(&result, "session_id", s->session_id);
    json_end_object(&result);
    jsonrpc_send_result(stdout, id, result.contents);
    strbuf_destroy(&result);
}

static int64_t get_session_id_param(const char *msg)
{
    /* Try top-level first, then search nested (inside params) */
    int64_t sid = json_get_int(msg, "session_id");
    if (sid >= 0) {
        return sid;
    }
    /* Brute-force search for "session_id":N anywhere in the message */
    const char *needle = "\"session_id\"";
    const char *p = strstr(msg, needle);
    if (p == NULL) {
        return -1;
    }
    p += strlen(needle);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p >= '0' && *p <= '9') {
        return strtoll(p, NULL, 10);
    }
    return -1;
}

static void handle_session_destroy(int64_t id, const char *msg)
{
    int64_t sid = get_session_id_param(msg);

    session_t *s = find_session((int)sid);
    if (s == NULL) {
        jsonrpc_send_error(stdout, id, JSONRPC_INTERNAL_ERROR,
                           "Session not found");
        return;
    }

    session_close(s);
    jsonrpc_send_result(stdout, id, "null");
}

static void handle_session_list(int64_t id)
{
    strbuf_t result;
    strbuf_init(&result);
    strbuf_append_str(&result, "{\"sessions\":[");

    int first = 1;
    int i;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active) {
            if (!first) {
                strbuf_append_byte(&result, ',');
            }
            first = 0;
            json_begin_object(&result);
            json_key_int(&result, "session_id", sessions[i].session_id);
            json_end_object(&result);
        }
    }

    strbuf_append_str(&result, "]}");
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
        /* Clean up all sessions before shutting down */
        {
            int i;
            for (i = 0; i < MAX_SESSIONS; i++) {
                if (sessions[i].active) {
                    session_close(&sessions[i]);
                }
            }
        }
        jsonrpc_send_result(stdout, id, "null");
        free(method);
        return 0;
    } else if (strcmp(method, "session/create") == 0) {
        handle_session_create(id, msg);
    } else if (strcmp(method, "session/destroy") == 0) {
        handle_session_destroy(id, msg);
    } else if (strcmp(method, "session/list") == 0) {
        handle_session_list(id);
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
    self_exe_path = get_self_exe();
    if (self_exe_path == NULL) {
        fprintf(stderr, "opsh: cannot determine own executable path\n");
        return 1;
    }

    memset(sessions, 0, sizeof(sessions));

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

    /* Clean up any remaining sessions */
    {
        int i;
        for (i = 0; i < MAX_SESSIONS; i++) {
            if (sessions[i].active) {
                session_close(&sessions[i]);
            }
        }
    }

    free(self_exe_path);
    return 0;
}
