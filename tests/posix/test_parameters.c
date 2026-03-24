#include "helpers.h"

/* POSIX 2.5 - Positional parameters */

static void test_positional_params(void)
{
    char *out;
    int status;

    out = run("f() { echo $1 $2 $3; }; f a b c", &status);
    tap_is_str(out, "a b c\n", "positional: $1 $2 $3 in function");
    tap_is_int(status, 0, "positional: $1 $2 $3 status");
    free(out);

    out = run("f() { echo $9; }; f a b c d e f g h i", &status);
    tap_is_str(out, "i\n", "positional: $9 single-digit");
    tap_is_int(status, 0, "positional: $9 status");
    free(out);

    /* ${10} multi-digit positional parameter */
    out = run("f() { echo ${10}; }; f a b c d e f g h i j", &status);
    tap_is_str(out, "j\n", "positional: ${10} multi-digit");
    tap_is_int(status, 0, "positional: ${10} status");
    free(out);

    out = run("set -- x y z; echo $1 $2 $3", &status);
    tap_is_str(out, "x y z\n", "positional: set -- reassigns");
    tap_is_int(status, 0, "positional: set -- status");
    free(out);

    out = run("f() { set -- inside; }; set -- orig; f; echo $1", &status);
    tap_is_str(out, "orig\n", "positional: params restored after function");
    tap_is_int(status, 0, "positional: params restored status");
    free(out);

    out = run("f() { shift; echo $1; }; f a b c", &status);
    tap_is_str(out, "b\n", "positional: shift");
    tap_is_int(status, 0, "positional: shift status");
    free(out);

    out = run("f() { shift 2; echo $1; }; f a b c d", &status);
    tap_is_str(out, "c\n", "positional: shift 2");
    tap_is_int(status, 0, "positional: shift 2 status");
    free(out);

    out = run("set -- a b c; shift; echo $#", &status);
    tap_is_str(out, "2\n", "positional: shift updates $#");
    tap_is_int(status, 0, "positional: shift updates $# status");
    free(out);
}

/* POSIX 2.5.2 - Special parameters */

static void test_special_params(void)
{
    char *out;
    int status;

    out = run("f() { for x in \"$@\"; do echo \"[$x]\"; done; }; f \"a b\" c", &status);
    tap_is_str(out, "[a b]\n[c]\n", "$@: word preservation");
    tap_is_int(status, 0, "$@: word preservation status");
    free(out);

    out = run("f() { for x in \"$@\"; do echo x; done; echo done; }; f", &status);
    tap_is_str(out, "done\n", "$@: zero fields when no args");
    tap_is_int(status, 0, "$@: zero fields status");
    free(out);

    out = run("f() { IFS=:; echo \"$*\"; }; f a b c", &status);
    tap_is_str(out, "a:b:c\n", "$*: joined with IFS");
    tap_is_int(status, 0, "$*: joined with IFS status");
    free(out);

    out = run("f() { echo $#; }; f a b c", &status);
    tap_is_str(out, "3\n", "$#: argument count");
    tap_is_int(status, 0, "$#: argument count status");
    free(out);

    out = run("true; echo $?", &status);
    tap_is_str(out, "0\n", "$?: true returns 0");
    tap_is_int(status, 0, "$?: true status");
    free(out);

    out = run("false; echo $?", &status);
    tap_is_str(out, "1\n", "$?: false returns 1");
    tap_is_int(status, 0, "$?: false echo status");
    free(out);

    out = run("echo $$", &status);
    tap_ok(strlen(out) > 1, "$$: non-empty output");
    tap_ok(out[0] >= '0' && out[0] <= '9', "$$: starts with digit");
    tap_is_int(status, 0, "$$: status");
    free(out);

    out = run("f() { echo $0; }; f", &status);
    tap_ok(strlen(out) > 1, "$0: non-empty in function");
    tap_is_int(status, 0, "$0: unchanged in function status");
    free(out);

    out = run("f() { echo $#; }; f x y", &status);
    tap_is_str(out, "2\n", "$#: in function");
    tap_is_int(status, 0, "$#: in function status");
    free(out);
}

/* Interaction tests: $@, $*, splitting behavior */

static void test_param_interactions(void)
{
    char *out;
    int status;

    out =
        run("f() { for x in \"$@\"; do echo \"($x)\"; done; }; f one \"two three\" four", &status);
    tap_is_str(out, "(one)\n(two three)\n(four)\n", "$@: iterates preserving words");
    tap_is_int(status, 0, "$@: iterates preserving words status");
    free(out);

    out = run("f() { IFS=-; echo \"$*\"; }; f x y z", &status);
    tap_is_str(out, "x-y-z\n", "$*: custom IFS joining");
    tap_is_int(status, 0, "$*: custom IFS joining status");
    free(out);

    out = run("f() { echo $@; }; f \"a b\" c", &status);
    tap_is_str(out, "a b c\n", "unquoted $@: subject to splitting");
    tap_is_int(status, 0, "unquoted $@: subject to splitting status");
    free(out);

    out = run("f() { echo \"$*\"; }; f", &status);
    tap_is_str(out, "\n", "$*: empty when no args");
    tap_is_int(status, 0, "$*: empty when no args status");
    free(out);

    out = run("f() { echo $#; }; f", &status);
    tap_is_str(out, "0\n", "$#: zero when no args");
    tap_is_int(status, 0, "$#: zero when no args status");
    free(out);

    out = run("set -- a b c; echo $#", &status);
    tap_is_str(out, "3\n", "$#: after set --");
    tap_is_int(status, 0, "$#: after set -- status");
    free(out);

    out = run("set --; echo $#", &status);
    tap_is_str(out, "0\n", "$#: after set -- with no args");
    tap_is_int(status, 0, "$#: after set -- with no args status");
    free(out);

    out = run("f() { set -- new args; echo $1 $2; }; f old; echo done", &status);
    tap_is_str(out, "new args\ndone\n", "set -- in function: local effect");
    tap_is_int(status, 0, "set -- in function: local effect status");
    free(out);
}

int main(void)
{
    tap_plan(51);

    test_positional_params();
    test_special_params();
    test_param_interactions();

    return tap_done();
}
