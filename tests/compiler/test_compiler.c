#include "../tap.h"
#include "compiler/compiler.h"
#include "foundation/util.h"
#include "parser/parser.h"
#include "vm/disasm.h"
#include "vm/vm.h"

#include <stdlib.h>
#include <string.h>

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

    vm_t vm;
    vm_init(&vm, img);
    vm.captured_stdout = xcalloc(1, 1024);
    vm.captured_stdout_cap = 1024;
    vm.captured_stdout_len = 0;

    int status = vm_run(&vm);
    if (status_out != NULL) {
        *status_out = status;
    }

    char *output = vm.captured_stdout;
    vm.captured_stdout = NULL;
    vm_destroy(&vm);
    image_free(img);

    return output;
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

int main(void)
{
    tap_plan(33);

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

    return tap_done();
}
