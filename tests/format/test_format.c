#include "../tap.h"
#include "format/format.h"
#include "parser/parser.h"

#include <stdlib.h>
#include <string.h>

/* Helper: format source and return result (caller frees) */
static char *fmt(const char *source, int indent)
{
    parser_t p;
    parser_init(&p, source, "<test>");
    sh_list_t *ast = parser_parse(&p);

    if (parser_error_count(&p) > 0) {
        sh_list_free(ast);
        parser_destroy(&p);
        return NULL;
    }

    comment_t *comments = parser_take_comments(&p);
    format_options_t opts = {.indent_width = indent};
    strbuf_t out;
    strbuf_init(&out);
    format_ast(&out, ast, comments, &opts);

    sh_list_free(ast);
    comment_free(comments);
    parser_destroy(&p);
    return strbuf_detach(&out);
}

/* Helper: verify idempotency of a given source */
static void check_idempotent(const char *source, const char *desc)
{
    char *first = fmt(source, 4);
    char *second = fmt(first, 4);
    tap_is_str(first, second, desc);
    free(first);
    free(second);
}

int main(void)
{
    tap_plan(49);

    /* === Basic commands === */

    {
        char *r = fmt("echo hello world", 4);
        tap_is_str(r, "echo hello world\n", "simple command");
        free(r);
    }

    {
        char *r = fmt("echo foo | cat | wc -l", 4);
        tap_is_str(r, "echo foo | cat | wc -l\n", "pipeline");
        free(r);
    }

    {
        char *r = fmt("true && echo yes || echo no", 4);
        tap_is_str(r, "true && echo yes || echo no\n", "sh-list");
        free(r);
    }

    {
        char *r = fmt("! grep error log.txt | head", 4);
        tap_is_str(r, "! grep error log.txt | head\n", "negated pipeline");
        free(r);
    }

    {
        char *r = fmt("sleep 10 &", 4);
        tap_is_str(r, "sleep 10 &\n", "async command");
        free(r);
    }

    {
        char *r = fmt("FOO=bar", 4);
        tap_is_str(r, "FOO=bar\n", "assignment");
        free(r);
    }

    {
        char *r = fmt("", 4);
        tap_is_str(r, "", "empty input");
        free(r);
    }

    /* === Compound commands === */

    {
        char *r = fmt("if true; then\necho yes\nelse\necho no\nfi", 4);
        tap_is_str(r, "if true; then\n    echo yes\nelse\n    echo no\nfi\n", "if-else");
        free(r);
    }

    {
        char *r = fmt("if true; then\necho a\nelif false; then\necho b\nelse\necho c\nfi", 4);
        tap_is_str(
            r, "if true; then\n    echo a\nelif false; then\n    echo b\nelse\n    echo c\nfi\n",
            "if-elif-else");
        free(r);
    }

    {
        char *r = fmt("for x in a b c; do\necho $x\ndone", 4);
        tap_is_str(r, "for x in a b c; do\n    echo $x\ndone\n", "for loop");
        free(r);
    }

    {
        char *r = fmt("while true; do\necho loop\nbreak\ndone", 4);
        tap_is_str(r, "while true; do\n    echo loop\n    break\ndone\n", "while loop");
        free(r);
    }

    {
        char *r = fmt("until false; do\necho loop\ndone", 4);
        tap_is_str(r, "until false; do\n    echo loop\ndone\n", "until loop");
        free(r);
    }

    {
        char *r = fmt("case $x in\nfoo)\necho foo\n;;\nesac", 4);
        tap_is_str(r, "case $x in\n(foo)\n    echo foo\n;;\nesac\n", "case break");
        free(r);
    }

    {
        char *r = fmt("case $x in\na) echo a ;& b) echo b ;| c) echo c ;; esac", 4);
        tap_is_str(r,
                   "case $x in\n(a)\n    echo a\n;&\n(b)\n    echo b\n;|\n(c)\n"
                   "    echo c\n;;\nesac\n",
                   "case fall-through and continue");
        free(r);
    }

    {
        char *r = fmt("foo() {\necho bar\n}", 4);
        tap_is_str(r, "foo() {\n    echo bar\n}\n", "function definition");
        free(r);
    }

    {
        char *r = fmt("(echo sub)", 4);
        tap_is_str(r, "(\n    echo sub\n)\n", "subshell");
        free(r);
    }

    {
        char *r = fmt("{ echo group; }", 4);
        tap_is_str(r, "{\n    echo group\n}\n", "brace group");
        free(r);
    }

    {
        char *r = fmt("[[ -f file && $x == \"yes\" ]]", 4);
        tap_is_str(r, "[[ -f file && $x == \"yes\" ]]\n", "bracket expression");
        free(r);
    }

    /* === Nesting === */

    {
        char *r = fmt("if true; then\nif false; then\necho deep\nfi\nfi", 4);
        tap_is_str(r, "if true; then\n    if false; then\n        echo deep\n    fi\nfi\n",
                   "nested compound");
        free(r);
    }

    /* === Indentation modes === */

    {
        char *r = fmt("if true; then\necho yes\nfi", 0);
        tap_is_str(r, "if true; then\n\techo yes\nfi\n", "tab indentation");
        free(r);
    }

    {
        char *r = fmt("if true; then\necho yes\nfi", 2);
        tap_is_str(r, "if true; then\n  echo yes\nfi\n", "2-space indentation");
        free(r);
    }

    /* === Quoting and expansions === */

    {
        char *r = fmt("echo \"$var\"", 4);
        tap_is_str(r, "echo \"$var\"\n", "double-quoted variable");
        free(r);
    }

    {
        char *r = fmt("echo $var", 4);
        tap_is_str(r, "echo $var\n", "unquoted variable");
        free(r);
    }

    {
        char *r = fmt("echo \"${var:-default}\"", 4);
        tap_is_str(r, "echo \"${var:-default}\"\n", "param expansion minus");
        free(r);
    }

    {
        char *r = fmt("echo \"${var:+alt}\"", 4);
        tap_is_str(r, "echo \"${var:+alt}\"\n", "param expansion plus");
        free(r);
    }

    {
        char *r = fmt("echo \"${var:=assign}\"", 4);
        tap_is_str(r, "echo \"${var:=assign}\"\n", "param expansion assign");
        free(r);
    }

    {
        char *r = fmt("echo \"${var:?error}\"", 4);
        tap_is_str(r, "echo \"${var:?error}\"\n", "param expansion error");
        free(r);
    }

    {
        char *r = fmt("echo ${#var}", 4);
        tap_is_str(r, "echo ${#var}\n", "param expansion length");
        free(r);
    }

    {
        char *r = fmt("echo \"${var#prefix}\"", 4);
        tap_is_str(r, "echo \"${var#prefix}\"\n", "param expansion match head");
        free(r);
    }

    {
        char *r = fmt("echo \"${var%%.txt}\"", 4);
        tap_is_str(r, "echo \"${var%%.txt}\"\n", "param expansion match tail longest");
        free(r);
    }

    {
        char *r = fmt("echo \"${var//old/new}\"", 4);
        tap_is_str(r, "echo \"${var//old/new}\"\n", "param expansion subst all");
        free(r);
    }

    {
        char *r = fmt("echo \"$((1 + 2))\"", 4);
        tap_is_str(r, "echo \"$((1 + 2))\"\n", "arithmetic expansion");
        free(r);
    }

    {
        char *r = fmt("echo \"hello $(whoami) world\"", 4);
        tap_is_str(r, "echo \"hello $(whoami) world\"\n", "command substitution");
        free(r);
    }

    {
        char *r = fmt("echo \"prefix\"$var\"suffix\"", 4);
        tap_is_str(r, "echo \"prefix\"$var\"suffix\"\n", "mixed quoted and unquoted");
        free(r);
    }

    /* Special parameters */
    {
        char *r = fmt("echo \"$?\" \"$#\" \"$@\" \"$*\"", 4);
        tap_is_str(r, "echo \"$?\" \"$#\" \"$@\" \"$*\"\n", "special parameters");
        free(r);
    }

    /* === Redirections === */

    {
        char *r = fmt("echo hello > /dev/null", 4);
        tap_is_str(r, "echo hello > /dev/null\n", "output redirect");
        free(r);
    }

    {
        char *r = fmt("echo x >> file.txt", 4);
        tap_is_str(r, "echo x >> file.txt\n", "append redirect");
        free(r);
    }

    {
        char *r = fmt("cat < input.txt", 4);
        tap_is_str(r, "cat < input.txt\n", "input redirect");
        free(r);
    }

    {
        char *r = fmt("echo x >| file.txt", 4);
        tap_is_str(r, "echo x >| file.txt\n", "clobber redirect");
        free(r);
    }

    {
        char *r = fmt("echo error 2>&1", 4);
        tap_is_str(r, "echo error 2>&1\n", "fd duplication");
        free(r);
    }

    {
        char *r = fmt("cat <<< \"hello\"", 4);
        tap_is_str(r, "cat <<< \"hello\"\n", "here-string");
        free(r);
    }

    {
        char *r = fmt("cat << EOF\nhello world\nEOF", 4);
        tap_is_str(r, "cat << EOF\nhello world\nEOF\n", "here-document");
        free(r);
    }

    /* === Comments === */

    {
        char *r = fmt("# hello\necho world", 4);
        tap_is_str(r, "# hello\necho world\n", "leading comment");
        free(r);
    }

    {
        char *r = fmt("echo hello # comment\necho world", 4);
        tap_is_str(r, "echo hello # comment\necho world\n", "trailing comment");
        free(r);
    }

    {
        char *r = fmt("if true; then\n# inside\necho yes\nfi", 4);
        tap_is_str(r, "if true; then\n    # inside\n    echo yes\nfi\n", "comment inside compound");
        free(r);
    }

    {
        char *r = fmt("echo hello\n# end", 4);
        tap_is_str(r, "echo hello\n# end\n", "comment at end of file");
        free(r);
    }

    {
        char *r = fmt("# just a comment", 4);
        tap_is_str(r, "# just a comment\n", "comment-only file");
        free(r);
    }

    /* === Idempotency === */

    check_idempotent("if true; then\necho yes\nfi", "idempotent: if");
    check_idempotent("# comment\necho hello # trailing\n# end", "idempotent: comments");

    return tap_done();
}
