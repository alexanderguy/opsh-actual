#include "helpers.h"

#include <ctype.h>

/* POSIX Job Control */

static void test_background_basics(void)
{
    char *out;
    int status;

    out = run("true & wait; echo $?", &status);
    tap_is_str(out, "0\n", "bg: wait returns 0 for true &");
    tap_is_int(status, 0, "bg: overall status 0");
    free(out);

    out = run("false & wait; echo $?", &status);
    tap_is_str(out, "1\n", "bg: wait returns 1 for false &");
    tap_is_int(status, 0, "bg: false & does not fail script");
    free(out);

    out = run("true & echo $!", &status);
    tap_ok(out != NULL && strlen(out) > 1, "bg: $! is non-empty");
    tap_ok(out != NULL && isdigit((unsigned char)out[0]), "bg: $! starts with digit");
    free(out);

    out = run("sleep 0 & wait $!; echo done", &status);
    tap_is_str(out, "done\n", "bg: wait for specific PID");
    tap_is_int(status, 0, "bg: wait $! status 0");
    free(out);

    out = run("true & true & wait; echo done", &status);
    tap_is_str(out, "done\n", "bg: wait no args waits for all");
    tap_is_int(status, 0, "bg: wait all status 0");
    free(out);
}

static void test_job_builtins(void)
{
    char *out;
    int status;

    out = run("type fg", &status);
    tap_is_str(out, "fg is a shell builtin\n", "type: fg is a builtin");
    tap_is_int(status, 0, "type: fg status 0");
    free(out);

    out = run("type bg", &status);
    tap_is_str(out, "bg is a shell builtin\n", "type: bg is a builtin");
    tap_is_int(status, 0, "type: bg status 0");
    free(out);

    out = run("type jobs", &status);
    tap_is_str(out, "jobs is a shell builtin\n", "type: jobs is a builtin");
    tap_is_int(status, 0, "type: jobs status 0");
    free(out);
}

static void test_job_table(void)
{
    char *out;
    int status;

    out = run("true & wait; jobs", &status);
    tap_is_int(status, 0, "jobs: runs without error after wait");
    tap_is_str(out, "", "jobs: empty after wait completes");
    free(out);

    out = run("echo hello & wait", &status);
    tap_ok(out != NULL && strstr(out, "hello") != NULL, "jobs: bg output captured by wait");
    tap_is_int(status, 0, "jobs: echo & wait status 0");
    free(out);
}

static void test_background_interaction(void)
{
    char *out;
    int status;

    out = run("x=before; true & x=after; echo $x", &status);
    tap_is_str(out, "after\n", "bg: foreground continues after &");
    tap_is_int(status, 0, "bg: foreground status 0");
    free(out);

    out = run("echo a & echo b & wait", &status);
    tap_ok(out != NULL && strstr(out, "a") != NULL, "bg: first bg output present");
    tap_ok(out != NULL && strstr(out, "b") != NULL, "bg: second bg output present");
    tap_is_int(status, 0, "bg: multiple bg status 0");
    free(out);
}

int main(void)
{
    tap_plan(25);

    test_background_basics();
    test_job_builtins();
    test_job_table();
    test_background_interaction();

    return tap_done();
}
