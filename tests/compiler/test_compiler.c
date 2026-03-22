#include "../tap.h"
#include "compiler/compiler.h"
#include "foundation/util.h"
#include "parser/parser.h"
#include "vm/disasm.h"
#include "vm/image_io.h"
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
static char *compile_and_run_from(const char *source, const char *filename, int *status_out)
{
    parser_t p;
    parser_init(&p, source, filename);
    sh_list_t *ast = parser_parse(&p);

    if (parser_error_count(&p) > 0) {
        sh_list_free(ast);
        parser_destroy(&p);
        return NULL;
    }

    bytecode_image_t *img = compile(ast, filename);
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

static char *compile_and_run(const char *source, int *status_out)
{
    return compile_and_run_from(source, "test", status_out);
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

static void test_builtin_cd_pwd(void)
{
    int status;
    char *out;
    char saved_cwd[4096];

    /* Save CWD so the cd test doesn't break subsequent tests */
    getcwd(saved_cwd, sizeof(saved_cwd));

    /* cd to a known directory and verify pwd output ends with newline */
    out = compile_and_run("cd /; pwd", &status);
    tap_is_str(out, "/\n", "cd/pwd: changes and shows directory");
    free(out);

    /* Restore CWD */
    chdir(saved_cwd);
}

static void test_builtin_export_unset(void)
{
    int status;
    char *out;

    out = compile_and_run("export X=exported; echo $X", &status);
    tap_is_str(out, "exported\n", "export: sets and echoes value");
    free(out);

    out = compile_and_run("Y=val; unset Y; echo ${Y:-gone}", &status);
    tap_is_str(out, "gone\n", "unset: removes variable");
    free(out);
}

static void test_builtin_readonly(void)
{
    int status;
    char *out;

    out = compile_and_run("readonly R=locked; echo $R", &status);
    tap_is_str(out, "locked\n", "readonly: sets value");
    free(out);
}

static void test_builtin_local(void)
{
    int status;
    char *out;

    out = compile_and_run("X=global\nf() { local X=local; echo $X; }\nf\necho $X", &status);
    tap_is_str(out, "local\nglobal\n", "local: scoped to function");
    free(out);
}

static void test_builtin_return(void)
{
    int status;
    char *out;

    out = compile_and_run("f() { echo before; return 42; echo after; }\nf\necho $?", &status);
    tap_is_str(out, "before\n42\n", "return: exits function with status");
    free(out);
}

static void test_builtin_test(void)
{
    int status;
    char *out;

    out = compile_and_run("test -d /tmp && echo yes", &status);
    tap_is_str(out, "yes\n", "test -d: directory exists");
    free(out);

    out = compile_and_run("test hello = hello && echo eq", &status);
    tap_is_str(out, "eq\n", "test =: strings equal");
    free(out);

    out = compile_and_run("test 5 -gt 3 && echo gt", &status);
    tap_is_str(out, "gt\n", "test -gt: integer comparison");
    free(out);

    out = compile_and_run("X=; [ -z $X ] && echo empty", &status);
    tap_is_str(out, "empty\n", "[: -z empty string");
    free(out);
}

static void test_builtin_printf(void)
{
    int status;
    char *out;

    out = compile_and_run("printf 'hello %s\\n' world", &status);
    tap_is_str(out, "hello world\n", "printf: string format");
    free(out);

    out = compile_and_run("printf '%d + %d = %d\\n' 2 3 5", &status);
    tap_is_str(out, "2 + 3 = 5\n", "printf: integer format");
    free(out);
}

static void test_builtin_shift(void)
{
    int status;
    char *out;

    out = compile_and_run("f() { shift; echo $1; }\nf a b c", &status);
    tap_is_str(out, "b\n", "shift: shifts positional params");
    free(out);
}

static void test_builtin_type(void)
{
    int status;
    char *out;

    out = compile_and_run("type echo", &status);
    tap_is_str(out, "echo is a shell builtin\n", "type: identifies builtin");
    free(out);
}

static void test_field_splitting(void)
{
    int status;
    char *out;

    /* $var with spaces should split into multiple arguments */
    out = compile_and_run("X='a b c'; echo $X", &status);
    tap_is_str(out, "a b c\n", "split: $var with spaces splits into 3 args");
    free(out);

    /* Empty variable should produce no arguments (0 fields) */
    out = compile_and_run("X=; echo $X end", &status);
    tap_is_str(out, "end\n", "split: empty var produces no fields");
    free(out);

    /* Quoted variable should not split */
    out = compile_and_run("X='a b c'; echo \"$X\"", &status);
    tap_is_str(out, "a b c\n", "split: quoted $var does not split");
    free(out);
}

static void test_field_splitting_ifs(void)
{
    int status;
    char *out;

    /* Custom IFS */
    out = compile_and_run("IFS=:; X='a:b:c'; echo $X", &status);
    tap_is_str(out, "a b c\n", "split: custom IFS splits on :");
    free(out);
}

static void test_no_split_in_assignment(void)
{
    int status;
    char *out;

    /* Assignment value should not be split */
    out = compile_and_run("X='a b c'; Y=$X; echo \"$Y\"", &status);
    tap_is_str(out, "a b c\n", "split: assignment value not split");
    free(out);
}

static void test_glob(void)
{
    int status;
    char *out;

    /* Glob that matches nothing returns the pattern literally */
    out = compile_and_run("echo /nonexistent_dir_xyz/*.qqq", &status);
    tap_is_str(out, "/nonexistent_dir_xyz/*.qqq\n", "glob: no match returns pattern");
    free(out);

    /* Glob that matches real files */
    mkdir("tmp", 0755);
    {
        FILE *f;
        f = fopen("tmp/glob_a.txt", "w");
        if (f) {
            fclose(f);
        }
        f = fopen("tmp/glob_b.txt", "w");
        if (f) {
            fclose(f);
        }
    }
    out = compile_and_run("echo tmp/glob_*.txt", &status);
    tap_ok(out != NULL && strstr(out, "glob_a.txt") != NULL, "glob: matches files (a)");
    tap_ok(out != NULL && strstr(out, "glob_b.txt") != NULL, "glob: matches files (b)");
    free(out);
    unlink("tmp/glob_a.txt");
    unlink("tmp/glob_b.txt");
}

static void test_exit_trap(void)
{
    int status;
    char *out;

    out = compile_and_run("trap 'echo cleanup' EXIT\nexit 0", &status);
    tap_is_str(out, "cleanup\n", "EXIT trap: runs on exit");
    free(out);
}

static void test_trap_builtin(void)
{
    int status;
    char *out;

    /* trap with no action -- should not crash */
    out = compile_and_run("trap '' INT", &status);
    tap_is_int(status, 0, "trap: ignore INT succeeds");
    free(out);

    /* trap reset */
    out = compile_and_run("trap - INT", &status);
    tap_is_int(status, 0, "trap: reset INT succeeds");
    free(out);
}

static void test_module_import(void)
{
    int status;
    char *out;

    /* Import a module and call its function.
     * The test binary runs from the project root, so use a path
     * relative to tests/compiler/ which has lib/greet.opsh */
    out = compile_and_run_from("lib::import greet\ngreet::hello", "tests/compiler/script.opsh",
                               &status);
    tap_is_str(out, "hello from greet module\n", "module: import and call");
    free(out);

    /* Call with arguments */
    out = compile_and_run_from("lib::import greet\ngreet::name world", "tests/compiler/script.opsh",
                               &status);
    tap_is_str(out, "hello world\n", "module: call with arguments");
    free(out);

    /* Duplicate import is idempotent */
    out = compile_and_run_from("lib::import greet\nlib::import greet\ngreet::hello",
                               "tests/compiler/script.opsh", &status);
    tap_is_str(out, "hello from greet module\n", "module: duplicate import is no-op");
    free(out);
}

static void test_opsb_roundtrip(void)
{
    /* Compile a script, serialize to .opsb, deserialize, and verify
     * the deserialized image produces the same output */
    const char *source = "greet() { echo hello; }\ngreet\nfor x in a b; do echo $x; done";

    parser_t p;
    parser_init(&p, source, "test");
    sh_list_t *ast = parser_parse(&p);
    bytecode_image_t *img1 = compile(ast, "test");
    sh_list_free(ast);
    parser_destroy(&p);

    tap_ok(img1 != NULL, "opsb: compiled original");

    /* Serialize */
    mkdir("tmp", 0755);
    {
        FILE *out = fopen("tmp/roundtrip.opsb", "wb");
        tap_ok(out != NULL, "opsb: opened for writing");
        if (out) {
            int r = image_write_opsb(img1, out);
            tap_is_int(r, 0, "opsb: write succeeded");
            fclose(out);
        }
    }

    /* Deserialize */
    bytecode_image_t *img2 = NULL;
    {
        FILE *in = fopen("tmp/roundtrip.opsb", "rb");
        tap_ok(in != NULL, "opsb: opened for reading");
        if (in) {
            img2 = image_read_opsb(in);
            fclose(in);
        }
    }
    tap_ok(img2 != NULL, "opsb: deserialized successfully");

    /* Structural comparison */
    if (img2 != NULL) {
        tap_is_int(img2->const_count, img1->const_count, "opsb: const count matches");
        tap_is_int((long long)img2->code_size, (long long)img1->code_size,
                   "opsb: code size matches");
        tap_is_int(img2->func_count, img1->func_count, "opsb: func count matches");
        tap_ok(memcmp(img2->code, img1->code, img1->code_size) == 0, "opsb: bytecode matches");
    } else {
        tap_skip(4, "deserialization failed");
    }

    /* Run deserialized image and compare output */
    if (img2 != NULL) {
        int pipefd[2];
        pipe(pipefd);
        fflush(stdout);
        int saved = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        vm_t vm;
        vm_init(&vm, img2);
        vm_run(&vm);

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

        tap_is_str(captured.contents, "hello\na\nb\n", "opsb: roundtrip produces same output");
        strbuf_destroy(&captured);
        vm_destroy(&vm);
    } else {
        tap_skip(1, "deserialization failed");
    }

    image_free(img1);
    image_free(img2);
    unlink("tmp/roundtrip.opsb");
}

int main(void)
{
    tap_plan(115);

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
    test_builtin_cd_pwd();
    test_builtin_export_unset();
    test_builtin_readonly();
    test_builtin_local();
    test_builtin_return();
    test_builtin_test();
    test_builtin_printf();
    test_builtin_shift();
    test_builtin_type();
    test_field_splitting();
    test_field_splitting_ifs();
    test_no_split_in_assignment();
    test_glob();
    test_exit_trap();
    test_trap_builtin();
    test_module_import();
    test_opsb_roundtrip();

    return tap_done();
}
