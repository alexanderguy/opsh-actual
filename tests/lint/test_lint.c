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

#define LINT_HAS(src, code, desc)                                                                  \
    do {                                                                                           \
        lint_diag_t *_d = run_lint(src);                                                           \
        tap_ok(find_code(_d, code) != NULL, desc);                                                 \
        lint_diag_free(_d);                                                                        \
    } while (0)

#define LINT_CLEAN(src, code, desc)                                                                \
    do {                                                                                           \
        lint_diag_t *_d = run_lint(src);                                                           \
        tap_ok(find_code(_d, code) == NULL, desc);                                                 \
        lint_diag_free(_d);                                                                        \
    } while (0)

int main(void)
{
    tap_plan(57);

    /* === SC2086: unquoted variable === */
    LINT_HAS("echo $foo", 2086, "SC2086: unquoted var fires");
    LINT_CLEAN("echo \"$foo\"", 2086, "SC2086: quoted var clean");
    LINT_CLEAN("$cmd arg", 2086, "SC2086: command name clean");
    LINT_CLEAN("echo $?", 2086, "SC2086: $? clean");
    LINT_CLEAN("[[ $foo == bar ]]", 2086, "SC2086: inside [[ ]] clean");
    {
        /* 2x SC2086 + 2x SC2154 (referenced but not assigned) */
        lint_diag_t *d = run_lint("echo $foo $bar");
        int sc2086_count = 0;
        const lint_diag_t *p;
        for (p = d; p != NULL; p = p->next) {
            if (p->code == 2086)
                sc2086_count++;
        }
        tap_is_int(sc2086_count, 2, "SC2086: two unquoted vars = two SC2086 diags");
        lint_diag_free(d);
    }

    /* === SC2048: unquoted $@ / $* === */
    LINT_HAS("echo $@", 2048, "SC2048: unquoted $@ fires");
    LINT_HAS("echo $*", 2048, "SC2048: unquoted $* fires");
    LINT_CLEAN("echo \"$@\"", 2048, "SC2048: quoted $@ clean");

    /* === SC2046: unquoted command substitution === */
    LINT_HAS("echo $(cmd)", 2046, "SC2046: unquoted $(cmd) fires");
    LINT_CLEAN("echo \"$(cmd)\"", 2046, "SC2046: quoted $(cmd) clean");

    /* === SC2164: cd without guard === */
    LINT_HAS("cd /tmp", 2164, "SC2164: bare cd fires");
    LINT_CLEAN("cd /tmp || exit", 2164, "SC2164: cd || exit clean");
    LINT_CLEAN("cd /tmp || return", 2164, "SC2164: cd || return clean");
    LINT_HAS("cd /tmp && echo ok", 2164, "SC2164: cd && not a guard");

    /* === SC2162: read without -r === */
    LINT_HAS("read line", 2162, "SC2162: read without -r fires");
    LINT_CLEAN("read -r line", 2162, "SC2162: read -r clean");

    /* === SC2230: which === */
    LINT_HAS("which ls", 2230, "SC2230: which fires");

    /* === SC2002: useless cat === */
    LINT_HAS("cat file | grep foo", 2002, "SC2002: useless cat fires");
    LINT_CLEAN("cat file", 2002, "SC2002: cat alone clean");

    /* === SC2008: piping to echo === */
    LINT_HAS("cmd | echo", 2008, "SC2008: piping to echo fires");

    /* === SC2216: piping to rm === */
    LINT_HAS("cmd | rm", 2216, "SC2216: piping to rm fires");

    /* === SC2155: local/export var=$(cmd) === */
    LINT_HAS("f() {\nlocal x=$(cmd)\n}", 2155, "SC2155: local var=$(cmd) fires");
    LINT_HAS("export x=$(cmd)", 2155, "SC2155: export var=$(cmd) fires");

    /* === SC2005: useless echo === */
    LINT_HAS("echo $(cmd)", 2005, "SC2005: echo $(cmd) fires");

    /* === SC2091: $(cmd) as bare command === */
    LINT_HAS("$(generate_cmd)", 2091, "SC2091: $(cmd) as command fires");

    /* === SC2059: variable in printf format === */
    LINT_HAS("printf $fmt arg", 2059, "SC2059: printf $fmt fires");
    LINT_CLEAN("printf '%s' \"$var\"", 2059, "SC2059: printf literal format clean");

    /* === SC2163: export $var === */
    LINT_HAS("export $FOO", 2163, "SC2163: export $var fires");

    /* === SC2269: self-assignment === */
    LINT_HAS("x=$x", 2269, "SC2269: self-assignment fires");

    /* === SC2172: trap with signal numbers === */
    LINT_HAS("trap 'echo' 2", 2172, "SC2172: trap by number fires");
    LINT_CLEAN("trap 'echo' INT", 2172, "SC2172: trap by name clean");

    /* === SC2184: unquoted unset args === */
    LINT_HAS("unset $var", 2184, "SC2184: unquoted unset fires");

    /* === SC2123: PATH assignment === */
    LINT_HAS("PATH=/usr/bin cmd", 2123, "SC2123: PATH assignment fires");

    /* === SC2015: A && B || C === */
    LINT_HAS("true && echo ok || echo fail", 2015, "SC2015: && || pattern fires");

    /* === SC2043: single-value for loop === */
    LINT_HAS("for x in single; do echo $x; done", 2043, "SC2043: constant for fires");
    LINT_CLEAN("for x in a b; do echo $x; done", 2043, "SC2043: multi-value clean");

    /* === SC2066: double-quoted for word === */
    LINT_HAS("var=x\nfor x in \"$var\"; do echo $x; done", 2066, "SC2066: quoted for fires");

    /* === SC2249: missing *) case === */
    LINT_HAS("case $x in\na) echo a ;;\nesac", 2249, "SC2249: missing *) fires");
    LINT_CLEAN("case $x in\na) echo a ;;\n*) echo b ;;\nesac", 2249, "SC2249: has *) clean");

    /* === SC2168: local outside function === */
    LINT_HAS("local x=1", 2168, "SC2168: local outside function fires");
    LINT_CLEAN("f() {\nlocal x=1\n}", 2168, "SC2168: local inside function clean");

    /* === SC2104: break in function outside loop === */
    LINT_HAS("f() {\nbreak\n}", 2104, "SC2104: break in func outside loop fires");
    LINT_CLEAN("f() {\nwhile true; do\nbreak\ndone\n}", 2104, "SC2104: break in loop clean");

    /* === SC2105: break outside loop === */
    LINT_HAS("break", 2105, "SC2105: break outside loop fires");

    /* === SC2006: backtick command substitution === */
    LINT_HAS("echo `cmd`", 2006, "SC2006: backtick fires");
    LINT_CLEAN("echo $(cmd)", 2006, "SC2006: $() clean");

    /* === SC2034: unused variable === */
    LINT_HAS("unused=value\necho hello", 2034, "SC2034: unused var fires");
    LINT_CLEAN("file=out.txt\necho hello > $file", 2034, "SC2034: var used in redir clean");

    /* === SC2154: referenced but not assigned === */
    LINT_CLEAN("read -r var\necho $var", 2154, "SC2154: read-assigned var clean");
    LINT_CLEAN("echo $USER", 2154, "SC2154: well-known env var clean");
    LINT_CLEAN("echo $TERM", 2154, "SC2154: TERM clean");

    /* === SC2105: continue outside loop === */
    LINT_HAS("continue", 2105, "SC2105: continue outside loop fires");

    /* === SC2043/SC2066: no double-fire === */
    {
        lint_diag_t *d = run_lint("for x in \"word\"; do echo $x; done");
        int count_2043 = 0;
        int count_2066 = 0;
        const lint_diag_t *p;
        for (p = d; p != NULL; p = p->next) {
            if (p->code == 2043)
                count_2043++;
            if (p->code == 2066)
                count_2066++;
        }
        /* A quoted string literal fires SC2043 only (not both) */
        tap_ok(count_2043 + count_2066 == 1, "SC2043/SC2066: no double-fire on quoted for");
        lint_diag_free(d);
    }

    /* === SC2329: unused function === */
    LINT_HAS("f() { echo hi; }", 2329, "SC2329: unused func fires");
    LINT_CLEAN("f() { echo hi; }\nf", 2329, "SC2329: called func clean");

    /* === Output format === */
    {
        lint_diag_t *d = run_lint("echo $foo");
        strbuf_t out;
        strbuf_init(&out);
        lint_format_diags(&out, d, LINT_FMT_GCC);
        tap_ok(strstr(out.contents, "[SC") != NULL, "gcc format: contains SC code");
        strbuf_destroy(&out);
        lint_diag_free(d);
    }

    return tap_done();
}
