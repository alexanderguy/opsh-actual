#ifndef OPSH_VM_VM_H
#define OPSH_VM_VM_H

#include "compiler/bytecode.h"
#include "exec/variable.h"
#include "opsh/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VM_STACK_MAX 1024
#define VM_CONST_POOL_MAX 65535

/*
 * Bytecode image: constant pool + bytecode segments.
 */
typedef struct {
    char **const_pool; /* array of string constants (owned) */
    uint16_t const_count;
    uint16_t const_cap; /* allocated capacity */

    uint8_t *code; /* bytecode */
    size_t code_size;
    size_t code_cap; /* allocated capacity */
} bytecode_image_t;

/*
 * VM state.
 */
typedef struct vm {
    bytecode_image_t *image;
    size_t ip; /* instruction pointer */

    value_t stack[VM_STACK_MAX];
    int stack_top; /* index of next free slot */

    environ_t *env; /* current variable scope */

    int laststatus; /* $? */

    /* Output capture for builtins */
    char *captured_stdout; /* if non-NULL, builtins write here */
    size_t captured_stdout_len;
    size_t captured_stdout_cap;

    bool halted;
} vm_t;

/* Initialize a VM with a bytecode image */
void vm_init(vm_t *vm, bytecode_image_t *image);

/* Free VM resources (does not free the image) */
void vm_destroy(vm_t *vm);

/* Execute until HALT or error. Returns the exit status. */
int vm_run(vm_t *vm);

/* Stack operations (used by builtins and tests) */
void vm_push(vm_t *vm, value_t val);
value_t vm_pop(vm_t *vm);
value_t vm_peek(vm_t *vm, int offset); /* 0 = top */

/* Bytecode image management */
bytecode_image_t *image_new(void);
void image_free(bytecode_image_t *img);
uint16_t image_add_const(bytecode_image_t *img, const char *str);
void image_emit_u8(bytecode_image_t *img, uint8_t byte);
void image_emit_u16(bytecode_image_t *img, uint16_t val);
void image_emit_i32(bytecode_image_t *img, int32_t val);

#endif /* OPSH_VM_VM_H */
