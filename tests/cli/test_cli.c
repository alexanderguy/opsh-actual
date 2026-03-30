#include "../tap.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * CLI integration tests: spawn opsh with various arguments and verify output.
 */

typedef struct {
    int status;
    char *out;
    char *err;
} run_result_t;

static run_result_t run_opsh(const char *args[], int nargs)
{
    int out_pipe[2], err_pipe[2];
    pipe(out_pipe);
    pipe(err_pipe);

    pid_t pid = fork();
    if (pid == 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);

        /* Build argv: "build/opsh" + args + NULL */
        char **argv = calloc((size_t)(nargs + 2), sizeof(char *));
        argv[0] = "build/opsh";
        int i;
        for (i = 0; i < nargs; i++) {
            argv[i + 1] = (char *)args[i];
        }
        argv[nargs + 1] = NULL;
        execv("build/opsh", argv);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    /* Read stdout */
    char out_buf[4096] = {0};
    ssize_t n = read(out_pipe[0], out_buf, sizeof(out_buf) - 1);
    if (n < 0)
        n = 0;
    out_buf[n] = '\0';
    close(out_pipe[0]);

    /* Read stderr */
    char err_buf[4096] = {0};
    n = read(err_pipe[0], err_buf, sizeof(err_buf) - 1);
    if (n < 0)
        n = 0;
    err_buf[n] = '\0';
    close(err_pipe[0]);

    int wstatus;
    waitpid(pid, &wstatus, 0);

    run_result_t r;
    r.status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
    r.out = strdup(out_buf);
    r.err = strdup(err_buf);
    return r;
}

static void free_result(run_result_t *r)
{
    free(r->out);
    free(r->err);
}

/* ---- Tests ---- */

static void test_c_flag(void)
{
    const char *args[] = {"-c", "echo hello"};
    run_result_t r = run_opsh(args, 2);
    tap_is_str(r.out, "hello\n", "-c: basic output");
    tap_is_int(r.status, 0, "-c: exit status 0");
    free_result(&r);
}

static void test_c_with_args(void)
{
    const char *args[] = {"-c", "echo $0 $1", "myname", "arg1"};
    run_result_t r = run_opsh(args, 4);
    tap_is_str(r.out, "myname arg1\n", "-c args: $0 and $1");
    free_result(&r);
}

static void test_c_dash_dash(void)
{
    const char *args[] = {"-c", "echo $0 $1", "--", "a", "b"};
    run_result_t r = run_opsh(args, 5);
    tap_is_str(r.out, "-- a\n", "-c --: -- is $0");
    free_result(&r);
}

static void test_script_file(void)
{
    /* Write a temp script */
    FILE *f = fopen("tmp/test_cli.opsh", "w");
    fprintf(f, "echo from_script\n");
    fclose(f);

    const char *args[] = {"tmp/test_cli.opsh"};
    run_result_t r = run_opsh(args, 1);
    tap_is_str(r.out, "from_script\n", "script file: output");
    tap_is_int(r.status, 0, "script file: status 0");
    free_result(&r);
    unlink("tmp/test_cli.opsh");
}

static void test_script_args(void)
{
    FILE *f = fopen("tmp/test_cli2.opsh", "w");
    fprintf(f, "echo $1 $2\n");
    fclose(f);

    const char *args[] = {"tmp/test_cli2.opsh", "hello", "world"};
    run_result_t r = run_opsh(args, 3);
    tap_is_str(r.out, "hello world\n", "script args: $1 $2");
    free_result(&r);
    unlink("tmp/test_cli2.opsh");
}

static void test_no_args(void)
{
    const char *args[] = {NULL};
    run_result_t r = run_opsh(args, 0);
    tap_ok(r.status != 0, "no args: nonzero exit");
    tap_ok(strstr(r.err, "no script") != NULL, "no args: error message");
    free_result(&r);
}

static void test_nonexistent_script(void)
{
    const char *args[] = {"nonexistent_script_xyz.opsh"};
    run_result_t r = run_opsh(args, 1);
    tap_ok(r.status != 0, "missing script: nonzero exit");
    free_result(&r);
}

static void test_c_exit_status(void)
{
    const char *args[] = {"-c", "exit 42"};
    run_result_t r = run_opsh(args, 2);
    tap_is_int(r.status, 42, "-c exit: status 42");
    free_result(&r);
}

static void test_help(void)
{
    const char *args[] = {"--help"};
    run_result_t r = run_opsh(args, 1);
    tap_is_int(r.status, 0, "--help: status 0");
    tap_ok(strstr(r.err, "opsh") != NULL, "--help: shows usage");
    free_result(&r);
}

static void test_format_subcommand(void)
{
    FILE *f = fopen("tmp/test_fmt.opsh", "w");
    fprintf(f, "echo    hello\n");
    fclose(f);

    const char *args[] = {"format", "tmp/test_fmt.opsh"};
    run_result_t r = run_opsh(args, 2);
    tap_is_str(r.out, "echo hello\n", "format: normalizes whitespace");
    tap_is_int(r.status, 0, "format: status 0");
    free_result(&r);
    unlink("tmp/test_fmt.opsh");
}

static void test_lint_subcommand(void)
{
    FILE *f = fopen("tmp/test_lint.opsh", "w");
    fprintf(f, "x=hello\necho $x\n");
    fclose(f);

    const char *args[] = {"lint", "tmp/test_lint.opsh"};
    run_result_t r = run_opsh(args, 2);
    tap_ok(strstr(r.out, "SC2086") != NULL || strstr(r.err, "SC2086") != NULL,
           "lint: detects SC2086");
    free_result(&r);
    unlink("tmp/test_lint.opsh");
}

static void test_unknown_option(void)
{
    const char *args[] = {"--bogus"};
    run_result_t r = run_opsh(args, 1);
    tap_is_int(r.status, 1, "unknown option: exit 1");
    tap_ok(strstr(r.err, "unknown option") != NULL, "unknown option: error message");
    free_result(&r);
}

static void test_c_missing_arg(void)
{
    const char *args[] = {"-c"};
    run_result_t r = run_opsh(args, 1);
    tap_is_int(r.status, 1, "-c no arg: exit 1");
    tap_ok(strstr(r.err, "-c requires") != NULL, "-c no arg: error message");
    free_result(&r);
}

static void test_o_missing_arg(void)
{
    const char *args[] = {"-o"};
    run_result_t r = run_opsh(args, 1);
    tap_is_int(r.status, 1, "-o no arg: exit 1");
    tap_ok(strstr(r.err, "-o requires") != NULL, "-o no arg: error message");
    free_result(&r);
}

static void test_serve_unexpected_arg(void)
{
    const char *args[] = {"serve", "--bogus"};
    run_result_t r = run_opsh(args, 2);
    tap_is_int(r.status, 1, "serve bogus: exit 1");
    tap_ok(strstr(r.err, "unexpected argument") != NULL, "serve bogus: error message");
    free_result(&r);
}

static void test_mcp_unexpected_arg(void)
{
    const char *args[] = {"mcp", "--bogus"};
    run_result_t r = run_opsh(args, 2);
    tap_is_int(r.status, 1, "mcp bogus: exit 1");
    tap_ok(strstr(r.err, "unexpected argument") != NULL, "mcp bogus: error message");
    free_result(&r);
}

static void test_lsp_unexpected_arg(void)
{
    const char *args[] = {"lsp", "--bogus"};
    run_result_t r = run_opsh(args, 2);
    tap_is_int(r.status, 1, "lsp bogus: exit 1");
    tap_ok(strstr(r.err, "unexpected argument") != NULL, "lsp bogus: error message");
    free_result(&r);
}

static void test_o_without_build(void)
{
    FILE *f = fopen("tmp/test_o.opsh", "w");
    fprintf(f, "echo hello\n");
    fclose(f);

    const char *args[] = {"-o", "tmp/test_o_out", "tmp/test_o.opsh"};
    run_result_t r = run_opsh(args, 3);
    tap_is_int(r.status, 1, "-o without build: exit 1");
    tap_ok(strstr(r.err, "-o is only valid") != NULL, "-o without build: error message");
    free_result(&r);
    unlink("tmp/test_o.opsh");
}

int main(void)
{
    tap_plan(30);

    test_c_flag();
    test_c_with_args();
    test_c_dash_dash();
    test_script_file();
    test_script_args();
    test_no_args();
    test_nonexistent_script();
    test_c_exit_status();
    test_help();
    test_format_subcommand();
    test_lint_subcommand();
    test_unknown_option();
    test_c_missing_arg();
    test_o_missing_arg();
    test_serve_unexpected_arg();
    test_mcp_unexpected_arg();
    test_lsp_unexpected_arg();
    test_o_without_build();

    tap_done();
    return 0;
}
