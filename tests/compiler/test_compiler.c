#include "../tap.h"
#include "compiler/compiler.h"
#include "foundation/util.h"
#include "parser/parser.h"
#include "vm/disasm.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Helper: compile source, run it, capture stdout, return the output.
 * Caller must free the returned string. Returns NULL on compile error.
 */
static char *compile_and_run(const char *source, int *status_out)
{
    parser_t p;
    parser_init(&p, source, "test");
    sh_list_t *ast = parser_parse(&p);

    if (parser_error_count(&p) > 0) {
        sh_list_free(ast);
        parser_destroy(&p);
        return NULL;
    }

    bytecode_image_t *img = compile(ast, "test");
    sh_list_free(ast);
    parser_destroy(&p);

    if (img == NULL) {
        return NULL;
    }

    /* Capture stdout by redirecting fd 1 to a pipe */
    int pipefd[2];
    pipe(pipefd);
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    vm_t vm;
    vm_init(&vm, img);

    int status = vm_run(&vm);
    if (status_out != NULL) {
        *status_out = status;
    }

    /* Restore stdout and read captured output */
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

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

static void test_echo_hello_world(void)
{
    int status;
    char *out = compile_and_run("echo hello world", &status);
    tap_ok(out != NULL, "echo hello world: compiled and ran");
    tap_is_str(out, "hello world\n", "echo hello world: correct output");
    tap_is_int(status, 0, "echo hello world: exit status 0");
    free(out);
}

static void test_echo_single_word(void)
{
    int status;
    char *out = compile_and_run("echo hi", &status);
    tap_is_str(out, "hi\n", "echo hi: correct output");
    tap_is_int(status, 0, "echo hi: exit status 0");
    free(out);
}

static void test_echo_no_args(void)
{
    int status;
    char *out = compile_and_run("echo", &status);
    tap_is_str(out, "\n", "echo (no args): outputs newline");
    free(out);
}

static void test_multiple_commands(void)
{
    int status;
    char *out = compile_and_run("echo first; echo second; echo third", &status);
    tap_is_str(out, "first\nsecond\nthird\n", "multiple commands: correct output");
    free(out);
}

static void test_assignment(void)
{
    int status;
    char *out = compile_and_run("VAR=hello", &status);
    tap_ok(out != NULL, "assignment: compiled and ran");
    tap_is_int(status, 0, "assignment: exit status 0");
    free(out);
}

static void test_true_false(void)
{
    int status;
    char *out;

    out = compile_and_run("true", &status);
    tap_is_int(status, 0, "true: exit status 0");
    free(out);

    out = compile_and_run("false", &status);
    tap_is_int(status, 1, "false: exit status 1");
    free(out);
}

static void test_exit_status(void)
{
    int status;
    char *out = compile_and_run("exit 42", &status);
    tap_is_int(status, 42, "exit 42: correct exit status");
    free(out);
}

static void test_colon(void)
{
    int status;
    char *out = compile_and_run(":", &status);
    tap_is_int(status, 0, "colon: exit status 0");
    free(out);
}

static void test_unknown_command(void)
{
    int status;
    char *out = compile_and_run("nonexistent_command", &status);
    tap_ok(out != NULL, "unknown command: compiled");
    tap_ok(status != 0, "unknown command: non-zero exit status");
    free(out);
}

static void test_disasm_output(void)
{
    parser_t p;
    parser_init(&p, "echo hello", "test");
    sh_list_t *ast = parser_parse(&p);
    bytecode_image_t *img = compile(ast, "test");
    sh_list_free(ast);
    parser_destroy(&p);

    tap_ok(img != NULL, "disasm: image compiled");

    FILE *devnull = fopen("/dev/null", "w");
    if (devnull != NULL) {
        disasm_image(img, devnull);
        fclose(devnull);
    }
    tap_ok(1, "disasm: compiled bytecode disassembles without crash");

    image_free(img);
}

static void test_variable_expansion(void)
{
    int status;
    char *out = compile_and_run("VAR=hello; echo $VAR", &status);
    tap_is_str(out, "hello\n", "$VAR: expands variable");
    free(out);
}

static void test_variable_in_word(void)
{
    int status;
    char *out = compile_and_run("NAME=world; echo hello$NAME", &status);
    tap_is_str(out, "helloworld\n", "hello$NAME: concatenated expansion");
    free(out);
}

static void test_braced_variable(void)
{
    int status;
    char *out = compile_and_run("X=foo; echo ${X}bar", &status);
    tap_is_str(out, "foobar\n", "${X}bar: braced expansion");
    free(out);
}

static void test_unset_variable(void)
{
    int status;
    char *out = compile_and_run("echo $UNSET_VAR", &status);
    tap_is_str(out, "\n", "unset var: expands to empty");
    free(out);
}

static void test_param_default(void)
{
    int status;
    char *out;

    out = compile_and_run("echo ${MISS:-fallback}", &status);
    tap_is_str(out, "fallback\n", "${:-}: uses default when unset");
    free(out);

    out = compile_and_run("HIT=value; echo ${HIT:-fallback}", &status);
    tap_is_str(out, "value\n", "${:-}: uses value when set");
    free(out);

    out = compile_and_run("EMPTY=; echo ${EMPTY:-fallback}", &status);
    tap_is_str(out, "fallback\n", "${:-}: uses default when empty (colon)");
    free(out);
}

static void test_param_assign(void)
{
    int status;
    char *out = compile_and_run("echo ${NEW:=assigned}; echo $NEW", &status);
    tap_is_str(out, "assigned\nassigned\n", "${:=}: assigns and returns");
    free(out);
}

static void test_param_alternate(void)
{
    int status;
    char *out;

    out = compile_and_run("X=yes; echo ${X:+alternate}", &status);
    tap_is_str(out, "alternate\n", "${:+}: uses alternate when set");
    free(out);

    out = compile_and_run("echo ${MISS:+alternate}", &status);
    tap_is_str(out, "\n", "${:+}: empty when unset");
    free(out);
}

static void test_param_error(void)
{
    int status;
    char *out = compile_and_run("echo ${MISS:?oops}", &status);
    tap_ok(status != 0, "${:?}: non-zero exit on unset");
    free(out);
}

static void test_param_length(void)
{
    int status;
    char *out = compile_and_run("X=hello; echo ${#X}", &status);
    tap_is_str(out, "5\n", "${#X}: string length");
    free(out);
}

static void test_special_question(void)
{
    int status;
    char *out = compile_and_run("true; echo $?", &status);
    tap_is_str(out, "0\n", "$?: exit status of last command");
    free(out);
}

static void test_arithmetic(void)
{
    int status;
    char *out;

    out = compile_and_run("echo $((1 + 2))", &status);
    tap_is_str(out, "3\n", "$((1+2)): basic addition");
    free(out);

    out = compile_and_run("echo $((10 - 3))", &status);
    tap_is_str(out, "7\n", "$((10-3)): subtraction");
    free(out);

    out = compile_and_run("echo $((4 * 5))", &status);
    tap_is_str(out, "20\n", "$((4*5)): multiplication");
    free(out);
}

static void test_and_if(void)
{
    int status;
    char *out;

    out = compile_and_run("true && echo yes", &status);
    tap_is_str(out, "yes\n", "&&: runs second when first succeeds");
    free(out);

    out = compile_and_run("false && echo no", &status);
    tap_is_str(out, "", "&&: skips second when first fails");
    free(out);
}

static void test_or_if(void)
{
    int status;
    char *out;

    out = compile_and_run("false || echo fallback", &status);
    tap_is_str(out, "fallback\n", "||: runs second when first fails");
    free(out);

    out = compile_and_run("true || echo no", &status);
    tap_is_str(out, "", "||: skips second when first succeeds");
    free(out);
}

static void test_and_or_chain(void)
{
    int status;
    char *out;

    out = compile_and_run("true && echo a && echo b", &status);
    tap_is_str(out, "a\nb\n", "&& chain: both run on success");
    free(out);

    out = compile_and_run("false && echo a || echo b", &status);
    tap_is_str(out, "b\n", "&& || mix: falls through to ||");
    free(out);

    out = compile_and_run("true && false || echo c", &status);
    tap_is_str(out, "c\n", "&& || mix: second fails, || catches");
    free(out);
}

static void test_and_or_status(void)
{
    int status;
    char *out;

    out = compile_and_run("true && true", &status);
    tap_is_int(status, 0, "&& status: 0 when both succeed");
    free(out);

    out = compile_and_run("true && false", &status);
    tap_is_int(status, 1, "&& status: 1 when second fails");
    free(out);

    out = compile_and_run("false || true", &status);
    tap_is_int(status, 0, "|| status: 0 when second succeeds");
    free(out);

    out = compile_and_run("false || false", &status);
    tap_is_int(status, 1, "|| status: 1 when both fail");
    free(out);
}

static void test_negation(void)
{
    int status;
    char *out;

    out = compile_and_run("! true", &status);
    tap_is_int(status, 1, "! true: status 1");
    free(out);

    out = compile_and_run("! false", &status);
    tap_is_int(status, 0, "! false: status 0");
    free(out);

    out = compile_and_run("! true && echo no", &status);
    tap_is_str(out, "", "! true &&: skips (negated success = failure)");
    free(out);

    out = compile_and_run("! false && echo yes", &status);
    tap_is_str(out, "yes\n", "! false &&: runs (negated failure = success)");
    free(out);
}

static void test_if_command(void)
{
    int status;
    char *out = compile_and_run("if true; then echo yes; fi", &status);
    tap_is_str(out, "yes\n", "if true: runs body");
    free(out);

    out = compile_and_run("if false; then echo no; fi", &status);
    tap_is_str(out, "", "if false: skips body");
    free(out);
}

static void test_if_else(void)
{
    int status;
    char *out = compile_and_run("if false; then echo no; else echo yes; fi", &status);
    tap_is_str(out, "yes\n", "if-else: runs else");
    free(out);

    out = compile_and_run("if true; then echo yes; else echo no; fi", &status);
    tap_is_str(out, "yes\n", "if-else: runs then");
    free(out);
}

static void test_if_elif(void)
{
    int status;
    char *out =
        compile_and_run("if false; then echo a; elif true; then echo b; else echo c; fi", &status);
    tap_is_str(out, "b\n", "if-elif-else: runs elif");
    free(out);
}

static void test_for_loop(void)
{
    int status;
    char *out = compile_and_run("for x in a b c; do echo $x; done", &status);
    tap_is_str(out, "a\nb\nc\n", "for loop: iterates over words");
    free(out);

    out = compile_and_run("for x in hello; do echo $x; done", &status);
    tap_is_str(out, "hello\n", "for loop: single word");
    free(out);
}

static void test_while_loop(void)
{
    int status;
    /* Simple test: condition immediately false */
    char *out = compile_and_run("while false; do echo loop; done", &status);
    tap_is_str(out, "", "while false: never enters loop");
    free(out);
}

static void test_until_loop(void)
{
    int status;
    char *out = compile_and_run("until true; do echo loop; done", &status);
    tap_is_str(out, "", "until true: never enters loop");
    free(out);
}

static void test_case_command(void)
{
    int status;
    char *out;

    out = compile_and_run("X=hello; case $X in\nhello) echo matched;;\n*) echo default;;\nesac",
                          &status);
    tap_is_str(out, "matched\n", "case: exact match");
    free(out);

    out =
        compile_and_run("X=other; case $X in\nhello) echo no;;\n*) echo default;;\nesac", &status);
    tap_is_str(out, "default\n", "case: glob fallthrough to *");
    free(out);
}

static void test_brace_group(void)
{
    int status;
    char *out = compile_and_run("{ echo hello; echo world; }", &status);
    tap_is_str(out, "hello\nworld\n", "brace group: runs both commands");
    free(out);
}

static void test_function_def(void)
{
    int status;
    char *out;

    out = compile_and_run("greet() { echo hello; }; greet", &status);
    tap_is_str(out, "hello\n", "function: define and call");
    free(out);

    out = compile_and_run("add() { echo done; }\nadd\nadd", &status);
    tap_is_str(out, "done\ndone\n", "function: call twice");
    free(out);
}

static void test_nested_control(void)
{
    int status;
    char *out;

    out = compile_and_run("for x in a b; do if true; then echo $x; fi; done", &status);
    tap_is_str(out, "a\nb\n", "nested: for + if");
    free(out);

    out = compile_and_run("if true; then for x in 1 2; do echo $x; done; fi", &status);
    tap_is_str(out, "1\n2\n", "nested: if + for");
    free(out);
}

static void test_command_substitution(void)
{
    int status;
    char *out;

    out = compile_and_run("X=$(echo hello); echo $X", &status);
    tap_is_str(out, "hello\n", "$(echo hello): captures output");
    free(out);

    out = compile_and_run("echo $(echo world)", &status);
    tap_is_str(out, "world\n", "$(echo world): inline cmdsub");
    free(out);

    out = compile_and_run("echo a$(echo b)c", &status);
    tap_is_str(out, "abc\n", "a$(echo b)c: cmdsub in word");
    free(out);
}

static void test_pipeline(void)
{
    int status;
    char *out;

    /* Single-command pipeline (no fork) */
    out = compile_and_run("echo hello", &status);
    tap_is_str(out, "hello\n", "pipeline: single command");
    free(out);

    /* Multi-command pipeline -- output goes to stdout of last command.
     * Since we capture stdout in the parent, and children write to real
     * stdout, we need to test differently. Use command substitution to
     * capture pipeline output. */
    out = compile_and_run("X=$(echo hello | true); echo done", &status);
    tap_is_str(out, "done\n", "pipeline: runs without crash");
    free(out);
}

static void test_negated_pipeline(void)
{
    int status;
    char *out;

    out = compile_and_run("! true", &status);
    tap_is_int(status, 1, "! true: negated to 1");
    free(out);

    out = compile_and_run("! false", &status);
    tap_is_int(status, 0, "! false: negated to 0");
    free(out);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (len > 0) {
        fread(buf, 1, (size_t)len, f);
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static void test_output_redirection(void)
{
    int status;

    /* echo hello > file */
    unlink("tmp/test_redir_out.txt");
    mkdir("tmp", 0755);
    char *out = compile_and_run("echo hello >tmp/test_redir_out.txt", &status);
    tap_is_int(status, 0, "> redirect: exit status 0");
    /* Output should NOT go to captured_stdout */
    tap_is_str(out, "", "> redirect: nothing on stdout");
    free(out);

    char *contents = read_file("tmp/test_redir_out.txt");
    tap_is_str(contents, "hello\n", "> redirect: file contains output");
    free(contents);
    unlink("tmp/test_redir_out.txt");
}

static void test_append_redirection(void)
{
    int status;

    mkdir("tmp", 0755);
    char *out;
    out = compile_and_run("echo first >tmp/test_redir_app.txt", &status);
    free(out);
    out = compile_and_run("echo second >>tmp/test_redir_app.txt", &status);
    tap_is_int(status, 0, ">> redirect: exit status 0");
    free(out);

    char *contents = read_file("tmp/test_redir_app.txt");
    tap_is_str(contents, "first\nsecond\n", ">> redirect: appended");
    free(contents);
    unlink("tmp/test_redir_app.txt");
}

static void test_fd_dup(void)
{
    int status;

    /* Redirect stderr to stdout via 2>&1, capture both */
    /* Since our echo writes to fd 1, and 2>&1 copies fd 1 to fd 2,
     * this test just verifies the dup doesn't crash */
    mkdir("tmp", 0755);
    char *out = compile_and_run("echo hello 2>&1", &status);
    tap_is_int(status, 0, "2>&1: exit status 0");
    free(out);
}

int main(void)
{
    tap_plan(77);

    test_echo_hello_world();
    test_echo_single_word();
    test_echo_no_args();
    test_multiple_commands();
    test_assignment();
    test_true_false();
    test_exit_status();
    test_colon();
    test_unknown_command();
    test_disasm_output();
    test_variable_expansion();
    test_variable_in_word();
    test_braced_variable();
    test_unset_variable();
    test_param_default();
    test_param_assign();
    test_param_alternate();
    test_param_error();
    test_param_length();
    test_special_question();
    test_arithmetic();
    test_and_if();
    test_or_if();
    test_and_or_chain();
    test_and_or_status();
    test_negation();
    test_if_command();
    test_if_else();
    test_if_elif();
    test_for_loop();
    test_while_loop();
    test_until_loop();
    test_case_command();
    test_brace_group();
    test_function_def();
    test_nested_control();
    test_command_substitution();
    test_pipeline();
    test_negated_pipeline();
    test_output_redirection();
    test_append_redirection();
    test_fd_dup();

    return tap_done();
}
