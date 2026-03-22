#include "../asm.h"
#include "../tap.h"
#include "foundation/util.h"
#include "vm/disasm.h"
#include "vm/vm.h"

#include <stdlib.h>
#include <string.h>

static void test_echo_hello_world(void)
{
    bytecode_image_t *img = image_new();
    uint16_t c_echo = image_add_const(img, "echo");
    uint16_t c_hello = image_add_const(img, "hello");
    uint16_t c_world = image_add_const(img, "world");

    /* echo hello world */
    ASM_REDIR_SAVE(img);
    ASM_PUSH_CONST(img, c_echo);
    ASM_PUSH_CONST(img, c_hello);
    ASM_PUSH_CONST(img, c_world);
    ASM_PUSH_INT(img, 3); /* argc = 3 */
    ASM_EXEC_BUILTIN(img, BUILTIN_ECHO);
    ASM_REDIR_RESTORE(img);
    ASM_HALT(img);

    vm_t vm;
    vm_init(&vm, img);

    /* Capture stdout */
    vm.captured_stdout = xcalloc(1, 256);
    vm.captured_stdout_cap = 256;
    vm.captured_stdout_len = 0;

    int status = vm_run(&vm);
    tap_is_int(status, 0, "echo hello world: exit status 0");
    tap_is_str(vm.captured_stdout, "hello world\n", "echo hello world: output");

    vm_destroy(&vm);
    image_free(img);
}

static void test_push_pop(void)
{
    bytecode_image_t *img = image_new();
    image_add_const(img, "test");

    ASM_PUSH_CONST(img, 0);
    ASM_PUSH_INT(img, 42);
    ASM_POP(img); /* pop the integer */
    ASM_HALT(img);

    vm_t vm;
    vm_init(&vm, img);
    vm_run(&vm);

    tap_is_int(vm.stack_top, 1, "push/pop: one value on stack");
    tap_is_int(vm.stack[0].type, VT_STRING, "push/pop: remaining is string");
    tap_is_str(vm.stack[0].data.string, "test", "push/pop: value is test");

    vm_destroy(&vm);
    image_free(img);
}

static void test_dup(void)
{
    bytecode_image_t *img = image_new();
    image_add_const(img, "abc");

    ASM_PUSH_CONST(img, 0);
    ASM_DUP(img);
    ASM_HALT(img);

    vm_t vm;
    vm_init(&vm, img);
    vm_run(&vm);

    tap_is_int(vm.stack_top, 2, "dup: two values on stack");
    tap_is_str(vm.stack[0].data.string, "abc", "dup: first is abc");
    tap_is_str(vm.stack[1].data.string, "abc", "dup: second is abc");

    vm_destroy(&vm);
    image_free(img);
}

static void test_concat(void)
{
    bytecode_image_t *img = image_new();
    image_add_const(img, "hello");
    image_add_const(img, " ");
    image_add_const(img, "world");

    ASM_PUSH_CONST(img, 0);
    ASM_PUSH_CONST(img, 1);
    ASM_PUSH_CONST(img, 2);
    ASM_CONCAT(img, 3);
    ASM_HALT(img);

    vm_t vm;
    vm_init(&vm, img);
    vm_run(&vm);

    tap_is_int(vm.stack_top, 1, "concat: one result");
    tap_is_str(vm.stack[0].data.string, "hello world", "concat: correct value");

    vm_destroy(&vm);
    image_free(img);
}

static void test_get_special_question(void)
{
    bytecode_image_t *img = image_new();

    ASM_GET_SPECIAL(img, SPECIAL_QUESTION);
    ASM_HALT(img);

    vm_t vm;
    vm_init(&vm, img);
    vm.laststatus = 42;
    vm_run(&vm);

    tap_is_int(vm.stack_top, 1, "$?: one value");
    tap_is_str(vm.stack[0].data.string, "42", "$?: value is 42");

    vm_destroy(&vm);
    image_free(img);
}

static void test_iterator(void)
{
    bytecode_image_t *img = image_new();
    uint16_t c_a = image_add_const(img, "a");
    uint16_t c_b = image_add_const(img, "b");
    uint16_t c_c = image_add_const(img, "c");

    ASM_PUSH_CONST(img, c_a);
    ASM_PUSH_CONST(img, c_b);
    ASM_PUSH_CONST(img, c_c);
    ASM_PUSH_INT(img, 3);
    ASM_INIT_ITER(img, 1);

    /*
     * GET_NEXT_ITER peeks at stack top for the iterator. Each yielded
     * value must be consumed (popped) before the next call, so the
     * iterator stays at top. We get each value, verify it's not none,
     * then pop it. After exhaustion, we verify VT_NONE.
     */
    ASM_GET_NEXT_ITER(img); /* push "a"; stack: [iter, "a"] */
    ASM_POP(img);           /* pop "a"; stack: [iter] */
    ASM_GET_NEXT_ITER(img); /* push "b" */
    ASM_POP(img);
    ASM_GET_NEXT_ITER(img); /* push "c" */
    ASM_POP(img);
    ASM_GET_NEXT_ITER(img); /* push VT_NONE (exhausted) */
    ASM_HALT(img);

    vm_t vm;
    vm_init(&vm, img);
    vm_run(&vm);

    /* Stack: [iterator, VT_NONE] */
    tap_is_int(vm.stack_top, 2, "iterator: 2 values on stack");
    tap_is_int(vm.stack[0].type, VT_ITERATOR, "iterator: slot 0 is iterator");
    tap_is_int(vm.stack[1].type, VT_NONE, "iterator: exhausted yields VT_NONE");

    /* Now verify the iterator actually had the right elements by checking
     * that it was fully consumed (position == count) */
    tap_is_int(vm.stack[0].data.iterator.count, 3, "iterator: had 3 elements");
    tap_is_int(vm.stack[0].data.iterator.position, 3, "iterator: all consumed");
    tap_is_str(vm.stack[0].data.iterator.elements[0], "a", "iterator: element 0 is a");
    tap_is_str(vm.stack[0].data.iterator.elements[1], "b", "iterator: element 1 is b");
    tap_is_str(vm.stack[0].data.iterator.elements[2], "c", "iterator: element 2 is c");

    vm_destroy(&vm);
    image_free(img);
}

static void test_exit_builtin(void)
{
    bytecode_image_t *img = image_new();
    image_add_const(img, "exit");
    image_add_const(img, "42");

    ASM_PUSH_CONST(img, 0); /* exit */
    ASM_PUSH_CONST(img, 1); /* 42 */
    ASM_PUSH_INT(img, 2);   /* argc */
    ASM_EXEC_BUILTIN(img, BUILTIN_EXIT);
    ASM_HALT(img);

    vm_t vm;
    vm_init(&vm, img);
    int status = vm_run(&vm);

    tap_is_int(status, 42, "exit 42: status is 42");

    vm_destroy(&vm);
    image_free(img);
}

static void test_disassembler(void)
{
    bytecode_image_t *img = image_new();
    image_add_const(img, "echo");
    image_add_const(img, "hello");

    ASM_PUSH_CONST(img, 0);
    ASM_PUSH_CONST(img, 1);
    ASM_PUSH_INT(img, 2);
    ASM_EXEC_BUILTIN(img, BUILTIN_ECHO);
    ASM_HALT(img);

    /* Just verify it doesn't crash */
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull != NULL) {
        disasm_image(img, devnull);
        fclose(devnull);
    }
    tap_ok(1, "disassembler: runs without crash");

    image_free(img);
}

static void test_const_pool_dedup(void)
{
    bytecode_image_t *img = image_new();
    uint16_t a1 = image_add_const(img, "hello");
    uint16_t a2 = image_add_const(img, "hello");
    uint16_t b = image_add_const(img, "world");

    tap_is_int(a1, a2, "const pool: deduplicates same string");
    tap_ok(b != a1, "const pool: different strings get different indices");
    tap_is_int(img->const_count, 2, "const pool: count reflects dedup");

    image_free(img);
}

int main(void)
{
    tap_plan(25);

    test_echo_hello_world();
    test_push_pop();
    test_dup();
    test_concat();
    test_get_special_question();
    test_iterator();
    test_exit_builtin();
    test_disassembler();
    test_const_pool_dedup();

    return tap_done();
}
