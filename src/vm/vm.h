#ifndef OPSH_VM_VM_H
#define OPSH_VM_VM_H

#include "agent/event.h"
#include "compiler/bytecode.h"
#include "exec/variable.h"
#include "opsh/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define VM_STACK_MAX 1024
#define VM_CONST_POOL_MAX 65535

/*
 * Module table entry.
 */
typedef struct {
    const char *name;   /* owned by the image */
    size_t init_offset; /* bytecode offset of init code */
} vm_module_t;

/*
 * Bytecode image: constant pool + bytecode segments.
 * Forward-declared as struct bytecode_image for use in vm_func_t.
 */
typedef struct bytecode_image bytecode_image_t;

/*
 * Function table entry.
 */
typedef struct {
    const char *name; /* owned by the image */
    size_t bytecode_offset;
    bytecode_image_t *image; /* image containing this function's bytecode (NULL = main) */
} vm_func_t;

struct bytecode_image {
    char **const_pool; /* array of string constants (owned) */
    uint16_t const_count;
    uint16_t const_cap; /* allocated capacity */

    uint8_t *code; /* bytecode */
    size_t code_size;
    size_t code_cap; /* allocated capacity */

    /* Function table (compiled functions) */
    vm_func_t *funcs;
    int func_count;

    /* Module table (compiled modules) */
    vm_module_t *modules;
    int module_count;
};

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
    int saved_loop_depth;
    bytecode_image_t *saved_image; /* image to restore on return */
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
    bytecode_image_t *image;      /* currently active image */
    bytecode_image_t *main_image; /* the original image (for NULL function refs) */
    size_t ip;                    /* instruction pointer */

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

    hashtable_t modules_loaded; /* module name -> (void*)1 (already initialized?) */

    /* Sub-images from eval/source kept alive for function references */
    bytecode_image_t **eval_images;
    int eval_image_count;
    int eval_image_cap;

    bool no_fork;             /* if true, fork opcodes return 127 (for fuzzing) */
    event_sink_t *event_sink; /* event output (NULL = no events) */
    int64_t next_command_id;  /* monotonic command ID counter */

    bool halted;
    bool exit_requested;   /* set by `exit` builtin (distinct from OP_HALT) */
    bool return_requested; /* set by `return` builtin to trigger OP_RET */

    const char *script_name; /* $0 */
    pid_t last_bg_pid;       /* $! */
    char option_flags[16];   /* $- */

    /* Shell options (set -e, -u, -x) */
    bool opt_errexit;       /* -e */
    bool opt_nounset;       /* -u */
    bool opt_xtrace;        /* -x */
    int errexit_suppressed; /* depth counter for if/while/&&/|| contexts */

    /* Trap handlers: command strings indexed by signal number.
     * NULL means default behavior, "" means ignore. */
    char *trap_handlers[32];
    char *exit_trap; /* EXIT trap handler (separate from signal traps) */
} vm_t;

/* Graceful exit: runs EXIT trap, then sets halted */
void vm_exit(vm_t *vm, int status);

/* Register a function in the VM (used by the compiler) */
void vm_register_func(vm_t *vm, const char *name, size_t offset);

/* Initialize a VM with a bytecode image */
void vm_init(vm_t *vm, bytecode_image_t *image);

/* Free VM resources (does not free the image) */
void vm_destroy(vm_t *vm);

/* Set positional parameters ($1..$N, $#) from command-line args */
void vm_set_args(vm_t *vm, int argc, char **argv);

/* Parse, compile, and execute a string in the current VM's environment.
 * Functions defined in the string become visible to the caller.
 * Returns the exit status. */
int vm_exec_string(vm_t *vm, const char *source, const char *label);

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
