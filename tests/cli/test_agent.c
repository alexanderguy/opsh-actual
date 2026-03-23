#include "../tap.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Agent event stream integration tests: verify --agent-stdio emits
 * JSON-RPC events for script and command lifecycle.
 */

static char *run_with_agent(const char *script_source)
{
    /* Write script to temp file */
    FILE *f = fopen("tmp/test_agent.opsh", "w");
    fprintf(f, "%s", script_source);
    fclose(f);

    int event_pipe[2];
    pipe(event_pipe);

    pid_t pid = fork();
    if (pid == 0) {
        close(event_pipe[0]);
        /* opsh --agent-stdio dups stderr to event fd, then nulls stderr.
         * We redirect stderr to the pipe so we capture events. */
        dup2(event_pipe[1], STDERR_FILENO);
        close(event_pipe[1]);
        /* Redirect stdout to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execl("build/opsh", "opsh", "--agent-stdio", "tmp/test_agent.opsh", NULL);
        _exit(127);
    }

    close(event_pipe[1]);

    /* Read all events */
    char buf[8192] = {0};
    size_t total = 0;
    struct pollfd pfd = {.fd = event_pipe[0], .events = POLLIN};
    while (poll(&pfd, 1, 3000) > 0) {
        ssize_t n = read(event_pipe[0], buf + total, sizeof(buf) - total - 1);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    close(event_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    unlink("tmp/test_agent.opsh");

    return strdup(buf);
}

static void test_script_events(void)
{
    char *events = run_with_agent("echo hello\n");

    tap_ok(strstr(events, "scriptStart") != NULL, "agent: has scriptStart");
    tap_ok(strstr(events, "scriptEnd") != NULL, "agent: has scriptEnd");
    tap_ok(strstr(events, "commandStart") != NULL, "agent: has commandStart");
    tap_ok(strstr(events, "commandEnd") != NULL, "agent: has commandEnd");
    free(events);
}

static void test_multiple_commands(void)
{
    char *events = run_with_agent("echo a\necho b\n");

    /* Should have at least 2 commandStart events */
    char *first = strstr(events, "commandStart");
    tap_ok(first != NULL, "multi cmd: first commandStart");
    if (first != NULL) {
        char *second = strstr(first + 1, "commandStart");
        tap_ok(second != NULL, "multi cmd: second commandStart");
    } else {
        tap_ok(0, "multi cmd: second commandStart");
    }
    free(events);
}

static void test_event_is_json(void)
{
    char *events = run_with_agent("echo test\n");

    /* Events should contain JSON-RPC fields */
    tap_ok(strstr(events, "\"jsonrpc\"") != NULL, "json: has jsonrpc field");
    tap_ok(strstr(events, "\"method\"") != NULL, "json: has method field");
    free(events);
}

int main(void)
{
    tap_plan(8);

    test_script_events();
    test_multiple_commands();
    test_event_is_json();

    tap_done();
    return 0;
}
