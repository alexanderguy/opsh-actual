#include "helpers.h"

static void test_simple_commands(void)
{
    int status;
    char *out;

    /* Assignment affects env */
    out = run("X=hello; echo $X", &status);
    tap_is_str(out, "hello\n", "simple cmd: assignment affects env");
    tap_is_int(status, 0, "simple cmd: assignment exit status 0");
    free(out);

    /* Assignment without command persists */
    out = run("X=val; echo $X", &status);
    tap_is_str(out, "val\n", "simple cmd: assignment persists");
    free(out);

    /* Command not found: status 127 */
    out = run("nonexistent_cmd_xyz_999", &status);
    tap_is_int(status, 127, "simple cmd: not found gives 127");
    free(out);

    /* Builtin found */
    out = run("echo hello", &status);
    tap_is_str(out, "hello\n", "simple cmd: builtin echo");
    tap_is_int(status, 0, "simple cmd: echo exit status 0");
    free(out);

    /* Multiple assignments */
    out = run("A=1; B=2; echo $A $B", &status);
    tap_is_str(out, "1 2\n", "simple cmd: multiple assignments");
    free(out);
}

static void test_pipelines(void)
{
    int status;
    char *out;

    /* Exit status is last command */
    out = run("true | false; echo $?", &status);
    tap_is_str(out, "1\n", "pipeline: exit status is last command");
    free(out);

    /* Pipeline negation */
    out = run("! true; echo $?", &status);
    tap_is_str(out, "1\n", "pipeline: ! true gives 1");
    free(out);

    out = run("! false; echo $?", &status);
    tap_is_str(out, "0\n", "pipeline: ! false gives 0");
    free(out);

    /* Negation of pipeline */
    out = run("! true | true; echo $?", &status);
    tap_is_str(out, "1\n", "pipeline: ! negates pipeline exit status");
    free(out);
}

static void test_lists(void)
{
    int status;
    char *out;

    /* Sequential */
    out = run("echo a; echo b", &status);
    tap_is_str(out, "a\nb\n", "list: sequential");
    tap_is_int(status, 0, "list: sequential exit status 0");
    free(out);

    /* Sequential exit status is last command */
    out = run("true; false; echo $?", &status);
    tap_is_str(out, "1\n", "list: sequential exit status is last");
    free(out);

    /* && runs on success */
    out = run("true && echo yes", &status);
    tap_is_str(out, "yes\n", "list: && runs on success");
    free(out);

    /* && skips on failure */
    out = run("false && echo no", &status);
    tap_is_str(out, "", "list: && skips on failure");
    free(out);

    /* || runs on failure */
    out = run("false || echo yes", &status);
    tap_is_str(out, "yes\n", "list: || runs on failure");
    free(out);

    /* || skips on success */
    out = run("true || echo no", &status);
    tap_is_str(out, "", "list: || skips on success");
    free(out);

    /* Chain: false && a || b */
    out = run("false && echo a || echo b", &status);
    tap_is_str(out, "b\n", "list: false && a || b gives b");
    free(out);

    /* Chain: true && false || c */
    out = run("true && false || echo c", &status);
    tap_is_str(out, "c\n", "list: true && false || c gives c");
    free(out);

    /* Background sets status 0 */
    out = run("true & wait; echo $?", &status);
    tap_is_str(out, "0\n", "list: background wait exit status");
    tap_is_int(status, 0, "list: background overall status 0");
    free(out);
}

static void test_subshell(void)
{
    int status;
    char *out;

    /* Subshell isolation */
    out = run("X=outer; (X=inner); echo $X", &status);
    tap_is_str(out, "outer\n", "subshell: variable isolation");
    free(out);

    /* Subshell exit status */
    out = run("(exit 42); echo $?", &status);
    tap_is_str(out, "42\n", "subshell: exit status propagated");
    free(out);
}

static void test_brace_group(void)
{
    int status;
    char *out;

    /* Brace group runs in current env */
    out = run("{ X=inside; }; echo $X", &status);
    tap_is_str(out, "inside\n", "brace group: current env");
    tap_is_int(status, 0, "brace group: exit status 0");
    free(out);
}

static void test_for_loop(void)
{
    int status;
    char *out;

    /* Basic iteration */
    out = run("for x in a b c; do echo $x; done", &status);
    tap_is_str(out, "a\nb\nc\n", "for: iteration");
    tap_is_int(status, 0, "for: exit status 0");
    free(out);

    /* Empty list */
    out = run("for x in; do echo no; done; echo done", &status);
    tap_is_str(out, "done\n", "for: empty list");
    free(out);

    /* for without in iterates $@ */
    out = run("f() { for x; do echo $x; done; }; f a b", &status);
    tap_is_str(out, "a\nb\n", "for: without in uses $@");
    free(out);
}

static void test_case(void)
{
    int status;
    char *out;

    /* Exact match */
    out = run("case hello in hello) echo yes;; esac", &status);
    tap_is_str(out, "yes\n", "case: exact match");
    tap_is_int(status, 0, "case: exact match exit status 0");
    free(out);

    /* Glob match */
    out = run("case hello in hel*) echo yes;; esac", &status);
    tap_is_str(out, "yes\n", "case: glob match");
    free(out);

    /* No match: case with no matching pattern */
    out = run("case hello in bye) echo no;; esac", &status);
    tap_is_int(status, 1, "case: no match status 1");
    free(out);

    /* Alternation */
    out = run("case b in a|b|c) echo yes;; esac", &status);
    tap_is_str(out, "yes\n", "case: alternation");
    free(out);

    /* Multiple patterns */
    out = run("case dog in cat) echo c;; dog) echo d;; esac", &status);
    tap_is_str(out, "d\n", "case: second pattern matches");
    free(out);
}

static void test_if(void)
{
    int status;
    char *out;

    /* if true */
    out = run("if true; then echo yes; fi", &status);
    tap_is_str(out, "yes\n", "if: true runs body");
    free(out);

    /* if false */
    out = run("if false; then echo no; fi", &status);
    tap_is_str(out, "", "if: false skips body");
    free(out);

    /* if-else */
    out = run("if false; then echo no; else echo yes; fi", &status);
    tap_is_str(out, "yes\n", "if: else branch");
    free(out);

    /* if-elif */
    out = run("if false; then echo a; elif true; then echo b; fi", &status);
    tap_is_str(out, "b\n", "if: elif branch");
    free(out);

    /* if with no branch taken: status reflects condition result */
    out = run("if false; then echo no; fi", &status);
    tap_is_int(status, 1, "if: status reflects condition when no branch taken");
    free(out);
}

static void test_while(void)
{
    int status;
    char *out;

    /* while loop */
    out = run("i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done", &status);
    tap_is_str(out, "0\n1\n2\n", "while: counts to 2");
    tap_is_int(status, 1, "while: status reflects last condition check");
    free(out);

    /* while never entered */
    out = run("while false; do echo no; done", &status);
    tap_is_int(status, 1, "while: never entered reflects condition status");
    free(out);
}

static void test_until(void)
{
    int status;
    char *out;

    /* until loop */
    out = run("i=0; until [ $i -ge 3 ]; do echo $i; i=$((i+1)); done", &status);
    tap_is_str(out, "0\n1\n2\n", "until: counts to 2");
    tap_is_int(status, 0, "until: exit status 0");
    free(out);

    /* until never entered */
    out = run("until true; do echo no; done; echo $?", &status);
    tap_is_str(out, "0\n", "until: never entered exit status 0");
    free(out);
}

static void test_break_continue(void)
{
    int status;
    char *out;

    /* break */
    out = run("for x in a b c; do echo $x; break; done", &status);
    tap_is_str(out, "a\n", "break: stops after first");
    free(out);

    /* continue */
    out = run("for x in a b c; do if [ $x = b ]; then continue; fi; echo $x; done", &status);
    tap_is_str(out, "a\nc\n", "continue: skips b");
    free(out);

    /* break N */
    out = run("for x in a b; do for y in 1 2; do echo $x$y; break 2; done; done", &status);
    tap_is_str(out, "a1\n", "break 2: breaks out of both loops");
    free(out);
}

static void test_functions_basic(void)
{
    int status;
    char *out;

    /* Basic function */
    out = run("f() { echo hello; }; f", &status);
    tap_is_str(out, "hello\n", "func: basic call");
    tap_is_int(status, 0, "func: basic exit status 0");
    free(out);

    /* Function with params */
    out = run("f() { echo $1 $2; }; f x y", &status);
    tap_is_str(out, "x y\n", "func: params");
    free(out);

    /* Positional params restored after function call */
    out = run("set -- outer; f() { echo $1; }; f inner; echo $1", &status);
    tap_is_str(out, "inner\nouter\n", "func: params restored after call");
    free(out);

    /* $# in function */
    out = run("f() { echo $#; }; f a b c", &status);
    tap_is_str(out, "3\n", "func: $# counts arguments");
    free(out);

    /* $0 unchanged in function */
    out = run("f() { echo $0; }; f", &status);
    tap_ok(strstr(out, "opsh") != NULL, "func: $0 contains opsh");
    free(out);
}

static void test_functions_return(void)
{
    int status;
    char *out;

    /* return N */
    out = run("f() { return 42; }; f; echo $?", &status);
    tap_is_str(out, "42\n", "func: return 42");
    free(out);

    /* return without arg returns 0 */
    out = run("f() { false; return; }; f; echo $?", &status);
    tap_is_str(out, "0\n", "func: return without arg returns 0");
    free(out);

    /* Exit status is last command in function */
    out = run("f() { true; }; f; echo $?", &status);
    tap_is_str(out, "0\n", "func: exit status is last command");
    free(out);

    /* Recursive function (assign $1 to named var for arithmetic) */
    out =
        run("f() { n=$1; if [ $n -le 0 ]; then echo done; return; fi; f $((n-1)); }; f 3", &status);
    tap_is_str(out, "done\n", "func: recursive call");
    tap_is_int(status, 0, "func: recursive exit status 0");
    free(out);
}

/* Total: 61 assertions */

int main(void)
{
    tap_plan(61);

    test_simple_commands();
    test_pipelines();
    test_lists();
    test_subshell();
    test_brace_group();
    test_for_loop();
    test_case();
    test_if();
    test_while();
    test_until();
    test_break_continue();
    test_functions_basic();
    test_functions_return();

    return tap_done();
}
