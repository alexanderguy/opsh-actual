#include "helpers.h"

/* POSIX 2.8 - Exit status and error handling */

static void test_exit_status_values(void)
{
    char *out;
    int status;

    out = run("nonexistent_cmd_xyz; echo $?", &status);
    tap_is_str(out, "127\n", "exit: command not found yields 127");
    free(out);

    out = run("exit 42", &status);
    tap_is_int(status, 42, "exit: exit 42 sets status");
    free(out);

    out = run("true", &status);
    tap_is_int(status, 0, "exit: true returns 0");
    free(out);

    out = run("false", &status);
    tap_is_int(status, 1, "exit: false returns 1");
    free(out);

    out = run("true; echo $?", &status);
    tap_is_str(out, "0\n", "exit: $? captures true status");
    free(out);

    out = run("false; echo $?", &status);
    tap_is_str(out, "1\n", "exit: $? captures false status");
    free(out);

    out = run("exit 0", &status);
    tap_is_int(status, 0, "exit: exit 0 sets status 0");
    free(out);

    out = run("(exit 7); echo $?", &status);
    tap_is_str(out, "7\n", "exit: subshell exit captured by $?");
    free(out);

    out = run("sh -c 'exit 3'; echo $?", &status);
    tap_is_str(out, "3\n", "exit: child process exit captured by $?");
    free(out);
}

static void test_errexit(void)
{
    char *out;
    int status;

    out = run("set -e; false; echo no", &status);
    tap_is_str(out, "", "errexit: exits on failure, no output");
    tap_ok(status != 0, "errexit: non-zero status on failure");
    free(out);

    out = run("set -e; if false; then echo no; fi; echo yes", &status);
    tap_is_str(out, "yes\n", "errexit: suppressed in if condition");
    tap_is_int(status, 0, "errexit: if condition status 0");
    free(out);

    out = run("set -e; false && true; echo yes", &status);
    tap_is_str(out, "yes\n", "errexit: suppressed in && LHS");
    tap_is_int(status, 0, "errexit: && LHS status 0");
    free(out);

    out = run("set -e; false || true; echo yes", &status);
    tap_is_str(out, "yes\n", "errexit: suppressed in || LHS");
    tap_is_int(status, 0, "errexit: || LHS status 0");
    free(out);

    out = run("set -e; ! false; echo yes", &status);
    tap_is_str(out, "yes\n", "errexit: suppressed with !");
    tap_is_int(status, 0, "errexit: ! status 0");
    free(out);

    out = run("set -e; while false; do :; done; echo yes", &status);
    tap_is_str(out, "yes\n", "errexit: suppressed in while condition");
    tap_is_int(status, 0, "errexit: while condition status 0");
    free(out);

    out = run("set -e; true; echo yes", &status);
    tap_is_str(out, "yes\n", "errexit: passes on success");
    tap_is_int(status, 0, "errexit: success status 0");
    free(out);
}

static void test_nounset(void)
{
    char *out;
    int status;

    out = run("set -u; echo $UNSET_VAR_XYZ", &status);
    tap_ok(status != 0, "nounset: error on unset variable");
    free(out);

    out = run("set -u; echo ${UNSET_XYZ:-ok}", &status);
    tap_is_str(out, "ok\n", "nounset: ${X:-default} does not error");
    tap_is_int(status, 0, "nounset: ${X:-default} status 0");
    free(out);

    out = run("set -u; echo ${UNSET_XYZ+alt}", &status);
    tap_is_str(out, "\n", "nounset: ${X+alt} empty when unset");
    tap_is_int(status, 0, "nounset: ${X+alt} status 0");
    free(out);
}

static void test_error_handling(void)
{
    char *out;
    int status;

    out = run("exit 255", &status);
    tap_is_int(status, 255, "error: exit 255 returns 255");
    free(out);

    out = run("false; true; echo $?", &status);
    tap_is_str(out, "0\n", "error: $? reflects last command");
    free(out);
}

int main(void)
{
    tap_plan(30);

    test_exit_status_values();
    test_errexit();
    test_nounset();
    test_error_handling();

    return tap_done();
}
