#include "../tap.h"
#include "lint/lint.h"
#include "parser/parser.h"

#include <stdlib.h>
#include <string.h>

static lint_diag_t *run_lint(const char *source)
{
    parser_t p;
    parser_init(&p, source, "<test>");
    sh_list_t *ast = parser_parse(&p);
    lint_diag_t *d = lint_check(ast, "<test>");
    sh_list_free(ast);
    parser_destroy(&p);
    return d;
}

static int diag_count(const lint_diag_t *d)
{
    int n = 0;
    while (d != NULL) {
        n++;
        d = d->next;
    }
    return n;
}

static const lint_diag_t *find_code(const lint_diag_t *d, int code)
{
    while (d != NULL) {
        if (d->code == code) {
            return d;
        }
        d = d->next;
    }
    return NULL;
}

int main(void)
{
    tap_plan(19);

    /* SC2086: unquoted variable */
    {
        lint_diag_t *d = run_lint("echo $foo");
        tap_ok(find_code(d, 2086) != NULL, "SC2086: unquoted $foo in argument");
        lint_diag_free(d);
    }

    /* SC2086: quoted variable is clean */
    {
        lint_diag_t *d = run_lint("echo \"$foo\"");
        tap_ok(find_code(d, 2086) == NULL, "SC2086: quoted $foo is clean");
        lint_diag_free(d);
    }

    /* SC2086: command name (index 0) should not trigger */
    {
        lint_diag_t *d = run_lint("$cmd arg");
        tap_ok(find_code(d, 2086) == NULL, "SC2086: unquoted command name is clean");
        lint_diag_free(d);
    }

    /* SC2086: $? should not trigger (no word splitting risk) */
    {
        lint_diag_t *d = run_lint("echo $?");
        tap_ok(find_code(d, 2086) == NULL, "SC2086: $? is clean");
        lint_diag_free(d);
    }

    /* SC2086: multiple unquoted vars */
    {
        lint_diag_t *d = run_lint("echo $foo $bar");
        tap_is_int(diag_count(d), 2, "SC2086: two unquoted vars produce two diagnostics");
        lint_diag_free(d);
    }

    /* SC2086: inside [[ ]] should not trigger */
    {
        lint_diag_t *d = run_lint("[[ $foo == bar ]]");
        tap_ok(find_code(d, 2086) == NULL, "SC2086: inside [[ ]] is clean");
        lint_diag_free(d);
    }

    /* SC2048: unquoted $@ */
    {
        lint_diag_t *d = run_lint("echo $@");
        const lint_diag_t *found = find_code(d, 2048);
        tap_ok(found != NULL, "SC2048: unquoted $@ fires");
        lint_diag_free(d);
    }

    /* SC2048: unquoted $* */
    {
        lint_diag_t *d = run_lint("echo $*");
        tap_ok(find_code(d, 2048) != NULL, "SC2048: unquoted $* fires");
        lint_diag_free(d);
    }

    /* SC2048: quoted $@ is clean */
    {
        lint_diag_t *d = run_lint("echo \"$@\"");
        tap_ok(find_code(d, 2048) == NULL, "SC2048: quoted $@ is clean");
        lint_diag_free(d);
    }

    /* SC2164: cd without guard */
    {
        lint_diag_t *d = run_lint("cd /tmp");
        tap_ok(find_code(d, 2164) != NULL, "SC2164: bare cd fires");
        lint_diag_free(d);
    }

    /* SC2164: cd || exit is clean */
    {
        lint_diag_t *d = run_lint("cd /tmp || exit");
        tap_ok(find_code(d, 2164) == NULL, "SC2164: cd || exit is clean");
        lint_diag_free(d);
    }

    /* SC2164: cd || return is clean */
    {
        lint_diag_t *d = run_lint("cd /tmp || return");
        tap_ok(find_code(d, 2164) == NULL, "SC2164: cd || return is clean");
        lint_diag_free(d);
    }

    /* SC2164: cd && echo is NOT guarded (wrong connector) */
    {
        lint_diag_t *d = run_lint("cd /tmp && echo ok");
        tap_ok(find_code(d, 2164) != NULL, "SC2164: cd && is not a guard");
        lint_diag_free(d);
    }

    /* Clean script */
    {
        lint_diag_t *d = run_lint("echo \"hello world\"");
        tap_is_int(diag_count(d), 0, "clean script: no diagnostics");
        lint_diag_free(d);
    }

    /* Inside compound command */
    {
        lint_diag_t *d = run_lint("if true; then\necho $foo\nfi");
        tap_ok(find_code(d, 2086) != NULL, "SC2086: inside if body");
        lint_diag_free(d);
    }

    /* Inside function */
    {
        lint_diag_t *d = run_lint("f() {\necho $foo\n}");
        tap_ok(find_code(d, 2086) != NULL, "SC2086: inside function body");
        lint_diag_free(d);
    }

    /* GCC output format */
    {
        lint_diag_t *d = run_lint("echo $foo");
        strbuf_t out;
        strbuf_init(&out);
        lint_format_diags(&out, d, LINT_FMT_GCC);
        tap_ok(strstr(out.contents, "[SC2086]") != NULL, "gcc format: contains [SC2086]");
        tap_ok(strstr(out.contents, "<test>:1:0:") != NULL,
               "gcc format: contains filename:line:col");
        strbuf_destroy(&out);
        lint_diag_free(d);
    }

    /* JSON1 output format */
    {
        lint_diag_t *d = run_lint("echo $foo");
        strbuf_t out;
        strbuf_init(&out);
        lint_format_diags(&out, d, LINT_FMT_JSON1);
        tap_ok(strstr(out.contents, "\"code\":2086") != NULL, "json1 format: contains code");
        strbuf_destroy(&out);
        lint_diag_free(d);
    }

    return tap_done();
}
