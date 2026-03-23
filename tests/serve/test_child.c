#include "../tap.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Integration tests for the child command-loop mode.
 * Spawns opsh --child-loop with pipes and exchanges length-prefixed commands.
 */

#define CHILD_TIMEOUT_MS 3000

typedef struct {
    pid_t pid;
    int to_fd;      /* write commands to child stdin */
    int from_fd;    /* read output from child stdout */
    int err_fd;     /* read errors from child stderr */
    int control_fd; /* read completion signals */
} child_proc_t;

static child_proc_t child_start(void)
{
    int to_child[2], from_child[2], err_child[2], control[2];
    pipe(to_child);
    pipe(from_child);
    pipe(err_child);
    pipe(control);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: set up fds carefully to avoid closing fd 3 prematurely.
         * Do all dup2s first, then close originals only if they aren't
         * one of our target fds (0, 1, 2, 3). */
        close(to_child[1]);
        close(from_child[0]);
        close(err_child[0]);
        close(control[0]);
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
        execl("build/opsh", "opsh", "--child-loop", NULL);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    close(err_child[1]);
    close(control[1]);

    child_proc_t c;
    c.pid = pid;
    c.to_fd = to_child[1];
    c.from_fd = from_child[0];
    c.err_fd = err_child[0];
    c.control_fd = control[0];
    return c;
}

static void child_stop(child_proc_t *c)
{
    close(c->to_fd);
    close(c->from_fd);
    close(c->err_fd);
    close(c->control_fd);
    int status;
    waitpid(c->pid, &status, 0);
}

/* Send a length-prefixed command */
static void child_send(child_proc_t *c, const char *source)
{
    uint32_t len = (uint32_t)strlen(source);
    uint8_t buf[4];
    buf[0] = len & 0xFF;
    buf[1] = (len >> 8) & 0xFF;
    buf[2] = (len >> 16) & 0xFF;
    buf[3] = (len >> 24) & 0xFF;
    write(c->to_fd, buf, 4);
    write(c->to_fd, source, len);
}

/* Wait for completion signal, return exit status */
static int child_wait_done(child_proc_t *c)
{
    struct pollfd pfd = {.fd = c->control_fd, .events = POLLIN};
    if (poll(&pfd, 1, CHILD_TIMEOUT_MS) <= 0) {
        return -1; /* timeout */
    }
    uint8_t status;
    ssize_t n = read(c->control_fd, &status, 1);
    if (n != 1) {
        return -1;
    }
    return (int)status;
}

/* Read available output from stdout */
static char *child_read_output(child_proc_t *c)
{
    struct pollfd pfd = {.fd = c->from_fd, .events = POLLIN};
    char buf[4096] = {0};
    size_t total = 0;
    /* Read until no more data available */
    while (poll(&pfd, 1, 100) > 0 && total < sizeof(buf) - 1) {
        ssize_t n = read(c->from_fd, buf + total, sizeof(buf) - total - 1);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    return strdup(buf);
}

/* ---- Tests ---- */

static void test_basic_command(void)
{
    child_proc_t c = child_start();

    child_send(&c, "echo hello");
    int status = child_wait_done(&c);
    char *out = child_read_output(&c);

    tap_is_int(status, 0, "basic: exit status 0");
    tap_is_str(out, "hello\n", "basic: output");
    free(out);

    child_stop(&c);
}

static void test_persistent_state(void)
{
    child_proc_t c = child_start();

    child_send(&c, "x=persistent");
    child_wait_done(&c);

    child_send(&c, "echo $x");
    int status = child_wait_done(&c);
    char *out = child_read_output(&c);

    tap_is_int(status, 0, "persistent: status 0");
    tap_is_str(out, "persistent\n", "persistent: variable survives");
    free(out);

    child_stop(&c);
}

static void test_multiple_commands(void)
{
    child_proc_t c = child_start();

    child_send(&c, "echo first");
    child_wait_done(&c);
    char *out1 = child_read_output(&c);
    tap_is_str(out1, "first\n", "multi: first output");
    free(out1);

    child_send(&c, "echo second");
    child_wait_done(&c);
    char *out2 = child_read_output(&c);
    tap_is_str(out2, "second\n", "multi: second output");
    free(out2);

    child_stop(&c);
}

static void test_exit_does_not_kill_session(void)
{
    child_proc_t c = child_start();

    child_send(&c, "exit 42");
    int status1 = child_wait_done(&c);
    tap_is_int(status1, 42, "exit: returns 42");

    /* Session should still be alive */
    child_send(&c, "echo still alive");
    int status2 = child_wait_done(&c);
    char *out = child_read_output(&c);
    tap_is_int(status2, 0, "exit: session continues");
    tap_is_str(out, "still alive\n", "exit: output after exit");
    free(out);

    child_stop(&c);
}

static void test_stdin_eof(void)
{
    child_proc_t c = child_start();

    /* Close stdin -- child should exit */
    close(c.to_fd);
    c.to_fd = -1;

    int status;
    waitpid(c.pid, &status, 0);
    tap_ok(WIFEXITED(status), "eof: child exited");

    close(c.from_fd);
    close(c.err_fd);
    close(c.control_fd);
}

static void test_function_persistence(void)
{
    child_proc_t c = child_start();

    child_send(&c, "greet() { echo \"hello $1\"; }");
    child_wait_done(&c);

    child_send(&c, "greet world");
    child_wait_done(&c);
    char *out = child_read_output(&c);
    tap_is_str(out, "hello world\n", "func persist: function callable");
    free(out);

    child_stop(&c);
}

int main(void)
{
    tap_plan(11);

    test_basic_command();
    test_persistent_state();
    test_multiple_commands();
    test_exit_does_not_kill_session();
    test_stdin_eof();
    test_function_persistence();

    tap_done();
    return 0;
}
