#include "helpers.h"

/*
 * POSIX 2.6 Word Expansion compliance tests.
 *
 * Sections covered:
 *   2.6.1 Tilde Expansion
 *   2.6.2 Parameter Expansion
 *   2.6.3 Command Substitution
 *   2.6.4 Arithmetic Expansion
 *   2.6.5 Field Splitting
 *   2.6.6 Pathname Expansion
 *   2.6.7 Quote Removal
 */

/* 2.6.1 Tilde Expansion — 4 assertions */

static void test_tilde_expansion(void)
{
    char *out;

    /* Tilde expands to the process HOME at expansion time.
     * Setting HOME in the script doesn't affect already-compiled tilde. */
    {
        char expected[4096];
        const char *home = getenv("HOME");
        snprintf(expected, sizeof(expected), "%s\n", home ? home : "");
        out = run("echo ~", NULL);
        tap_is_str(out, expected, "tilde: ~ expands to HOME");
        free(out);

        snprintf(expected, sizeof(expected), "%s/foo\n", home ? home : "");
        out = run("echo ~/foo", NULL);
        tap_is_str(out, expected, "tilde: ~/path appends to HOME");
        free(out);
    }

    out = run("echo '~'", NULL);
    tap_is_str(out, "~\n", "tilde: single-quoted ~ is literal");
    free(out);

    out = run("echo \"~\"", NULL);
    tap_is_str(out, "~\n", "tilde: double-quoted ~ is literal");
    free(out);
}

/* 2.6.2 Parameter Expansion — 21 assertions */

static void test_parameter_expansion(void)
{
    char *out;
    int status;

    /* ${X:-word} */
    out = run("echo ${MISS:-fallback}", NULL);
    tap_is_str(out, "fallback\n", "param: ${X:-word} uses word when unset");
    free(out);

    out = run("E=; echo ${E:-fallback}", NULL);
    tap_is_str(out, "fallback\n", "param: ${X:-word} uses word when null");
    free(out);

    out = run("V=hi; echo ${V:-fallback}", NULL);
    tap_is_str(out, "hi\n", "param: ${X:-word} uses value when set");
    free(out);

    /* ${X-word} (no colon) */
    out = run("E=; echo ${E-fallback}", NULL);
    tap_is_str(out, "\n", "param: ${X-word} uses value when null");
    free(out);

    out = run("echo ${MISS-fallback}", NULL);
    tap_is_str(out, "fallback\n", "param: ${X-word} uses word when unset");
    free(out);

    /* ${X:=word} */
    out = run("echo ${N:=assigned}; echo $N", NULL);
    tap_is_str(out, "assigned\nassigned\n", "param: ${X:=word} assigns and uses word");
    free(out);

    /* ${X:+word} */
    out = run("V=yes; echo ${V:+alt}", NULL);
    tap_is_str(out, "alt\n", "param: ${X:+word} alternate when set");
    free(out);

    out = run("echo ${MISS:+alt}", NULL);
    tap_is_str(out, "\n", "param: ${X:+word} empty when unset");
    free(out);

    /* ${X+word} (no colon) */
    out = run("E=; echo ${E+alt}", NULL);
    tap_is_str(out, "alt\n", "param: ${X+word} alternate even when null");
    free(out);

    /* ${X:?word} */
    out = run("echo ${MISS:?oops}", &status);
    tap_ok(status != 0, "param: ${X:?word} errors when unset");
    free(out);

    /* ${#X} */
    out = run("X=hello; echo ${#X}", NULL);
    tap_is_str(out, "5\n", "param: ${#X} string length");
    free(out);

    /* ${X#pat} ${X##pat} */
    out = run("X=abcabc; echo ${X#a*c}", NULL);
    tap_is_str(out, "abc\n", "param: ${X#pat} shortest prefix removal");
    free(out);

    out = run("X=abcabc; echo ${X##a*c}", NULL);
    tap_is_str(out, "\n", "param: ${X##pat} longest prefix removal");
    free(out);

    /* ${X%pat} ${X%%pat} */
    out = run("X=abcabc; echo ${X%a*c}", NULL);
    tap_is_str(out, "abc\n", "param: ${X%pat} shortest suffix removal");
    free(out);

    out = run("X=abcabc; echo ${X%%a*c}", NULL);
    tap_is_str(out, "\n", "param: ${X%%pat} longest suffix removal");
    free(out);

    /* ${X/pat/rep} ${X//pat/rep} */
    out = run("X=aabaa; echo ${X/a/x}", NULL);
    tap_is_str(out, "xabaa\n", "param: ${X/pat/rep} first substitution");
    free(out);

    out = run("X=aabaa; echo ${X//a/x}", NULL);
    tap_is_str(out, "xxbxx\n", "param: ${X//pat/rep} global substitution");
    free(out);

    /* ${X/#pat/rep} ${X/%pat/rep} */
    out = run("X=hello; echo ${X/#hel/wor}", NULL);
    tap_is_str(out, "worlo\n", "param: ${X/#pat/rep} anchor prefix");
    free(out);

    out = run("X=hello; echo ${X/%llo/p}", NULL);
    tap_is_str(out, "hep\n", "param: ${X/%pat/rep} anchor suffix");
    free(out);

    /* nested expansion — not yet supported */
    tap_skip(1, "nested ${} in expansion word not implemented");

    /* expansion in double quotes preserves spaces */
    out = run("X=\"a  b\"; echo \"${X}\"", NULL);
    tap_is_str(out, "a  b\n", "param: double-quoted expansion no split");
    free(out);
}

/* 2.6.3 Command Substitution — 7 assertions */

static void test_command_substitution(void)
{
    char *out;
    int status;

    out = run("echo $(echo hello)", NULL);
    tap_is_str(out, "hello\n", "cmdsub: basic");
    free(out);

    out = run("X=$(printf 'hi\\n\\n\\n'); echo \"[$X]\"", NULL);
    tap_is_str(out, "[hi]\n", "cmdsub: trailing newlines stripped");
    free(out);

    out = run("X=$(printf 'a\\nb'); echo \"$X\"", NULL);
    tap_is_str(out, "a\nb\n", "cmdsub: embedded newlines preserved");
    free(out);

    out = run("echo $(echo $(echo deep))", NULL);
    tap_is_str(out, "deep\n", "cmdsub: nested");
    free(out);

    out = run("$(exit 42); echo $?", &status);
    tap_is_str(out, "42\n", "cmdsub: exit status propagated");
    free(out);

    out = run("echo \"$(echo \"a b\")\"", NULL);
    tap_is_str(out, "a b\n", "cmdsub: double-quoted no split");
    free(out);

    out = run("echo \"$(echo \"a  b  c\")\"", NULL);
    tap_is_str(out, "a  b  c\n", "cmdsub: preserves internal spacing");
    free(out);
}

/* 2.6.4 Arithmetic Expansion — 18 assertions */

static void test_arithmetic_expansion(void)
{
    char *out;

    out = run("echo $((2+3))", NULL);
    tap_is_str(out, "5\n", "arith: addition");
    free(out);

    out = run("echo $((10-3))", NULL);
    tap_is_str(out, "7\n", "arith: subtraction");
    free(out);

    out = run("echo $((4*5))", NULL);
    tap_is_str(out, "20\n", "arith: multiplication");
    free(out);

    out = run("echo $((10/3))", NULL);
    tap_is_str(out, "3\n", "arith: division truncates");
    free(out);

    out = run("echo $((10%3))", NULL);
    tap_is_str(out, "1\n", "arith: modulo");
    free(out);

    out = run("echo $((3==3))", NULL);
    tap_is_str(out, "1\n", "arith: equality true");
    free(out);

    out = run("echo $((3!=4))", NULL);
    tap_is_str(out, "1\n", "arith: inequality true");
    free(out);

    out = run("echo $((3<4))", NULL);
    tap_is_str(out, "1\n", "arith: less than");
    free(out);

    out = run("echo $((4>3))", NULL);
    tap_is_str(out, "1\n", "arith: greater than");
    free(out);

    out = run("echo $((1&&1))", NULL);
    tap_is_str(out, "1\n", "arith: logical and");
    free(out);

    out = run("echo $((1||0))", NULL);
    tap_is_str(out, "1\n", "arith: logical or");
    free(out);

    out = run("echo $((!0))", NULL);
    tap_is_str(out, "1\n", "arith: logical not");
    free(out);

    out = run("echo $((6&3))", NULL);
    tap_is_str(out, "2\n", "arith: bitwise and");
    free(out);

    out = run("echo $((6|3))", NULL);
    tap_is_str(out, "7\n", "arith: bitwise or");
    free(out);

    out = run("echo $((6^3))", NULL);
    tap_is_str(out, "5\n", "arith: bitwise xor");
    free(out);

    out = run("x=10; echo $((x+1))", NULL);
    tap_is_str(out, "11\n", "arith: variable reference");
    free(out);

    out = run("echo $((x=5)); echo $x", NULL);
    tap_is_str(out, "5\n5\n", "arith: assignment");
    free(out);

    out = run("echo $((1 ? 10 : 20))", NULL);
    tap_is_str(out, "10\n", "arith: ternary");
    free(out);
}

/* 2.6.5 Field Splitting — 7 assertions */

static void test_field_splitting(void)
{
    char *out;

    out = run("X='a b c'; echo $X", NULL);
    tap_is_str(out, "a b c\n", "split: default IFS splits spaces");
    free(out);

    out = run("X='  a  b  '; echo $X", NULL);
    tap_is_str(out, "a b\n", "split: leading/trailing whitespace stripped");
    free(out);

    out = run("IFS=:; X='a:b:c'; echo $X", NULL);
    tap_is_str(out, "a b c\n", "split: custom IFS");
    free(out);

    out = run("IFS=''; X='a b'; echo $X", NULL);
    tap_is_str(out, "a b\n", "split: empty IFS disables splitting");
    free(out);

    out = run("X='a  b  c'; echo \"$X\"", NULL);
    tap_is_str(out, "a  b  c\n", "split: quoted prevents split");
    free(out);

    out = run("X=; echo $X end", NULL);
    tap_is_str(out, "end\n", "split: empty var produces no fields");
    free(out);

    out = run("echo 'a   b   c'", NULL);
    tap_is_str(out, "a   b   c\n", "split: literal text not split");
    free(out);
}

/* 2.6.6 Pathname Expansion — 4 assertions */

static void test_pathname_expansion(void)
{
    char *out;

    out = run("echo /nonexistent_xyz/*.qqq", NULL);
    tap_is_str(out, "/nonexistent_xyz/*.qqq\n", "glob: no match returns literal");
    free(out);

    out = run("echo \"*\"", NULL);
    tap_is_str(out, "*\n", "glob: quoted * is literal");
    free(out);

    ensure_tmp();
    {
        FILE *f;
        f = fopen("tmp/glob_test_a.txt", "w");
        if (f) {
            fprintf(f, "a\n");
            fclose(f);
        }
        f = fopen("tmp/glob_test_b.txt", "w");
        if (f) {
            fprintf(f, "b\n");
            fclose(f);
        }

        out = run("echo tmp/glob_test_*.txt", NULL);
        tap_ok(strstr(out, "glob_test_a.txt") != NULL, "glob: * matches file a");
        tap_ok(strstr(out, "glob_test_b.txt") != NULL, "glob: * matches file b");
        free(out);

        unlink("tmp/glob_test_a.txt");
        unlink("tmp/glob_test_b.txt");
    }
}

/* 2.6.7 Quote Removal — 3 assertions */

static void test_quote_removal(void)
{
    char *out;

    out = run("echo 'hello'", NULL);
    tap_is_str(out, "hello\n", "quote removal: single quotes removed");
    free(out);

    out = run("echo \"hello\"", NULL);
    tap_is_str(out, "hello\n", "quote removal: double quotes removed");
    free(out);

    out = run("echo hel\\lo", NULL);
    tap_is_str(out, "hello\n", "quote removal: backslash removed");
    free(out);
}

/*
 * Assertion tally:
 *   tilde:       4 tap_is_str
 *   parameter:  20 tap_is_str + 1 tap_ok = 21
 *   cmdsub:      7 tap_is_str
 *   arith:      18 tap_is_str
 *   split:       7 tap_is_str
 *   glob:        2 tap_is_str + 2 tap_ok = 4
 *   quote:       3 tap_is_str
 *   Total: 64
 */

int main(void)
{
    tap_plan(64);

    test_tilde_expansion();
    test_parameter_expansion();
    test_command_substitution();
    test_arithmetic_expansion();
    test_field_splitting();
    test_pathname_expansion();
    test_quote_removal();

    tap_done();
    return 0;
}
