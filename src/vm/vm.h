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
 * Function table entry.
 */
typedef struct {
    const char *name; /* owned by the image */
    size_t bytecode_offset;
} vm_func_t;

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

    /* Function table (compiled functions) */
    vm_func_t *funcs;
    int func_count;
} bytecode_image_t;

#define VM_CALL_STACK_MAX 256
#define VM_LOOP_STACK_MAX 64
#define VM_REDIR_STACK_MAX 64
#define VM_SAVED_FD_BASE 100 /* save FDs starting from this number */

/*
 * Saved FD entry for redirection restore.
 */
typedef struct {
    int original_fd; /* the FD that was redirected */
    int saved_fd;    /* where the original was dup'd to (-1 if closed) */
} saved_fd_t;

/*
 * Redirection save frame.
 */
typedef struct {
    saved_fd_t entries[16]; /* saved FD entries */
    int count;
} redir_frame_t;

/*
 * Call frame for function calls.
 */
typedef struct {
    size_t return_ip;
    environ_t *saved_env;
    int saved_stack_base;
} call_frame_t;

/*
 * Loop frame for break/continue.
 */
typedef struct {
    size_t continue_ip;  /* jump here for continue */
    size_t break_ip;     /* jump here for break */
    int saved_stack_top; /* stack depth at loop entry */
} loop_frame_t;

/*
 * VM state.
 */
typedef struct vm {
    bytecode_image_t *image;
    size_t ip; /* instruction pointer */

    value_t stack[VM_STACK_MAX];
    int stack_top; /* index of next free slot */

    call_frame_t call_stack[VM_CALL_STACK_MAX];
    int call_depth;

    loop_frame_t loop_stack[VM_LOOP_STACK_MAX];
    int loop_depth;

    redir_frame_t redir_stack[VM_REDIR_STACK_MAX];
    int redir_depth;

    vm_func_t *func_table; /* function registry (owned) */
    int func_count;

    environ_t *env; /* current variable scope */

    int laststatus; /* $? */

    /* Output capture for builtins */
    char *captured_stdout; /* if non-NULL, builtins write here */
    size_t captured_stdout_len;
    size_t captured_stdout_cap;

    bool halted;
} vm_t;

/* Register a function in the VM (used by the compiler) */
void vm_register_func(vm_t *vm, const char *name, size_t offset);

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
