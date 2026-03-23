#include "helpers.h"

/* POSIX 2.2.1 - Backslash escaping */

static void test_backslash(void)
{
    char *out;
    int status;

    out = run("echo hello\\ world", &status);
    tap_is_str(out, "hello world\n", "backslash: space in unquoted context");
    tap_is_int(status, 0, "backslash: space status");
    free(out);

    out = run("echo hel\\\nlo", &status);
    tap_is_str(out, "hello\n", "backslash: line continuation");
    tap_is_int(status, 0, "backslash: line continuation status");
    free(out);

    out = run("echo \"\\$HOME\"", &status);
    tap_is_str(out, "$HOME\n", "backslash: dollar in double quotes");
    tap_is_int(status, 0, "backslash: dollar in double quotes status");
    free(out);

    out = run("echo \"\\\\\"", &status);
    tap_is_str(out, "\\\n", "backslash: backslash in double quotes");
    tap_is_int(status, 0, "backslash: backslash in double quotes status");
    free(out);

    out = run("echo \"\\`echo nope\\`\"", &status);
    tap_is_str(out, "`echo nope`\n", "backslash: backtick in double quotes");
    tap_is_int(status, 0, "backslash: backtick in double quotes status");
    free(out);

    out = run("echo \"\\\"\"", &status);
    tap_is_str(out, "\"\n", "backslash: quote in double quotes");
    tap_is_int(status, 0, "backslash: quote in double quotes status");
    free(out);

    out = run("echo \"\\a\"", &status);
    tap_is_str(out, "\\a\n", "backslash: ordinary char preserved in double quotes");
    tap_is_int(status, 0, "backslash: ordinary char preserved status");
    free(out);

    out = run("echo \"\\z\"", &status);
    tap_is_str(out, "\\z\n", "backslash: \\z preserved in double quotes");
    tap_is_int(status, 0, "backslash: \\z preserved status");
    free(out);
}

/* POSIX 2.2.2 - Single quotes */

static void test_single_quotes(void)
{
    char *out;
    int status;

    out = run("echo '$VAR'", &status);
    tap_is_str(out, "$VAR\n", "single quotes: dollar literal");
    tap_is_int(status, 0, "single quotes: dollar literal status");
    free(out);

    out = run("echo '\\n'", &status);
    tap_is_str(out, "\\n\n", "single quotes: no escape processing");
    tap_is_int(status, 0, "single quotes: no escape processing status");
    free(out);

    out = run("echo 'a\\b'", &status);
    tap_is_str(out, "a\\b\n", "single quotes: backslash literal");
    tap_is_int(status, 0, "single quotes: backslash literal status");
    free(out);

    out = run("echo '$(echo nope)'", &status);
    tap_is_str(out, "$(echo nope)\n", "single quotes: command sub literal");
    tap_is_int(status, 0, "single quotes: command sub literal status");
    free(out);

    out = run("echo '`echo nope`'", &status);
    tap_is_str(out, "`echo nope`\n", "single quotes: backtick literal");
    tap_is_int(status, 0, "single quotes: backtick literal status");
    free(out);
}

/* POSIX 2.2.3 - Double quotes */

static void test_double_quotes(void)
{
    char *out;
    int status;

    out = run("X=hi; echo \"$X\"", &status);
    tap_is_str(out, "hi\n", "double quotes: parameter expansion");
    tap_is_int(status, 0, "double quotes: parameter expansion status");
    free(out);

    out = run("echo \"$(echo hi)\"", &status);
    tap_is_str(out, "hi\n", "double quotes: command substitution");
    tap_is_int(status, 0, "double quotes: command substitution status");
    free(out);

    out = run("echo \"$((1+2))\"", &status);
    tap_is_str(out, "3\n", "double quotes: arithmetic expansion");
    tap_is_int(status, 0, "double quotes: arithmetic expansion status");
    free(out);

    out = run("X=\"a  b\"; echo \"$X\"", &status);
    tap_is_str(out, "a  b\n", "double quotes: field splitting suppressed");
    tap_is_int(status, 0, "double quotes: field splitting suppressed status");
    free(out);

    out = run("echo \"*.c\"", &status);
    tap_is_str(out, "*.c\n", "double quotes: glob suppressed");
    tap_is_int(status, 0, "double quotes: glob suppressed status");
    free(out);

    out = run("X=; echo \"x${X}x\"", &status);
    tap_is_str(out, "xx\n", "double quotes: empty string preserved");
    tap_is_int(status, 0, "double quotes: empty string preserved status");
    free(out);

    out = run("echo \"$(echo \"inner\")\"", &status);
    tap_is_str(out, "inner\n", "double quotes: nested cmdsub");
    tap_is_int(status, 0, "double quotes: nested cmdsub status");
    free(out);
}

/* Mixed and adjacent quoting */

static void test_mixed_quoting(void)
{
    char *out;
    int status;

    out = run("echo \"a\"\"b\"", &status);
    tap_is_str(out, "ab\n", "mixed: adjacent double quotes");
    tap_is_int(status, 0, "mixed: adjacent double quotes status");
    free(out);

    out = run("echo \"a\"'b'c", &status);
    tap_is_str(out, "abc\n", "mixed: double single unquoted");
    tap_is_int(status, 0, "mixed: double single unquoted status");
    free(out);

    out = run("echo 'x'\"y\"z", &status);
    tap_is_str(out, "xyz\n", "mixed: single double unquoted");
    tap_is_int(status, 0, "mixed: single double unquoted status");
    free(out);

    out = run("X=val; echo \"pre'$X'post\"", &status);
    tap_is_str(out, "pre'val'post\n", "mixed: single quotes inside double quotes");
    tap_is_int(status, 0, "mixed: single quotes inside double quotes status");
    free(out);
}

int main(void)
{
    tap_plan(48);

    test_backslash();
    test_single_quotes();
    test_double_quotes();
    test_mixed_quoting();

    return tap_done();
}
