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

int main(void)
{
    tap_plan(17);

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

    return tap_done();
}
