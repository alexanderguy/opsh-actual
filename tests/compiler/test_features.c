#include "../tap.h"
#include "compiler/compiler.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "parser/parser.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Feature integration tests: compile and run shell code, capture output.
 * Tests shell features added during POSIX compliance work.
 */

static char *run(const char *source, int *status_out)
{
    parser_t p;
    parser_init(&p, source, "test");
    sh_list_t *ast = parser_parse(&p);
    if (ast == NULL || parser_error_count(&p) > 0) {
        sh_list_free(ast);
        parser_destroy(&p);
        if (status_out)
            *status_out = 2;
        return xstrdup("");
    }

    bytecode_image_t *img = compile(ast, "test");
    sh_list_free(ast);
    parser_destroy(&p);
    if (img == NULL) {
        if (status_out)
            *status_out = 2;
        return xstrdup("");
    }

    int pipefd[2];
    pipe(pipefd);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    vm_t vm;
    vm_init(&vm, img);
    int status = vm_run(&vm);
    if (status_out)
        *status_out = status;

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    strbuf_t captured;
    strbuf_init(&captured);
    {
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            strbuf_append_bytes(&captured, buf, (size_t)n);
        }
    }
    close(pipefd[0]);

    vm_destroy(&vm);
    image_free(img);
    return strbuf_detach(&captured);
}

/* ---- Here-documents ---- */

static void test_heredoc(void)
{
    char *out;

    out = run("cat <<EOF\nhello world\nEOF\n", NULL);
    tap_is_str(out, "hello world\n", "heredoc: basic");
    free(out);

    out = run("cat <<-EOF\n\thello\n\tEOF\n", NULL);
    tap_is_str(out, "hello\n", "heredoc: tab strip");
    free(out);

    out = run("cat <<'EOF'\n$HOME\nEOF\n", NULL);
    tap_is_str(out, "$HOME\n", "heredoc: quoted delimiter suppresses expansion");
    free(out);

    out = run("X=world; cat <<EOF\nhello $X\nEOF\n", NULL);
    tap_is_str(out, "hello world\n", "heredoc: expansion");
    free(out);
}

/* ---- Pattern expansion ---- */

static void test_patterns(void)
{
    char *out;

    out = run("x=hello.world.txt; echo ${x#*.}", NULL);
    tap_is_str(out, "world.txt\n", "pattern: # shortest prefix");
    free(out);

    out = run("x=hello.world.txt; echo ${x##*.}", NULL);
    tap_is_str(out, "txt\n", "pattern: ## longest prefix");
    free(out);

    out = run("x=hello.world.txt; echo ${x%.txt}", NULL);
    tap_is_str(out, "hello.world\n", "pattern: % shortest suffix");
    free(out);

    out = run("x=hello.world.txt; echo ${x%%.*}", NULL);
    tap_is_str(out, "hello\n", "pattern: %% longest suffix");
    free(out);

    out = run("x=hello; echo ${x/ell/ELL}", NULL);
    tap_is_str(out, "hELLo\n", "pattern: / substitute first");
    free(out);

    out = run("x=hello; echo ${x//l/L}", NULL);
    tap_is_str(out, "heLLo\n", "pattern: // substitute all");
    free(out);

    out = run("x=hello; echo ${x/#hel/HEL}", NULL);
    tap_is_str(out, "HELlo\n", "pattern: /# anchored head");
    free(out);

    out = run("x=hello; echo ${x/%llo/LLO}", NULL);
    tap_is_str(out, "heLLO\n", "pattern: /% anchored tail");
    free(out);
}

/* ---- Tilde expansion ---- */

static void test_tilde(void)
{
    char *out;
    int status;

    out = run("echo ~", &status);
    tap_ok(strlen(out) > 2, "tilde: ~ expands to something");
    tap_ok(out[0] == '/', "tilde: starts with /");
    free(out);

    out = run("echo ~/bin", NULL);
    tap_ok(strstr(out, "/bin") != NULL, "tilde: ~/bin has /bin");
    free(out);

    out = run("echo \"~\"", NULL);
    tap_is_str(out, "~\n", "tilde: quoted ~ is literal");
    free(out);

    out = run("x=~/a:~/b; echo $x", NULL);
    tap_ok(strstr(out, ":") != NULL, "tilde: colon-separated expands");
    tap_ok(out[0] == '/', "tilde: first part expanded");
    free(out);
}

/* ---- Arrays ---- */

static void test_arrays(void)
{
    char *out;

    out = run("arr=(hello world foo); echo ${arr[0]} ${arr[1]} ${arr[2]}", NULL);
    tap_is_str(out, "hello world foo\n", "array: basic access");
    free(out);

    out = run("arr=(a b c d); echo ${#arr[@]}", NULL);
    tap_is_str(out, "4\n", "array: length");
    free(out);

    out = run("arr=(a b c); arr[1]=X; echo ${arr[1]}", NULL);
    tap_is_str(out, "X\n", "array: set element");
    free(out);

    out = run("arr=(); echo ${#arr[@]}", NULL);
    tap_is_str(out, "0\n", "array: empty");
    free(out);
}

/* ---- "$@" expansion ---- */

static void test_expand_at(void)
{
    char *out;

    /* Test via function call since run() has no script args */
    out = run("f() { for x in \"$@\"; do echo \"[$x]\"; done; }; f \"hello world\" foo", NULL);
    tap_is_str(out, "[hello world]\n[foo]\n", "$@: preserves word boundaries");
    free(out);

    out = run("f() { echo $#; }; f a b c", NULL);
    tap_is_str(out, "3\n", "$#: count");
    free(out);

    out = run("f() { for x; do echo $x; done; }; f a b c", NULL);
    tap_is_str(out, "a\nb\nc\n", "for x; iterates $@");
    free(out);
}

/* ---- Subshells ---- */

static void test_subshell(void)
{
    char *out;
    int status;

    out = run("(echo sub)", NULL);
    tap_is_str(out, "sub\n", "subshell: basic output");
    free(out);

    out = run("(x=inner); echo $x", NULL);
    tap_is_str(out, "\n", "subshell: variable isolation");
    free(out);

    out = run("(exit 42); echo $?", &status);
    tap_is_str(out, "42\n", "subshell: exit status");
    free(out);
}

/* ---- set options ---- */

static void test_set_options(void)
{
    char *out;
    int status;

    out = run("set -- a b c; echo $1 $2 $3", NULL);
    tap_is_str(out, "a b c\n", "set --: positional params");
    free(out);

    out = run("set -e; true; echo alive", &status);
    tap_is_str(out, "alive\n", "set -e: passes on success");
    free(out);

    out = run("set -e; false; echo NO", &status);
    tap_ok(status != 0, "set -e: exits on failure");
    free(out);

    out = run("set -e; if false; then echo Y; else echo N; fi; echo after", NULL);
    tap_is_str(out, "N\nafter\n", "set -e: if condition suppressed");
    free(out);

    out = run("set -e; false || echo recovered; echo after", NULL);
    tap_is_str(out, "recovered\nafter\n", "set -e: || suppressed");
    free(out);
}

/* ---- break/continue ---- */

static void test_break_continue(void)
{
    char *out;

    out = run("for x in 1 2 3; do if [ \"$x\" = \"2\" ]; then break; fi; echo $x; done", NULL);
    tap_is_str(out, "1\n", "break: stops loop");
    free(out);

    out = run("for x in 1 2 3; do if [ \"$x\" = \"2\" ]; then continue; fi; echo $x; done", NULL);
    tap_is_str(out, "1\n3\n", "continue: skips iteration");
    free(out);

    out = run("while true; do echo once; break; done", NULL);
    tap_is_str(out, "once\n", "break: in while loop");
    free(out);
}

/* ---- eval / source / command ---- */

static void test_eval_source_command(void)
{
    char *out;

    out = run("eval 'echo hello'", NULL);
    tap_is_str(out, "hello\n", "eval: basic");
    free(out);

    out = run("eval 'x=5'; echo $x", NULL);
    tap_is_str(out, "5\n", "eval: env sharing");
    free(out);

    out = run("command echo bypass", NULL);
    tap_is_str(out, "bypass\n", "command: bypass functions");
    free(out);

    /* source: write temp file, source it, check variable */
    {
        FILE *f = fopen("tmp/test_source_feat.opsh", "w");
        if (f) {
            fprintf(f, "SOURCED_VAR=yes\n");
            fclose(f);
        }
        out = run(". tmp/test_source_feat.opsh; echo $SOURCED_VAR", NULL);
        tap_is_str(out, "yes\n", "source: env sharing");
        free(out);
        unlink("tmp/test_source_feat.opsh");
    }
}

/* ---- [[ ]] expressions ---- */

static void test_bracket(void)
{
    char *out;

    out = run("[[ -f /etc/hosts ]] && echo yes || echo no", NULL);
    tap_is_str(out, "yes\n", "[[: file test");
    free(out);

    out = run("[[ \"a\" == \"a\" ]] && echo match", NULL);
    tap_is_str(out, "match\n", "[[: string eq");
    free(out);

    out = run("[[ \"hello.txt\" == *.txt ]] && echo glob", NULL);
    tap_is_str(out, "glob\n", "[[: glob match");
    free(out);

    out = run("[[ ! -f /nonexistent ]] && echo yes", NULL);
    tap_is_str(out, "yes\n", "[[: negation");
    free(out);

    out = run("[[ \"a\" != \"b\" ]] && echo diff", NULL);
    tap_is_str(out, "diff\n", "[[: !=");
    free(out);

    out = run("[[ -z \"\" ]] && echo empty", NULL);
    tap_is_str(out, "empty\n", "[[: -z empty string");
    free(out);
}

/* ---- Compound command redirections ---- */

static void test_compound_redirs(void)
{
    char *out;

    /* if with redirect: output goes to file, not stdout */
    out = run("if true; then echo inside; fi > tmp/test_redir.txt; cat tmp/test_redir.txt", NULL);
    tap_is_str(out, "inside\n", "compound redir: if > file");
    free(out);
    unlink("tmp/test_redir.txt");

    /* brace group with redirect */
    out = run("{ echo group; } > tmp/test_redir2.txt; cat tmp/test_redir2.txt", NULL);
    tap_is_str(out, "group\n", "compound redir: { } > file");
    free(out);
    unlink("tmp/test_redir2.txt");

    /* redirect restore: output after compound goes to stdout */
    out = run("{ echo inside; } > /dev/null; echo outside", NULL);
    tap_is_str(out, "outside\n", "compound redir: restore after");
    free(out);
}

/* ---- Temporary assignment scoping ---- */

static void test_temp_assign(void)
{
    char *out;

    out = run("x=before; x=during echo test; echo $x", NULL);
    tap_is_str(out, "test\nbefore\n", "temp assign: reverts");
    free(out);
}

/* ---- getopts ---- */

static void test_getopts(void)
{
    char *out;

    out = run("while getopts 'ab:c' opt -a -b val -c; do echo \"$opt=$OPTARG\"; done", NULL);
    tap_is_str(out, "a=\nb=val\nc=\n", "getopts: basic options");
    free(out);

    out = run("while getopts 'abc' opt -abc; do echo $opt; done", NULL);
    tap_is_str(out, "a\nb\nc\n", "getopts: bundled options");
    free(out);
}

/* ---- Additional builtins ---- */

static void test_builtins_extra(void)
{
    char *out;
    int status;

    /* umask */
    out = run("umask 077; umask", NULL);
    tap_is_str(out, "0077\n", "umask: set and get");
    free(out);

    /* kill -l */
    out = run("kill -l", NULL);
    tap_ok(strstr(out, "HUP") != NULL, "kill -l: lists signals");
    free(out);

    /* wait with no children */
    out = run("wait", &status);
    tap_is_int(status, 0, "wait: no children returns 0");
    free(out);

    /* set -u */
    out = run("set -u; echo ${UNSET_XYZ:-default}", &status);
    tap_is_str(out, "default\n", "set -u: default expansion works");
    free(out);

    /* colon builtin */
    out = run(": ; echo $?", NULL);
    tap_is_str(out, "0\n", "colon: returns 0");
    free(out);
}

int main(void)
{
    tap_plan(57);

    test_heredoc();
    test_patterns();
    test_tilde();
    test_arrays();
    test_expand_at();
    test_subshell();
    test_set_options();
    test_break_continue();
    test_eval_source_command();
    test_bracket();
    test_compound_redirs();
    test_temp_assign();
    test_getopts();
    test_builtins_extra();

    tap_done();
    return 0;
}
