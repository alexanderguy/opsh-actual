#include "vm/vm.h"

#include "builtins/builtins.h"
#include "compiler/compiler.h"
#include "exec/signal.h"
#include "foundation/rcstr.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "vm/arith.h"

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <inttypes.h>
#include <pwd.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Bytecode image management
 */

bytecode_image_t *image_new(void)
{
    bytecode_image_t *img = xcalloc(1, sizeof(*img));
    img->const_pool = NULL;
    img->const_count = 0;
    img->code = NULL;
    img->code_size = 0;
    return img;
}

void image_free(bytecode_image_t *img)
{
    uint16_t i;
    if (img == NULL) {
        return;
    }
    for (i = 0; i < img->const_count; i++) {
        rcstr_release(img->const_pool[i]);
    }
    free(img->const_pool);
    free(img->code);
    {
        int fi;
        for (fi = 0; fi < img->func_count; fi++) {
            free((void *)img->funcs[fi].name);
        }
        free(img->funcs);
    }
    {
        int mi;
        for (mi = 0; mi < img->module_count; mi++) {
            free((void *)img->modules[mi].name);
        }
        free(img->modules);
    }
    free(img);
}

uint16_t image_add_const(bytecode_image_t *img, const char *str)
{
    uint16_t i;

    /* Check for duplicates */
    for (i = 0; i < img->const_count; i++) {
        if (strcmp(img->const_pool[i], str) == 0) {
            return i;
        }
    }

    if (img->const_count >= VM_CONST_POOL_MAX) {
        fprintf(stderr, "opsh: constant pool overflow (max %d constants)\n", VM_CONST_POOL_MAX);
        abort();
    }

    if (img->const_count >= img->const_cap) {
        uint16_t new_cap = img->const_cap ? img->const_cap * 2 : 16;
        if (new_cap < img->const_cap) {
            new_cap = VM_CONST_POOL_MAX; /* overflow: clamp */
        }
        img->const_pool = xrealloc(img->const_pool, (size_t)new_cap * sizeof(char *));
        img->const_cap = new_cap;
    }
    img->const_pool[img->const_count] = rcstr_new(str);
    return img->const_count++;
}

static void image_ensure_capacity(bytecode_image_t *img, size_t additional)
{
    size_t needed;
    size_t new_cap;

    if (checked_add(img->code_size, additional, &needed) != 0) {
        fprintf(stderr, "opsh: bytecode size overflow\n");
        abort();
    }
    if (needed <= img->code_cap) {
        return;
    }
    new_cap = img->code_cap ? img->code_cap : 64;
    while (new_cap < needed) {
        size_t doubled;
        if (checked_mul(new_cap, 2, &doubled) != 0) {
            new_cap = needed;
            break;
        }
        new_cap = doubled;
    }
    img->code = xrealloc(img->code, new_cap);
    img->code_cap = new_cap;
}

void image_emit_u8(bytecode_image_t *img, uint8_t byte)
{
    image_ensure_capacity(img, 1);
    img->code[img->code_size++] = byte;
}

void image_emit_u16(bytecode_image_t *img, uint16_t val)
{
    image_ensure_capacity(img, 2);
    img->code[img->code_size++] = (uint8_t)(val & 0xFF);
    img->code[img->code_size++] = (uint8_t)((val >> 8) & 0xFF);
}

void image_emit_i32(bytecode_image_t *img, int32_t val)
{
    uint32_t uval = (uint32_t)val;
    image_ensure_capacity(img, 4);
    img->code[img->code_size++] = (uint8_t)(uval & 0xFF);
    img->code[img->code_size++] = (uint8_t)((uval >> 8) & 0xFF);
    img->code[img->code_size++] = (uint8_t)((uval >> 16) & 0xFF);
    img->code[img->code_size++] = (uint8_t)((uval >> 24) & 0xFF);
}

/*
 * VM core
 */

void vm_init(vm_t *vm, bytecode_image_t *image)
{
    memset(vm, 0, sizeof(*vm));
    vm->image = image;
    vm->main_image = image;
    vm->ip = 0;
    vm->stack_top = 0;
    vm->call_depth = 0;
    vm->loop_depth = 0;
    /* Copy the function table so it can be extended independently */
    if (image != NULL && image->func_count > 0) {
        vm->func_table = xmalloc((size_t)image->func_count * sizeof(vm_func_t));
        memcpy(vm->func_table, image->funcs, (size_t)image->func_count * sizeof(vm_func_t));
        vm->func_count = image->func_count;
    } else {
        vm->func_table = NULL;
        vm->func_count = 0;
    }
    ht_init(&vm->modules_loaded);
    vm->env = environ_new(NULL, false);
    vm->laststatus = 0;
    vm->halted = false;
}

void vm_register_func(vm_t *vm, const char *name, size_t offset)
{
    vm->func_table = xrealloc(vm->func_table, ((size_t)vm->func_count + 1) * sizeof(vm_func_t));
    vm->func_table[vm->func_count].name = name;
    vm->func_table[vm->func_count].bytecode_offset = offset;
    vm->func_table[vm->func_count].image = NULL;
    vm->func_count++;
}

void vm_destroy(vm_t *vm)
{
    int i;
    for (i = 0; i < vm->stack_top; i++) {
        value_destroy(&vm->stack[i]);
    }
    vm->stack_top = 0;

    /* Free all scopes */
    while (vm->env != NULL) {
        environ_t *parent = vm->env->parent;
        environ_destroy(vm->env);
        vm->env = parent;
    }

    free(vm->func_table);
    vm->func_table = NULL;
    vm->func_count = 0;
    ht_destroy(&vm->modules_loaded);

    /* Free eval/source sub-images */
    {
        int ei;
        for (ei = 0; ei < vm->eval_image_count; ei++) {
            image_free(vm->eval_images[ei]);
        }
        free(vm->eval_images);
        vm->eval_images = NULL;
        vm->eval_image_count = 0;
    }

    free(vm->captured_stdout);
    vm->captured_stdout = NULL;

    /* Free trap handlers */
    {
        int ti;
        for (ti = 0; ti < 32; ti++) {
            free(vm->trap_handlers[ti]);
            vm->trap_handlers[ti] = NULL;
        }
        free(vm->exit_trap);
        vm->exit_trap = NULL;
    }
}

void vm_set_args(vm_t *vm, int argc, char **argv)
{
    int i;
    for (i = 0; i < argc; i++) {
        char name[16];
        snprintf(name, sizeof(name), "%d", i + 1);
        environ_set(vm->env, name, value_string(rcstr_new(argv[i])));
    }
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", argc);
        environ_set(vm->env, "#", value_string(rcstr_new(buf)));
    }
}

int vm_exec_string(vm_t *vm, const char *source, const char *label)
{
    parser_t p;
    parser_init(&p, source, label);
    sh_list_t *ast = parser_parse(&p);

    if (ast == NULL || parser_error_count(&p) > 0) {
        sh_list_free(ast);
        parser_destroy(&p);
        vm->laststatus = 2;
        return 1;
    }

    bytecode_image_t *img = compile(ast, label);
    sh_list_free(ast);
    parser_destroy(&p);

    if (img == NULL) {
        vm->laststatus = 2;
        return 1;
    }

    vm_t sub;
    vm_init(&sub, img);
    /* Share environment with caller. */
    environ_destroy(sub.env);
    sub.env = vm->env;

    /* Copy parent functions into the sub-VM so runtime name lookup
     * (OP_EXEC_SIMPLE) can find functions defined by earlier calls.
     * Skip any that collide with the sub-image's own compiled functions
     * (those are dispatched via OP_EXEC_FUNC by index and take priority). */
    {
        int pi;
        for (pi = 0; pi < vm->func_count; pi++) {
            /* Check if sub already has a function with this name */
            int conflict = 0;
            int si;
            for (si = 0; si < sub.func_count; si++) {
                if (strcmp(sub.func_table[si].name, vm->func_table[pi].name) == 0) {
                    conflict = 1;
                    break;
                }
            }
            if (!conflict) {
                sub.func_table = xrealloc(sub.func_table,
                                          ((size_t)sub.func_count + 1) * sizeof(vm_func_t));
                sub.func_table[sub.func_count].name = vm->func_table[pi].name;
                sub.func_table[sub.func_count].bytecode_offset = vm->func_table[pi].bytecode_offset;
                sub.func_table[sub.func_count].image = vm->func_table[pi].image;
                sub.func_count++;
            }
        }
    }

    int status = vm_run(&sub);

    /* After execution, merge new functions into the parent's table.
     * Tag them with the sub-image pointer for image-swap dispatch. */
    {
        int fi;
        bool has_new_funcs = false;
        for (fi = 0; fi < sub.func_count; fi++) {
            /* Functions inherited from the parent have a non-NULL image
             * pointer.  Only functions compiled in this image (image==NULL)
             * are genuinely new and need to be merged back. */
            if (sub.func_table[fi].image != NULL)
                continue;

            has_new_funcs = true;

            /* Check if this function already exists in the parent table */
            int existing = -1;
            int pi;
            for (pi = 0; pi < vm->func_count; pi++) {
                if (strcmp(vm->func_table[pi].name, sub.func_table[fi].name) == 0) {
                    existing = pi;
                    break;
                }
            }
            vm_func_t entry;
            entry.name = sub.func_table[fi].name;
            entry.bytecode_offset = sub.func_table[fi].bytecode_offset;
            entry.image = img;

            if (existing >= 0) {
                /* Overwrite existing function (redefinition) */
                vm->func_table[existing].bytecode_offset = entry.bytecode_offset;
                vm->func_table[existing].image = entry.image;
            } else {
                /* Append new function */
                vm->func_table =
                    xrealloc(vm->func_table, ((size_t)vm->func_count + 1) * sizeof(vm_func_t));
                vm->func_table[vm->func_count] = entry;
                vm->func_count++;
            }
        }

        if (has_new_funcs) {
            if (vm->eval_image_count >= vm->eval_image_cap) {
                int new_cap = vm->eval_image_cap ? vm->eval_image_cap * 2 : 4;
                vm->eval_images =
                    xrealloc(vm->eval_images, (size_t)new_cap * sizeof(bytecode_image_t *));
                vm->eval_image_cap = new_cap;
            }
            vm->eval_images[vm->eval_image_count++] = img;
            img = NULL; /* ownership transferred */
        }

        /* Transfer any sub-images from nested eval/source */
        if (sub.eval_image_count > 0) {
            int si;
            for (si = 0; si < sub.eval_image_count; si++) {
                if (vm->eval_image_count >= vm->eval_image_cap) {
                    int new_cap = vm->eval_image_cap ? vm->eval_image_cap * 2 : 4;
                    vm->eval_images =
                        xrealloc(vm->eval_images, (size_t)new_cap * sizeof(bytecode_image_t *));
                    vm->eval_image_cap = new_cap;
                }
                vm->eval_images[vm->eval_image_count++] = sub.eval_images[si];
            }
            sub.eval_image_count = 0;
        }
    }

    /* Propagate state back to the caller */
    vm->laststatus = sub.laststatus;
    if (sub.exit_requested) {
        vm->exit_requested = true;
        vm_exit(vm, sub.laststatus);
    }
    if (sub.return_requested) {
        vm->return_requested = true;
    }

    sub.env = NULL; /* don't double-free */
    vm_destroy(&sub);
    if (img != NULL) {
        image_free(img); /* no functions defined, safe to free */
    }

    return status;
}

void vm_exit(vm_t *vm, int status)
{
    vm->laststatus = status;

    /* Run EXIT trap if set. Clear it first to prevent recursion
     * if the trap body calls exit. */
    if (vm->exit_trap != NULL && vm->exit_trap[0] != '\0') {
        char *trap_cmd = vm->exit_trap;
        vm->exit_trap = NULL; /* prevent re-entry */
        vm_exec_string(vm, trap_cmd, "EXIT trap");
        free(trap_cmd);
    }

    vm->laststatus = status;
    vm->halted = true;
}

void vm_push(vm_t *vm, value_t val)
{
    if (vm->stack_top >= VM_STACK_MAX) {
        fprintf(stderr, "opsh: operand stack overflow\n");
        abort();
    }
    vm->stack[vm->stack_top++] = val;
}

value_t vm_pop(vm_t *vm)
{
    if (vm->stack_top <= 0) {
        fprintf(stderr, "opsh: operand stack underflow\n");
        abort();
    }
    return vm->stack[--vm->stack_top];
}

value_t vm_peek(vm_t *vm, int offset)
{
    int idx = vm->stack_top - 1 - offset;
    if (idx < 0 || idx >= vm->stack_top) {
        fprintf(stderr, "opsh: operand stack peek out of bounds\n");
        abort();
    }
    return vm->stack[idx];
}

static uint8_t read_u8(vm_t *vm)
{
    if (vm->ip >= vm->image->code_size) {
        fprintf(stderr, "opsh: bytecode read past end\n");
        abort();
    }
    return vm->image->code[vm->ip++];
}

static uint16_t read_u16(vm_t *vm)
{
    uint16_t lo = read_u8(vm);
    uint16_t hi = read_u8(vm);
    return lo | (hi << 8);
}

static int32_t read_i32(vm_t *vm)
{
    uint32_t b0 = read_u8(vm);
    uint32_t b1 = read_u8(vm);
    uint32_t b2 = read_u8(vm);
    uint32_t b3 = read_u8(vm);
    return (int32_t)(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
}

static void vm_jump(vm_t *vm, int32_t offset)
{
    int64_t target = (int64_t)vm->ip + offset;
    if (target < 0 || (size_t)target > vm->image->code_size) {
        fprintf(stderr, "opsh: jump target %lld out of bounds (code size %zu)\n", (long long)target,
                vm->image->code_size);
        vm->laststatus = 1;
        vm->halted = true;
        return;
    }
    vm->ip = (size_t)target;
}

/*
 * Dispatch loop
 */
int vm_run(vm_t *vm)
{
    while (!vm->halted && vm->ip < vm->image->code_size) {
        /* Safe point: check for pending signals */
        if (signal_pending()) {
            int sig = signal_get_pending();
            signal_clear();
            if (sig > 0 && sig < 32 && vm->trap_handlers[sig] != NULL) {
                /* Execute the trap handler */
                char *handler = vm->trap_handlers[sig];
                if (handler[0] != '\0') {
                    vm_exec_string(vm, handler, "trap");
                }
                /* Empty handler ("") means ignore the signal */
            } else if (sig == SIGINT || sig == SIGTERM) {
                /* Default behavior: halt */
                vm->laststatus = 128 + sig;
                vm->halted = true;
                break;
            }
        }

        uint8_t op = read_u8(vm);

        switch ((opcode_t)op) {
        case OP_PUSH_CONST: {
            uint16_t idx = read_u16(vm);
            if (idx >= vm->image->const_count) {
                fprintf(stderr, "opsh: constant pool index %u out of range\n", idx);
                vm->laststatus = 1;
                vm->halted = true;
                break;
            }
            char *s = rcstr_retain(vm->image->const_pool[idx]);
            vm_push(vm, value_string(s));
            break;
        }

        case OP_PUSH_INT: {
            int32_t val = read_i32(vm);
            vm_push(vm, value_integer(val));
            break;
        }

        case OP_POP: {
            value_t v = vm_pop(vm);
            value_destroy(&v);
            break;
        }

        case OP_DUP: {
            value_t top = vm_peek(vm, 0);
            vm_push(vm, value_clone(&top));
            break;
        }

        case OP_CONCAT: {
            uint16_t count = read_u16(vm);
            strbuf_t result;
            int i;
            strbuf_init(&result);

            /* Pop `count` values and concatenate in order */
            value_t *vals = xmalloc((size_t)count * sizeof(value_t));
            for (i = count - 1; i >= 0; i--) {
                vals[i] = vm_pop(vm);
            }
            for (i = 0; i < (int)count; i++) {
                char *s = value_to_string(&vals[i]);
                strbuf_append_str(&result, s);
                rcstr_release(s);
                value_destroy(&vals[i]);
            }
            free(vals);

            vm_push(vm, value_string(rcstr_from_buf(result.contents, result.length)));
            strbuf_destroy(&result);
            break;
        }

        case OP_GET_SPECIAL: {
            uint8_t which = read_u8(vm);
            switch ((special_var_t)which) {
            case SPECIAL_QUESTION: {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", vm->laststatus);
                vm_push(vm, value_string(rcstr_new(buf)));
                break;
            }
            case SPECIAL_HASH: {
                variable_t *hash_var = environ_get(vm->env, "#");
                if (hash_var != NULL && hash_var->value.type == VT_STRING) {
                    vm_push(vm, value_string(rcstr_retain(hash_var->value.data.string)));
                } else {
                    vm_push(vm, value_string(rcstr_new("0")));
                }
                break;
            }
            case SPECIAL_AT: {
                variable_t *hash_var = environ_get(vm->env, "#");
                int count = 0;
                if (hash_var != NULL) {
                    count = (int)value_to_integer(&hash_var->value);
                }
                strbuf_t result;
                strbuf_init(&result);
                int pi;
                for (pi = 1; pi <= count; pi++) {
                    char name[16];
                    snprintf(name, sizeof(name), "%d", pi);
                    variable_t *pv = environ_get(vm->env, name);
                    if (pv != NULL) {
                        if (result.length > 0) {
                            strbuf_append_byte(&result, ' ');
                        }
                        char *s = value_to_string(&pv->value);
                        strbuf_append_str(&result, s);
                        rcstr_release(s);
                    }
                }
                {
                    char *tmp = strbuf_detach(&result);
                    vm_push(vm, value_string(rcstr_new(tmp)));
                    free(tmp);
                }
                break;
            }
            case SPECIAL_STAR: {
                variable_t *hash_var = environ_get(vm->env, "#");
                int count = 0;
                if (hash_var != NULL) {
                    count = (int)value_to_integer(&hash_var->value);
                }
                char sep = ' ';
                variable_t *ifs_var = environ_get(vm->env, "IFS");
                if (ifs_var != NULL && ifs_var->value.type == VT_STRING) {
                    if (ifs_var->value.data.string[0] != '\0') {
                        sep = ifs_var->value.data.string[0];
                    } else {
                        sep = '\0';
                    }
                }
                strbuf_t result;
                strbuf_init(&result);
                int pi;
                for (pi = 1; pi <= count; pi++) {
                    char name[16];
                    snprintf(name, sizeof(name), "%d", pi);
                    variable_t *pv = environ_get(vm->env, name);
                    if (pv != NULL) {
                        if (result.length > 0 && sep != '\0') {
                            strbuf_append_byte(&result, sep);
                        }
                        char *s = value_to_string(&pv->value);
                        strbuf_append_str(&result, s);
                        rcstr_release(s);
                    }
                }
                {
                    char *tmp = strbuf_detach(&result);
                    vm_push(vm, value_string(rcstr_new(tmp)));
                    free(tmp);
                }
                break;
            }
            case SPECIAL_DOLLAR: {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d", (int)getpid());
                vm_push(vm, value_string(rcstr_new(buf)));
                break;
            }
            case SPECIAL_BANG: {
                if (vm->last_bg_pid > 0) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%d", (int)vm->last_bg_pid);
                    vm_push(vm, value_string(rcstr_new(buf)));
                } else {
                    vm_push(vm, value_string(rcstr_new("")));
                }
                break;
            }
            case SPECIAL_DASH: {
                vm_push(vm, value_string(rcstr_new(vm->option_flags)));
                break;
            }
            case SPECIAL_ZERO: {
                const char *name = vm->script_name ? vm->script_name : "opsh";
                vm_push(vm, value_string(rcstr_new(name)));
                break;
            }
            default:
                vm_push(vm, value_string(rcstr_new("")));
                break;
            }
            break;
        }

        case OP_EXPAND_PARAM: {
            uint16_t name_idx = read_u16(vm);
            uint8_t op = read_u8(vm);
            uint8_t flags = read_u8(vm);
            const char *name = vm->image->const_pool[name_idx];

            /* ${#var} -- string length */
            if (flags & PE_STRLEN) {
                variable_t *var = environ_get(vm->env, name);
                if (var != NULL && var->value.type == VT_ARRAY) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%d", var->value.data.array.count);
                    vm_push(vm, value_string(rcstr_new(buf)));
                } else if (var != NULL && var->value.type == VT_STRING) {
                    size_t len = utf8_strlen(var->value.data.string);
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%zu", len);
                    vm_push(vm, value_string(rcstr_new(buf)));
                } else {
                    vm_push(vm, value_string(rcstr_new("0")));
                }
                break;
            }

            variable_t *var = environ_get(vm->env, name);
            char *var_val = NULL;
            bool is_set = (var != NULL && var->value.type != VT_NONE);
            bool is_empty =
                !is_set || (var->value.type == VT_STRING && var->value.data.string[0] == '\0');
            bool use_colon = (flags & PE_COLON) != 0;
            /* With colon: check set AND non-empty. Without: check set only. */
            bool condition = use_colon ? (is_set && !is_empty) : is_set;

            if (is_set) {
                var_val = value_to_string(&var->value);
            }

            switch ((param_exp_type_t)op) {
            case PE_NONE: {
                /* Simple ${var} */
                if (var_val != NULL) {
                    vm_push(vm, value_string(var_val));
                } else {
                    vm_push(vm, value_string(rcstr_new("")));
                }
                break;
            }
            case PE_DEFAULT: {
                /* ${var:-word} or ${var-word} */
                value_t word = vm_pop(vm);
                if (condition) {
                    value_destroy(&word);
                    vm_push(vm, value_string(var_val));
                } else {
                    rcstr_release(var_val);
                    vm_push(vm, word);
                }
                break;
            }
            case PE_ALTERNATE: {
                /* ${var:+word} or ${var+word} */
                value_t word = vm_pop(vm);
                if (condition) {
                    rcstr_release(var_val);
                    vm_push(vm, word);
                } else {
                    value_destroy(&word);
                    if (var_val != NULL) {
                        vm_push(vm, value_string(var_val));
                    } else {
                        vm_push(vm, value_string(rcstr_new("")));
                    }
                }
                break;
            }
            case PE_ASSIGN: {
                /* ${var:=word} or ${var=word} */
                value_t word = vm_pop(vm);
                if (condition) {
                    value_destroy(&word);
                    vm_push(vm, value_string(var_val));
                } else {
                    rcstr_release(var_val);
                    char *assigned = value_to_string(&word);
                    environ_set(vm->env, name, word);
                    vm_push(vm, value_string(assigned));
                }
                break;
            }
            case PE_ERROR: {
                /* ${var:?word} or ${var?word} */
                value_t word = vm_pop(vm);
                if (condition) {
                    value_destroy(&word);
                    vm_push(vm, value_string(var_val));
                } else {
                    rcstr_release(var_val);
                    char *msg = value_to_string(&word);
                    fprintf(stderr, "opsh: %s: %s\n", name, msg);
                    rcstr_release(msg);
                    value_destroy(&word);
                    vm->laststatus = 1;
                    vm->halted = true;
                    vm_push(vm, value_string(rcstr_new("")));
                }
                break;
            }
            case PE_TRIM: {
                /* ${var#pat}, ${var##pat}, ${var%pat}, ${var%%pat} */
                value_t pattern_v = vm_pop(vm);
                char *pat = value_to_string(&pattern_v);
                value_destroy(&pattern_v);

                if (var_val == NULL) {
                    rcstr_release(pat);
                    vm_push(vm, value_string(rcstr_new("")));
                    break;
                }

                size_t len = strlen(var_val);
                bool match_head = (flags & PE_PREFIX) != 0;
                bool longest = (flags & PE_LONGEST) != 0;
                char *result = NULL;

                if (match_head) {
                    /* Prefix match: truncate at position i, test var[0..i] */
                    size_t best = 0;
                    bool found = false;
                    size_t i;
                    for (i = 0; i <= len; i++) {
                        char saved = var_val[i];
                        var_val[i] = '\0';
                        if (fnmatch(pat, var_val, 0) == 0) {
                            best = i;
                            found = true;
                            var_val[i] = saved;
                            if (!longest) {
                                break;
                            }
                        }
                        var_val[i] = saved;
                    }
                    result = found ? rcstr_new(var_val + best) : rcstr_new(var_val);
                } else {
                    /* Suffix match: test var[i..] */
                    size_t best = len;
                    bool found = false;
                    size_t i;
                    for (i = len;; i--) {
                        if (fnmatch(pat, var_val + i, 0) == 0) {
                            best = i;
                            found = true;
                            if (!longest) {
                                break;
                            }
                        }
                        if (i == 0) {
                            break;
                        }
                    }
                    if (found) {
                        char tmp[best + 1];
                        memcpy(tmp, var_val, best);
                        tmp[best] = '\0';
                        result = rcstr_new(tmp);
                    } else {
                        result = rcstr_new(var_val);
                    }
                }

                rcstr_release(var_val);
                rcstr_release(pat);
                vm_push(vm, value_string(result));
                break;
            }
            case PE_REPLACE: {
                /* ${var/pat/rep} or ${var//pat/rep} */
                value_t rep_v = vm_pop(vm);
                value_t pat_v = vm_pop(vm);
                char *rep = value_to_string(&rep_v);
                char *pat = value_to_string(&pat_v);
                value_destroy(&rep_v);
                value_destroy(&pat_v);

                if (var_val == NULL) {
                    rcstr_release(pat);
                    rcstr_release(rep);
                    vm_push(vm, value_string(rcstr_new("")));
                    break;
                }

                bool replace_all = (flags & PE_GLOBAL) != 0;
                bool anchor_head = (flags & PE_PREFIX) != 0;
                bool anchor_tail = (flags & PE_SUFFIX) != 0;
                size_t len = strlen(var_val);
                strbuf_t result;
                strbuf_init(&result);
                size_t pos = 0;

                /* For tail anchor, try matching suffix at each start position */
                if (anchor_tail) {
                    size_t best_start = len;
                    size_t ti;
                    for (ti = 0; ti <= len; ti++) {
                        if (fnmatch(pat, var_val + ti, 0) == 0) {
                            best_start = ti;
                            break; /* first (longest) suffix match */
                        }
                    }
                    if (best_start < len) {
                        strbuf_append_bytes(&result, var_val, best_start);
                        strbuf_append_str(&result, rep);
                    } else {
                        strbuf_append_str(&result, var_val);
                    }
                    free(var_val);
                    free(pat);
                    free(rep);
                    vm_push(vm, value_string(strbuf_detach(&result)));
                    break;
                }

                while (pos <= len) {
                    /* Head anchor: only match at position 0 */
                    if (anchor_head && pos != 0) {
                        strbuf_append_str(&result, var_val + pos);
                        break;
                    }

                    /* At each position, find the longest match */
                    bool matched = false;
                    size_t match_end = pos;
                    size_t j;
                    for (j = pos; j <= len; j++) {
                        char saved = var_val[j];
                        var_val[j] = '\0';
                        if (fnmatch(pat, var_val + pos, 0) == 0) {
                            match_end = j;
                            matched = true;
                        }
                        var_val[j] = saved;
                    }

                    if (matched) {
                        strbuf_append_str(&result, rep);
                        if (match_end == pos) {
                            /* Zero-length match: advance past one char */
                            if (pos < len) {
                                strbuf_append_byte(&result, var_val[pos]);
                            }
                            pos++;
                        } else {
                            pos = match_end;
                        }
                        /* Stop if we've consumed the entire string */
                        if (pos >= len && match_end >= len) {
                            break;
                        }
                        if (!replace_all) {
                            /* Append remainder */
                            strbuf_append_str(&result, var_val + pos);
                            pos = len + 1;
                        }
                    } else {
                        if (pos < len) {
                            strbuf_append_byte(&result, var_val[pos]);
                        }
                        pos++;
                    }
                }

                rcstr_release(var_val);
                rcstr_release(pat);
                rcstr_release(rep);
                {
                    char *tmp = strbuf_detach(&result);
                    vm_push(vm, value_string(rcstr_new(tmp)));
                    free(tmp);
                }
                break;
            }
            }
            break;
        }

        case OP_EXPAND_ARITH: {
            value_t expr_val = vm_pop(vm);
            char *expr_str = value_to_string(&expr_val);
            value_destroy(&expr_val);

            arith_error_t arith_err = ARITH_OK;
            int64_t result = arith_eval(expr_str, vm->env, &arith_err);

            if (arith_err != ARITH_OK) {
                const char *msg = "arithmetic syntax error";
                if (arith_err == ARITH_ERR_DIV_ZERO) {
                    msg = "division by zero";
                } else if (arith_err == ARITH_ERR_DEPTH) {
                    msg = "arithmetic recursion too deep";
                }
                /* Strip trailing ) left by the $((...)) lexer */
                size_t elen = strlen(expr_str);
                if (elen > 0 && expr_str[elen - 1] == ')') {
                    expr_str[elen - 1] = '\0';
                }
                fprintf(stderr, "opsh: %s: %s\n", msg, expr_str);
                vm->laststatus = 1;
                vm->halted = true;
            }

            rcstr_release(expr_str);

            char buf[32];
            snprintf(buf, sizeof(buf), "%" PRId64, result);
            vm_push(vm, value_string(rcstr_new(buf)));
            break;
        }

        case OP_SPLIT_FIELDS: {
            value_t val = vm_pop(vm);
            char *str = value_to_string(&val);
            value_destroy(&val);

            /* Get IFS (default: space, tab, newline) */
            const char *ifs = " \t\n";
            variable_t *ifs_var = environ_get(vm->env, "IFS");
            if (ifs_var != NULL && ifs_var->value.type == VT_STRING) {
                ifs = ifs_var->value.data.string;
            }

            /* If IFS is empty, no splitting -- push as single field */
            if (ifs[0] == '\0') {
                vm_push(vm, value_string(str));
                vm_push(vm, value_integer(1));
                break;
            }

            /* Split the string by IFS characters */
            {
                int field_count = 0;
                const char *p = str;

                /* Skip leading IFS whitespace */
                while (*p && strchr(ifs, *p) != NULL && (*p == ' ' || *p == '\t' || *p == '\n')) {
                    p++;
                }

                while (*p) {
                    const char *start = p;
                    /* Find next IFS character */
                    while (*p && strchr(ifs, *p) == NULL) {
                        p++;
                    }

                    size_t len = (size_t)(p - start);
                    char tmp[len + 1];
                    memcpy(tmp, start, len);
                    tmp[len] = '\0';
                    vm_push(vm, value_string(rcstr_new(tmp)));
                    field_count++;

                    /* Skip IFS delimiters */
                    if (*p) {
                        bool saw_nonws_delim = false;
                        /* Skip one non-whitespace IFS delimiter */
                        if (strchr(ifs, *p) != NULL && *p != ' ' && *p != '\t' && *p != '\n') {
                            saw_nonws_delim = true;
                            p++;
                        }
                        /* Skip IFS whitespace */
                        while (*p && strchr(ifs, *p) != NULL &&
                               (*p == ' ' || *p == '\t' || *p == '\n')) {
                            p++;
                        }
                        /* Trailing non-whitespace delimiter produces empty field */
                        if (saw_nonws_delim && *p == '\0') {
                            vm_push(vm, value_string(rcstr_new("")));
                            field_count++;
                        }
                    }
                }

                /* Empty string produces zero fields */
                if (field_count == 0 && str[0] == '\0') {
                    /* Push nothing, count = 0 */
                }

                rcstr_release(str);
                vm_push(vm, value_integer(field_count));
            }
            break;
        }

        case OP_GLOB: {
            /* Pop pattern, expand glob, push matches... count.
             * If no matches, push the pattern itself with count=1. */
            value_t pattern_val = vm_pop(vm);
            char *pattern = value_to_string(&pattern_val);
            value_destroy(&pattern_val);

            /* Check if pattern contains glob characters */
            bool has_glob = false;
            {
                const char *p = pattern;
                while (*p) {
                    if (*p == '*' || *p == '?' || *p == '[') {
                        has_glob = true;
                        break;
                    }
                    p++;
                }
            }

            if (!has_glob) {
                /* No glob chars -- push as-is, count=1 */
                vm_push(vm, value_string(pattern));
                vm_push(vm, value_integer(1));
                break;
            }

            /* Simple glob: use the glob(3) function */
            {
                glob_t gl;
                int ret = glob(pattern, GLOB_NOSORT, NULL, &gl);
                if (ret == 0 && gl.gl_pathc > 0) {
                    size_t gi;
                    for (gi = 0; gi < gl.gl_pathc; gi++) {
                        vm_push(vm, value_string(rcstr_new(gl.gl_pathv[gi])));
                    }
                    vm_push(vm, value_integer((int64_t)gl.gl_pathc));
                    globfree(&gl);
                } else {
                    /* No matches -- return pattern literally */
                    if (ret == 0) {
                        globfree(&gl);
                    }
                    vm_push(vm, value_string(pattern));
                    pattern = NULL; /* ownership transferred */
                    vm_push(vm, value_integer(1));
                }
            }

            rcstr_release(pattern);
            break;
        }

        case OP_COLLECT_WORDS: {
            /* Pop group_count, then for each group pop (count, values...).
             * Flatten into (args... argc). */
            value_t ngroups_val = vm_pop(vm);
            int ngroups = (int)value_to_integer(&ngroups_val);
            value_destroy(&ngroups_val);

            /* Collect all groups in reverse order */
            int total_args = 0;
            int gi;

            /* First pass: pop all groups and their values into a temp array */
            typedef struct {
                value_t *values;
                int count;
            } word_group;
            word_group *groups = xcalloc(ngroups ? (size_t)ngroups : 1, sizeof(word_group));

            for (gi = ngroups - 1; gi >= 0; gi--) {
                value_t count_val = vm_pop(vm);
                int count = (int)value_to_integer(&count_val);
                value_destroy(&count_val);
                groups[gi].count = count;
                groups[gi].values = xcalloc(count ? (size_t)count : 1, sizeof(value_t));
                int vi;
                for (vi = count - 1; vi >= 0; vi--) {
                    groups[gi].values[vi] = vm_pop(vm);
                }
                total_args += count;
            }

            /* Second pass: push all values in order, then argc */
            for (gi = 0; gi < ngroups; gi++) {
                int vi;
                for (vi = 0; vi < groups[gi].count; vi++) {
                    vm_push(vm, groups[gi].values[vi]);
                }
                free(groups[gi].values);
            }
            free(groups);

            vm_push(vm, value_integer(total_args));
            break;
        }

        case OP_EXPAND_ARGS: {
            /* Push each positional parameter as a separate value, then count.
             * Produces the same stack layout as SPLIT_FIELDS but preserves
             * word boundaries for "$@". */
            variable_t *hash_var = environ_get(vm->env, "#");
            int count = 0;
            if (hash_var != NULL) {
                count = (int)value_to_integer(&hash_var->value);
            }
            int pi;
            for (pi = 1; pi <= count; pi++) {
                char name[16];
                snprintf(name, sizeof(name), "%d", pi);
                variable_t *pv = environ_get(vm->env, name);
                if (pv != NULL) {
                    vm_push(vm, value_clone(&pv->value));
                } else {
                    vm_push(vm, value_string(rcstr_new("")));
                }
            }
            vm_push(vm, value_integer(count));
            break;
        }

        case OP_QUOTE_REMOVE:
            break;

        case OP_EXPAND_TILDE: {
            value_t v = vm_pop(vm);
            char *s = value_to_string(&v);
            value_destroy(&v);
            const char *home = getenv("HOME");

            /* ~user expansion */
            if (s[0] == '~' && s[1] != '/' && s[1] != '\0' && s[1] != ':') {
                const char *end = s + 1;
                while (*end && *end != '/' && *end != ':') {
                    end++;
                }
                size_t ulen = (size_t)(end - s - 1);
                char *username = xmalloc(ulen + 1);
                memcpy(username, s + 1, ulen);
                username[ulen] = '\0';
                struct passwd *pw = getpwnam(username);
                free(username);
                if (pw != NULL) {
                    strbuf_t result;
                    strbuf_init(&result);
                    strbuf_append_str(&result, pw->pw_dir);
                    strbuf_append_str(&result, end);
                    free(s);
                    vm_push(vm, value_string(strbuf_detach(&result)));
                } else {
                    vm_push(vm, value_string(s));
                }
                break;
            }

            if (s[0] == '~' && (s[1] == '/' || s[1] == '\0' || s[1] == ':') && home != NULL) {
                strbuf_t result;
                strbuf_init(&result);
                /* Expand leading tilde */
                strbuf_append_str(&result, home);
                /* Process rest of string, expanding ~ after : */
                const char *p = s + 1;
                while (*p) {
                    if (*p == ':' && p[1] == '~' && (p[2] == '/' || p[2] == '\0' || p[2] == ':')) {
                        strbuf_append_byte(&result, ':');
                        strbuf_append_str(&result, home);
                        p += 2;
                    } else {
                        strbuf_append_byte(&result, *p);
                        p++;
                    }
                }
                rcstr_release(s);
                {
                    char *tmp = strbuf_detach(&result);
                    vm_push(vm, value_string(rcstr_new(tmp)));
                    free(tmp);
                }
            } else {
                vm_push(vm, value_string(s));
            }
            break;
        }

        case OP_JMP: {
            int32_t offset = read_i32(vm);
            vm_jump(vm, offset);
            break;
        }

        case OP_JMP_TRUE: {
            int32_t offset = read_i32(vm);
            if (vm->laststatus == 0) {
                vm_jump(vm, offset);
            }
            break;
        }

        case OP_JMP_FALSE: {
            int32_t offset = read_i32(vm);
            if (vm->laststatus != 0) {
                vm_jump(vm, offset);
            }
            break;
        }

        case OP_JMP_NONE: {
            int32_t offset = read_i32(vm);
            value_t v = vm_pop(vm);
            bool is_none = (v.type == VT_NONE);
            value_destroy(&v);
            if (is_none) {
                vm_jump(vm, offset);
            }
            break;
        }

        case OP_INIT_ITER: {
            uint16_t group_count = read_u16(vm);
            size_t total = 0;
            int gi, ei;

            /* First pass: count total elements */
            /* Each group is (field_N...field_0, count) on the stack */
            /* We have group_count groups; pop them all */
            int *group_sizes = xcalloc(group_count ? group_count : 1, sizeof(int));

            for (gi = group_count - 1; gi >= 0; gi--) {
                value_t count_val = vm_pop(vm);
                int64_t gs = value_to_integer(&count_val);
                value_destroy(&count_val);
                if (gs < 0 || gs > VM_STACK_MAX) {
                    fprintf(stderr, "opsh: INIT_ITER: invalid group size %lld\n", (long long)gs);
                    free(group_sizes);
                    vm->laststatus = 1;
                    vm->halted = true;
                    break;
                }
                group_sizes[gi] = (int)gs;
                total += (size_t)gs;
            }
            if (vm->halted) {
                break;
            }

            /* Now pop all the fields */
            char **elements = xcalloc(total ? total : 1, sizeof(char *));
            size_t pos = total;
            for (gi = group_count - 1; gi >= 0; gi--) {
                for (ei = group_sizes[gi] - 1; ei >= 0; ei--) {
                    value_t field = vm_pop(vm);
                    pos--;
                    elements[pos] = value_to_string(&field);
                    value_destroy(&field);
                }
            }

            free(group_sizes);

            value_t iter;
            iter.type = VT_ITERATOR;
            iter.data.iterator.elements = elements;
            iter.data.iterator.count = (int)total;
            iter.data.iterator.position = 0;
            vm_push(vm, iter);
            break;
        }

        case OP_GET_NEXT_ITER: {
            /* Peek at top of stack -- should be a VT_ITERATOR */
            if (vm->stack_top <= 0 || vm->stack[vm->stack_top - 1].type != VT_ITERATOR) {
                fprintf(stderr, "opsh: GET_NEXT_ITER: no iterator on stack\n");
                vm->laststatus = 1;
                vm->halted = true;
                break;
            }
            iterator_t *it = &vm->stack[vm->stack_top - 1].data.iterator;
            if (it->position >= it->count) {
                vm_push(vm, value_none());
            } else {
                char *s = rcstr_new(it->elements[it->position]);
                it->position++;
                vm_push(vm, value_string(s));
            }
            break;
        }

        case OP_GET_VAR: {
            uint16_t name_idx = read_u16(vm);
            const char *name = vm->image->const_pool[name_idx];
            variable_t *var = environ_get(vm->env, name);
            if (var != NULL) {
                vm_push(vm, value_clone(&var->value));
            } else if (vm->opt_nounset) {
                fprintf(stderr, "opsh: %s: unbound variable\n", name);
                vm->laststatus = 1;
                vm->halted = true;
                vm_push(vm, value_string(rcstr_new("")));
            } else {
                /* Unset variable: push empty string */
                vm_push(vm, value_string(rcstr_new("")));
            }
            break;
        }

        case OP_GET_LOCAL: {
            uint16_t name_idx = read_u16(vm);
            const char *name = vm->image->const_pool[name_idx];
            variable_t *var = ht_get(&vm->env->vars, name);
            if (var != NULL) {
                vm_push(vm, value_clone(&var->value));
            } else {
                /* Fall back to full chain walk */
                var = environ_get(vm->env, name);
                if (var != NULL) {
                    vm_push(vm, value_clone(&var->value));
                } else {
                    vm_push(vm, value_string(rcstr_new("")));
                }
            }
            break;
        }

        case OP_SET_VAR: {
            uint16_t name_idx = read_u16(vm);
            const char *name = vm->image->const_pool[name_idx];
            value_t val = vm_pop(vm);
            environ_assign(vm->env, name, val);
            break;
        }

        case OP_SET_LOCAL: {
            uint16_t name_idx = read_u16(vm);
            const char *name = vm->image->const_pool[name_idx];
            value_t val = vm_pop(vm);
            environ_set(vm->env, name, val);
            break;
        }

        case OP_PUSH_SCOPE: {
            uint8_t flags = read_u8(vm);
            bool is_temp = (flags & 1) != 0;
            vm->env = environ_new(vm->env, is_temp);
            break;
        }

        case OP_POP_SCOPE: {
            if (vm->env->parent == NULL) {
                fprintf(stderr, "opsh: cannot pop global scope\n");
                vm->laststatus = 1;
                vm->halted = true;
                break;
            }
            environ_t *old = vm->env;
            vm->env = old->parent;
            environ_destroy(old);
            break;
        }

        case OP_EXPORT: {
            uint16_t name_idx = read_u16(vm);
            const char *name = vm->image->const_pool[name_idx];
            environ_export(vm->env, name);
            break;
        }

        case OP_GET_ARRAY: {
            uint16_t name_idx = read_u16(vm);
            const char *name = vm->image->const_pool[name_idx];
            value_t idx_val = vm_pop(vm);
            int idx = (int)value_to_integer(&idx_val);
            value_destroy(&idx_val);
            variable_t *var = environ_get(vm->env, name);
            if (var != NULL && var->value.type == VT_ARRAY && idx >= 0 &&
                idx < var->value.data.array.count) {
                char *s = rcstr_retain(var->value.data.array.elements[idx]);
                vm_push(vm, value_string(s));
            } else {
                vm_push(vm, value_string(rcstr_new("")));
            }
            break;
        }

        case OP_SET_ARRAY: {
            uint16_t name_idx = read_u16(vm);
            const char *name = vm->image->const_pool[name_idx];
            value_t idx_val = vm_pop(vm);
            value_t val = vm_pop(vm);
            int idx = (int)value_to_integer(&idx_val);
            value_destroy(&idx_val);
            char *str = value_to_string(&val);
            value_destroy(&val);

            variable_t *var = environ_get(vm->env, name);
            if (var != NULL && var->value.type == VT_ARRAY) {
                /* Extend if needed */
                if (idx >= var->value.data.array.count) {
                    int new_count = idx + 1;
                    var->value.data.array.elements = xrealloc(var->value.data.array.elements,
                                                              (size_t)new_count * sizeof(char *));
                    int ai;
                    for (ai = var->value.data.array.count; ai < new_count; ai++) {
                        var->value.data.array.elements[ai] = rcstr_new("");
                    }
                    var->value.data.array.count = new_count;
                }
                if (idx >= 0) {
                    rcstr_release(var->value.data.array.elements[idx]);
                    var->value.data.array.elements[idx] = str;
                } else {
                    rcstr_release(str);
                }
            } else {
                /* Create new array */
                int new_count = idx + 1;
                if (new_count < 1) {
                    new_count = 1;
                }
                char **elements = xcalloc((size_t)new_count, sizeof(char *));
                int ai;
                for (ai = 0; ai < new_count; ai++) {
                    elements[ai] = rcstr_new("");
                }
                if (idx >= 0 && idx < new_count) {
                    rcstr_release(elements[idx]);
                    elements[idx] = str;
                } else {
                    rcstr_release(str);
                }
                value_t arr;
                arr.type = VT_ARRAY;
                arr.data.array.elements = elements;
                arr.data.array.count = new_count;
                environ_assign(vm->env, name, arr);
            }
            break;
        }

        case OP_SET_ARRAY_BULK: {
            uint16_t name_idx = read_u16(vm);
            uint16_t count = read_u16(vm);
            const char *name = vm->image->const_pool[name_idx];

            char **elements = xcalloc(count ? (size_t)count : 1, sizeof(char *));
            int ai;
            for (ai = (int)count - 1; ai >= 0; ai--) {
                value_t v = vm_pop(vm);
                elements[ai] = value_to_string(&v);
                value_destroy(&v);
            }

            value_t arr;
            arr.type = VT_ARRAY;
            arr.data.array.elements = elements;
            arr.data.array.count = (int)count;
            environ_assign(vm->env, name, arr);
            break;
        }

        case OP_EXEC_BUILTIN: {
            uint16_t builtin_idx = read_u16(vm);

            /* Pop argc from stack */
            value_t argc_val = vm_pop(vm);
            int argc = (int)value_to_integer(&argc_val);
            value_destroy(&argc_val);

            /* Pop argc arguments */
            value_t *argv = xcalloc(argc ? (size_t)argc : 1, sizeof(value_t));
            int i;
            for (i = argc - 1; i >= 0; i--) {
                argv[i] = vm_pop(vm);
            }

            /* xtrace: print command before execution */
            if (vm->opt_xtrace) {
                const char *ps4 = "+ ";
                variable_t *ps4_var = environ_get(vm->env, "PS4");
                if (ps4_var != NULL && ps4_var->value.type == VT_STRING) {
                    ps4 = ps4_var->value.data.string;
                }
                fprintf(stderr, "%s", ps4);
                for (i = 0; i < argc; i++) {
                    char *s = value_to_string(&argv[i]);
                    if (i > 0) {
                        fputc(' ', stderr);
                    }
                    fprintf(stderr, "%s", s);
                    free(s);
                }
                fputc('\n', stderr);
            }

            if (builtin_idx < (uint16_t)builtin_count() && builtin_table[builtin_idx].fn != NULL) {
                int64_t cmd_id = vm->next_command_id++;
                {
                    event_t ev = {0};
                    ev.type = EVENT_COMMAND_START;
                    ev.id = cmd_id;
                    ev.name = builtin_table[builtin_idx].name;
                    event_emit(vm->event_sink, &ev);
                }
                int status = builtin_table[builtin_idx].fn(vm, argc, argv);
                vm->laststatus = status;
                {
                    event_t ev = {0};
                    ev.type = EVENT_COMMAND_END;
                    ev.id = cmd_id;
                    ev.status = status;
                    event_emit(vm->event_sink, &ev);
                }
                /* Special cases */
                if (strcmp(builtin_table[builtin_idx].name, "exit") == 0) {
                    vm->exit_requested = true;
                    vm_exit(vm, status);
                }
                /* return builtin triggers function return */
                if (vm->return_requested) {
                    if (vm->call_depth > 0) {
                        vm->return_requested = false;
                        call_frame_t *frame = &vm->call_stack[--vm->call_depth];
                        while (vm->stack_top > frame->saved_stack_base) {
                            value_t v = vm_pop(vm);
                            value_destroy(&v);
                        }
                        vm->loop_depth = frame->saved_loop_depth;
                        environ_t *func_env = vm->env;
                        vm->env = func_env->parent;
                        environ_destroy(func_env);
                        vm->ip = frame->return_ip;
                        vm->image = frame->saved_image;
                    } else {
                        /* At top level: halt but keep return_requested
                         * so vm_exec_string can propagate it */
                        vm->halted = true;
                    }
                }
            } else {
                fprintf(stderr, "opsh: unknown builtin index %u\n", builtin_idx);
                vm->laststatus = 127;
            }

            for (i = 0; i < argc; i++) {
                value_destroy(&argv[i]);
            }
            free(argv);
            break;
        }

        case OP_EXEC_SIMPLE: {
            uint8_t flags = read_u8(vm);
            (void)flags;

            /* Pop argc and arguments */
            value_t argc_val = vm_pop(vm);
            int argc = (int)value_to_integer(&argc_val);
            value_destroy(&argc_val);

            value_t *argv = xcalloc(argc ? (size_t)argc : 1, sizeof(value_t));
            int i;
            for (i = argc - 1; i >= 0; i--) {
                argv[i] = vm_pop(vm);
            }

            /* Emit commandStart event */
            int64_t exec_cmd_id = vm->next_command_id++;
            char *exec_cmd_name = (argc > 0) ? value_to_string(&argv[0]) : NULL;
            bool exec_is_func = false; /* suppress commandEnd for async func dispatch */
            {
                event_t ev = {0};
                ev.type = EVENT_COMMAND_START;
                ev.id = exec_cmd_id;
                ev.name = exec_cmd_name;
                event_emit(vm->event_sink, &ev);
            }

            /* Try runtime function lookup first */
            if (argc > 0) {
                char *cmd_name = exec_cmd_name;
                int fi;
                for (fi = 0; fi < vm->func_count; fi++) {
                    if (strcmp(vm->func_table[fi].name, cmd_name) == 0) {
                        rcstr_release(cmd_name);
                        exec_cmd_name = NULL;

                        /* Push call frame */
                        if (vm->call_depth >= VM_CALL_STACK_MAX) {
                            fprintf(stderr, "opsh: call stack overflow\n");
                            vm->laststatus = 1;
                            vm->halted = true;
                            for (i = 0; i < argc; i++) {
                                value_destroy(&argv[i]);
                            }
                            free(argv);
                            goto exec_simple_done;
                        }
                        call_frame_t *frame = &vm->call_stack[vm->call_depth++];
                        frame->return_ip = vm->ip;
                        frame->saved_env = vm->env;
                        frame->saved_stack_base = vm->stack_top;
                        frame->saved_loop_depth = vm->loop_depth;
                        frame->saved_image = vm->image;
                        vm->env = environ_new(vm->env, false);

                        /* Bind positional parameters */
                        for (i = 1; i < argc; i++) {
                            char param_name[16];
                            snprintf(param_name, sizeof(param_name), "%d", i);
                            environ_set(vm->env, param_name, argv[i]);
                            argv[i] = value_none();
                        }
                        {
                            char count_str[16];
                            snprintf(count_str, sizeof(count_str), "%d", argc > 0 ? argc - 1 : 0);
                            environ_set(vm->env, "#", value_string(rcstr_new(count_str)));
                        }

                        for (i = 0; i < argc; i++) {
                            value_destroy(&argv[i]);
                        }
                        free(argv);

                        vm->image =
                            vm->func_table[fi].image ? vm->func_table[fi].image : vm->main_image;
                        vm->ip = vm->func_table[fi].bytecode_offset;
                        exec_is_func = true;
                        goto exec_simple_done;
                    }
                }

                /* Also try builtins at runtime */
                int bi = builtin_lookup(cmd_name);
                if (bi >= 0) {
                    rcstr_release(cmd_name);
                    exec_cmd_name = NULL;
                    int status = builtin_table[bi].fn(vm, argc, argv);
                    vm->laststatus = status;
                    if (strcmp(builtin_table[bi].name, "exit") == 0) {
                        vm->exit_requested = true;
                        vm_exit(vm, status);
                    }
                    if (vm->return_requested) {
                        vm->return_requested = false;
                        if (vm->call_depth > 0) {
                            call_frame_t *frame = &vm->call_stack[--vm->call_depth];
                            while (vm->stack_top > frame->saved_stack_base) {
                                value_t v = vm_pop(vm);
                                value_destroy(&v);
                            }
                            vm->loop_depth = frame->saved_loop_depth;
                            environ_t *func_env = vm->env;
                            vm->env = func_env->parent;
                            environ_destroy(func_env);
                            vm->ip = frame->return_ip;
                            vm->image = frame->saved_image;
                        } else {
                            vm->halted = true;
                        }
                    }
                    for (i = 0; i < argc; i++) {
                        value_destroy(&argv[i]);
                    }
                    free(argv);
                    goto exec_simple_done;
                }

                /* External command: fork and execvp */
                {
                    char **exec_argv = xcalloc((size_t)argc + 1, sizeof(char *));
                    exec_argv[0] = cmd_name;
                    for (i = 1; i < argc; i++) {
                        exec_argv[i] = value_to_string(&argv[i]);
                    }
                    exec_argv[argc] = NULL;

                    if (vm->no_fork) {
                        vm->laststatus = 127;
                        for (i = 1; i < argc; i++) {
                            rcstr_release(exec_argv[i]);
                        }
                        free(exec_argv);
                        rcstr_release(cmd_name);
                        exec_cmd_name = NULL;
                        for (i = 0; i < argc; i++) {
                            value_destroy(&argv[i]);
                        }
                        free(argv);
                        goto exec_simple_done;
                    }
                    fflush(stdout);
                    fflush(stderr);
                    pid_t pid = fork();
                    if (pid < 0) {
                        fprintf(stderr, "opsh: fork: %s\n", strerror(errno));
                        vm->laststatus = 126;
                    } else if (pid == 0) {
                        /* Child: export shell vars to C env, reset signals */
                        signal_reset();
                        environ_export_to_c(vm->env);
                        execvp(exec_argv[0], exec_argv);
                        /* execvp failed */
                        int err = errno;
                        fprintf(stderr, "opsh: %s: %s\n", exec_argv[0], strerror(err));
                        _exit(err == ENOENT ? 127 : 126);
                    } else {
                        /* Parent: wait for child */
                        int wstatus = 0;
                        pid_t wp;
                        while ((wp = waitpid(pid, &wstatus, 0)) < 0 && errno == EINTR) {
                            /* retry */
                        }
                        if (wp < 0) {
                            vm->laststatus = 127;
                        } else if (WIFEXITED(wstatus)) {
                            vm->laststatus = WEXITSTATUS(wstatus);
                        } else if (WIFSIGNALED(wstatus)) {
                            vm->laststatus = 128 + WTERMSIG(wstatus);
                        } else {
                            vm->laststatus = 1;
                        }
                    }

                    for (i = 1; i < argc; i++) {
                        rcstr_release(exec_argv[i]);
                    }
                    free(exec_argv);
                    rcstr_release(cmd_name);
                    exec_cmd_name = NULL;
                }
            }

            for (i = 0; i < argc; i++) {
                value_destroy(&argv[i]);
            }
            free(argv);
        exec_simple_done:
            if (!exec_is_func) {
                event_t ev = {0};
                ev.type = EVENT_COMMAND_END;
                ev.id = exec_cmd_id;
                ev.status = vm->laststatus;
                event_emit(vm->event_sink, &ev);
            }
            rcstr_release(exec_cmd_name);
            break;
        }

        case OP_EXEC_FUNC: {
            uint16_t func_idx = read_u16(vm);

            /* Pop argc from stack */
            value_t argc_val = vm_pop(vm);
            int argc = (int)value_to_integer(&argc_val);
            value_destroy(&argc_val);

            /* Pop arguments */
            value_t *argv = xcalloc(argc ? (size_t)argc : 1, sizeof(value_t));
            int i;
            for (i = argc - 1; i >= 0; i--) {
                argv[i] = vm_pop(vm);
            }

            if (func_idx >= (uint16_t)vm->func_count) {
                fprintf(stderr, "opsh: unknown function index %u\n", func_idx);
                vm->laststatus = 127;
                for (i = 0; i < argc; i++) {
                    value_destroy(&argv[i]);
                }
                free(argv);
                break;
            }

            /* Push call frame */
            if (vm->call_depth >= VM_CALL_STACK_MAX) {
                fprintf(stderr, "opsh: call stack overflow\n");
                vm->laststatus = 1;
                vm->halted = true;
                for (i = 0; i < argc; i++) {
                    value_destroy(&argv[i]);
                }
                free(argv);
                break;
            }
            call_frame_t *frame = &vm->call_stack[vm->call_depth++];
            frame->return_ip = vm->ip;
            frame->saved_env = vm->env;
            frame->saved_stack_base = vm->stack_top;
            frame->saved_loop_depth = vm->loop_depth;
            frame->saved_image = vm->image;

            /* Push a new scope for the function */
            vm->env = environ_new(vm->env, false);

            /* Bind positional parameters: $1..$N (skip argv[0] which is the name) */
            for (i = 1; i < argc; i++) {
                char param_name[16];
                snprintf(param_name, sizeof(param_name), "%d", i);
                environ_set(vm->env, param_name, argv[i]);
                argv[i] = value_none(); /* ownership transferred */
            }
            /* Set $# (argument count, excluding function name) */
            {
                char count_str[16];
                snprintf(count_str, sizeof(count_str), "%d", argc > 0 ? argc - 1 : 0);
                environ_set(vm->env, "#", value_string(rcstr_new(count_str)));
            }

            for (i = 0; i < argc; i++) {
                value_destroy(&argv[i]);
            }
            free(argv);

            /* Swap to the function's image */
            vm->image =
                vm->func_table[func_idx].image ? vm->func_table[func_idx].image : vm->main_image;

            /* Jump to function body */
            vm->ip = vm->func_table[func_idx].bytecode_offset;
            break;
        }

        case OP_RET: {
            if (vm->call_depth <= 0) {
                vm->halted = true;
                break;
            }
            call_frame_t *frame = &vm->call_stack[--vm->call_depth];

            /* Clean up any extra values left on the stack by the function */
            while (vm->stack_top > frame->saved_stack_base) {
                value_t v = vm_pop(vm);
                value_destroy(&v);
            }

            /* Restore loop depth */
            vm->loop_depth = frame->saved_loop_depth;

            /* Pop function scope */
            environ_t *func_env = vm->env;
            vm->env = func_env->parent;
            environ_destroy(func_env);

            /* Restore IP and image */
            vm->ip = frame->return_ip;
            vm->image = frame->saved_image;
            break;
        }

        case OP_LOOP_ENTER:
            /* Recorded by the compiler via jump targets; VM tracks for break */
            if (vm->loop_depth >= VM_LOOP_STACK_MAX) {
                fprintf(stderr, "opsh: loop nesting too deep\n");
                vm->laststatus = 1;
                vm->halted = true;
                break;
            }
            vm->loop_stack[vm->loop_depth].saved_stack_top = vm->stack_top;
            vm->loop_depth++;
            break;

        case OP_LOOP_EXIT:
            if (vm->loop_depth > 0) {
                vm->loop_depth--;
            }
            break;

        case OP_TEST_UNARY: {
            uint8_t op = read_u8(vm);
            value_t arg = vm_pop(vm);
            char *s = value_to_string(&arg);
            value_destroy(&arg);
            int result = 1; /* default: false */

            switch ((test_op_t)op) {
            case TEST_F:
            case TEST_D:
            case TEST_E:
            case TEST_S:
            case TEST_R:
            case TEST_W:
            case TEST_X:
            case TEST_L:
            case TEST_P:
            case TEST_B:
            case TEST_C:
            case TEST_SS:
            case TEST_G:
            case TEST_U:
            case TEST_K:
            case TEST_O:
            case TEST_GG:
            case TEST_NT: {
                /* Map enum to operator string for shared test_file helper */
                static const char *const file_ops[] = {
                    [TEST_F] = "-f",  [TEST_D] = "-d",  [TEST_E] = "-e", [TEST_S] = "-s",
                    [TEST_R] = "-r",  [TEST_W] = "-w",  [TEST_X] = "-x", [TEST_L] = "-L",
                    [TEST_P] = "-p",  [TEST_B] = "-b",  [TEST_C] = "-c", [TEST_SS] = "-S",
                    [TEST_G] = "-g",  [TEST_U] = "-u",  [TEST_K] = "-k", [TEST_O] = "-O",
                    [TEST_GG] = "-G", [TEST_NT] = "-N",
                };
                result = test_file(file_ops[op], s);
                break;
            }
            case TEST_T: {
                int fd_num = (int)strtol(s, NULL, 10);
                result = isatty(fd_num) ? 0 : 1;
                break;
            }
            case TEST_N:
                result = (s[0] != '\0') ? 0 : 1;
                break;
            case TEST_Z:
                result = (s[0] == '\0') ? 0 : 1;
                break;
            default:
                break;
            }

            free(s);
            vm->laststatus = result;
            break;
        }

        case OP_TEST_BINARY: {
            uint8_t op = read_u8(vm);
            value_t rhs_v = vm_pop(vm);
            value_t lhs_v = vm_pop(vm);
            char *lhs = value_to_string(&lhs_v);
            char *rhs = value_to_string(&rhs_v);
            value_destroy(&lhs_v);
            value_destroy(&rhs_v);
            int result = 1;

            switch ((test_op_t)op) {
            case TEST_SEQ:
                /* In [[ ]], == does glob pattern matching (rhs is pattern) */
                result = (fnmatch(rhs, lhs, 0) == 0) ? 0 : 1;
                break;
            case TEST_SNE:
                result = (fnmatch(rhs, lhs, 0) != 0) ? 0 : 1;
                break;
            case TEST_EQ:
                result = (strtoll(lhs, NULL, 10) == strtoll(rhs, NULL, 10)) ? 0 : 1;
                break;
            case TEST_NE:
                result = (strtoll(lhs, NULL, 10) != strtoll(rhs, NULL, 10)) ? 0 : 1;
                break;
            case TEST_LT:
                result = (strtoll(lhs, NULL, 10) < strtoll(rhs, NULL, 10)) ? 0 : 1;
                break;
            case TEST_LE:
                result = (strtoll(lhs, NULL, 10) <= strtoll(rhs, NULL, 10)) ? 0 : 1;
                break;
            case TEST_GT:
                result = (strtoll(lhs, NULL, 10) > strtoll(rhs, NULL, 10)) ? 0 : 1;
                break;
            case TEST_GE:
                result = (strtoll(lhs, NULL, 10) >= strtoll(rhs, NULL, 10)) ? 0 : 1;
                break;
            case TEST_FNT:
            case TEST_FOT:
            case TEST_FEF: {
                struct stat st1, st2;
                if (stat(lhs, &st1) == 0 && stat(rhs, &st2) == 0) {
                    if (op == TEST_FNT) {
                        result = (st1.st_mtime > st2.st_mtime) ? 0 : 1;
                    } else if (op == TEST_FOT) {
                        result = (st1.st_mtime < st2.st_mtime) ? 0 : 1;
                    } else {
                        result = (st1.st_dev == st2.st_dev && st1.st_ino == st2.st_ino) ? 0 : 1;
                    }
                }
                break;
            }
            case TEST_REGEX: {
                regex_t re;
                if (regcomp(&re, rhs, REG_EXTENDED) == 0) {
                    size_t nmatch = re.re_nsub + 1;
                    regmatch_t *matches = xcalloc(nmatch, sizeof(regmatch_t));
                    if (regexec(&re, lhs, nmatch, matches, 0) == 0) {
                        result = 0;
                        /* Populate BASH_REMATCH array */
                        int count = 0;
                        size_t mi;
                        for (mi = 0; mi < nmatch; mi++) {
                            if (matches[mi].rm_so >= 0) {
                                count = (int)mi + 1;
                            }
                        }
                        char **elements = xcalloc(count ? (size_t)count : 1, sizeof(char *));
                        for (mi = 0; mi < (size_t)count; mi++) {
                            if (matches[mi].rm_so >= 0) {
                                size_t mlen = (size_t)(matches[mi].rm_eo - matches[mi].rm_so);
                                elements[mi] = rcstr_from_buf(lhs + matches[mi].rm_so, mlen);
                            } else {
                                elements[mi] = rcstr_new("");
                            }
                        }
                        value_t arr;
                        arr.type = VT_ARRAY;
                        arr.data.array.elements = elements;
                        arr.data.array.count = count;
                        environ_assign(vm->env, "BASH_REMATCH", arr);
                    } else {
                        result = 1;
                        /* Clear BASH_REMATCH on non-match */
                        value_t empty_arr;
                        empty_arr.type = VT_ARRAY;
                        empty_arr.data.array.elements = NULL;
                        empty_arr.data.array.count = 0;
                        environ_assign(vm->env, "BASH_REMATCH", empty_arr);
                    }
                    free(matches);
                    regfree(&re);
                } else {
                    fprintf(stderr, "opsh: invalid regex: %s\n", rhs);
                    result = 2;
                }
                break;
            }
            default:
                break;
            }

            free(lhs);
            free(rhs);
            vm->laststatus = result;
            break;
        }

        case OP_NEGATE_STATUS:
            vm->laststatus = (vm->laststatus == 0) ? 1 : 0;
            break;

        case OP_PATTERN_MATCH: {
            value_t pattern_val = vm_pop(vm);
            value_t subject_val = vm_pop(vm);
            char *pattern = value_to_string(&pattern_val);
            char *subject = value_to_string(&subject_val);
            value_destroy(&pattern_val);
            value_destroy(&subject_val);

            vm->laststatus = (fnmatch(pattern, subject, 0) == 0) ? 0 : 1;

            rcstr_release(pattern);
            rcstr_release(subject);
            break;
        }

        case OP_CMD_SUBST: {
            uint32_t sub_offset = (uint32_t)read_u8(vm);
            sub_offset |= (uint32_t)read_u8(vm) << 8;
            sub_offset |= (uint32_t)read_u8(vm) << 16;
            sub_offset |= (uint32_t)read_u8(vm) << 24;

            int pipefd[2];
            if (pipe(pipefd) < 0) {
                fprintf(stderr, "opsh: pipe failed\n");
                vm_push(vm, value_string(rcstr_new("")));
                vm->laststatus = 1;
                break;
            }

            if (vm->no_fork) {
                close(pipefd[0]);
                close(pipefd[1]);
                vm_push(vm, value_string(rcstr_new("")));
                vm->laststatus = 127;
                break;
            }

            /* Flush stdio before forking to avoid duplicated buffered output */
            fflush(stdout);
            fflush(stderr);

            pid_t pid = fork();
            if (pid < 0) {
                fprintf(stderr, "opsh: fork failed\n");
                close(pipefd[0]);
                close(pipefd[1]);
                vm_push(vm, value_string(rcstr_new("")));
                vm->laststatus = 1;
                break;
            }

            if (pid == 0) {
                /* Child: restore signal defaults, clear traps */
                signal_reset();
                free(vm->exit_trap);
                vm->exit_trap = NULL;
                vm->event_sink = NULL; /* children don't emit events */
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);

                vm->ip = (size_t)sub_offset;
                vm->halted = false;
                vm->captured_stdout = NULL;
                {
                    int child_status = vm_run(vm);
                    fflush(stdout);
                    _exit(child_status);
                }
            }

            /* Parent: read from pipe */
            close(pipefd[1]);
            strbuf_t captured;
            strbuf_init(&captured);
            {
                char buf[4096];
                ssize_t n;
                for (;;) {
                    n = read(pipefd[0], buf, sizeof(buf));
                    if (n > 0) {
                        strbuf_append_bytes(&captured, buf, (size_t)n);
                    } else if (n == 0) {
                        break;
                    } else if (errno == EINTR) {
                        continue;
                    } else {
                        break;
                    }
                }
            }
            close(pipefd[0]);

            int wstatus;
            while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {
                /* retry */
            }
            if (WIFEXITED(wstatus)) {
                vm->laststatus = WEXITSTATUS(wstatus);
            } else {
                vm->laststatus = 1;
            }

            /* Strip trailing newlines */
            while (captured.length > 0 && captured.contents[captured.length - 1] == '\n') {
                captured.length--;
                captured.contents[captured.length] = '\0';
            }

            {
                char *tmp = strbuf_detach(&captured);
                vm_push(vm, value_string(rcstr_new(tmp)));
                free(tmp);
            }
            break;
        }

        case OP_SUBSHELL: {
            uint32_t sub_offset = (uint32_t)read_u8(vm);
            sub_offset |= (uint32_t)read_u8(vm) << 8;
            sub_offset |= (uint32_t)read_u8(vm) << 16;
            sub_offset |= (uint32_t)read_u8(vm) << 24;

            if (vm->no_fork) {
                vm->laststatus = 127;
                break;
            }

            fflush(stdout);
            fflush(stderr);

            pid_t pid = fork();
            if (pid < 0) {
                fprintf(stderr, "opsh: fork failed\n");
                vm->laststatus = 1;
                break;
            }

            if (pid == 0) {
                /* Child: run the subshell body */
                signal_reset();
                free(vm->exit_trap);
                vm->exit_trap = NULL;
                vm->event_sink = NULL;
                vm->ip = (size_t)sub_offset;
                vm->halted = false;
                int child_status = vm_run(vm);
                fflush(stdout);
                fflush(stderr);
                _exit(child_status);
            }

            /* Parent: wait for child */
            {
                int wstatus = 0;
                pid_t wp;
                while ((wp = waitpid(pid, &wstatus, 0)) < 0 && errno == EINTR) {
                    /* retry */
                }
                if (wp < 0) {
                    vm->laststatus = 127;
                } else if (WIFEXITED(wstatus)) {
                    vm->laststatus = WEXITSTATUS(wstatus);
                } else if (WIFSIGNALED(wstatus)) {
                    vm->laststatus = 128 + WTERMSIG(wstatus);
                } else {
                    vm->laststatus = 1;
                }
            }
            break;
        }

        case OP_BACKGROUND: {
            uint32_t sub_offset = (uint32_t)read_u8(vm);
            sub_offset |= (uint32_t)read_u8(vm) << 8;
            sub_offset |= (uint32_t)read_u8(vm) << 16;
            sub_offset |= (uint32_t)read_u8(vm) << 24;

            int64_t bg_cmd_id = vm->next_command_id++;
            {
                event_t ev = {0};
                ev.type = EVENT_COMMAND_START;
                ev.id = bg_cmd_id;
                ev.name = "(background)";
                event_emit(vm->event_sink, &ev);
            }

            if (vm->no_fork) {
                vm->laststatus = 0;
                break;
            }

            fflush(stdout);
            fflush(stderr);

            pid_t pid = fork();
            if (pid < 0) {
                fprintf(stderr, "opsh: fork failed\n");
                vm->laststatus = 1;
            } else if (pid == 0) {
                /* Child: run the background command */
                signal_reset();
                free(vm->exit_trap);
                vm->exit_trap = NULL;
                vm->event_sink = NULL;
                vm->opt_errexit = false; /* POSIX: errexit disabled in async */
                /* POSIX: stdin redirected to /dev/null for async commands */
                {
                    int devnull = open("/dev/null", O_RDONLY);
                    if (devnull >= 0 && devnull != STDIN_FILENO) {
                        dup2(devnull, STDIN_FILENO);
                        close(devnull);
                    }
                }
                vm->ip = (size_t)sub_offset;
                vm->halted = false;
                int child_status = vm_run(vm);
                fflush(stdout);
                fflush(stderr);
                _exit(child_status);
            } else {
                /* Parent: record PID, don't wait */
                vm->last_bg_pid = pid;
                vm->laststatus = 0;
            }

            {
                event_t ev = {0};
                ev.type = EVENT_COMMAND_END;
                ev.id = bg_cmd_id;
                ev.status = vm->laststatus;
                event_emit(vm->event_sink, &ev);
            }
            break;
        }

        case OP_PIPELINE: {
            /* Read cmd_count, then collect PIPELINE_CMD offsets */
            uint16_t cmd_count = read_u16(vm);
            uint32_t *offsets = xcalloc(cmd_count ? cmd_count : 1, sizeof(uint32_t));
            int pi;

            for (pi = 0; pi < (int)cmd_count; pi++) {
                uint8_t next_op = read_u8(vm);
                if (next_op != OP_PIPELINE_CMD) {
                    fprintf(stderr, "opsh: expected PIPELINE_CMD, got 0x%02x\n", next_op);
                    vm->laststatus = 1;
                    vm->halted = true;
                    free(offsets);
                    goto pipeline_done;
                }
                uint32_t off = (uint32_t)read_u8(vm);
                off |= (uint32_t)read_u8(vm) << 8;
                off |= (uint32_t)read_u8(vm) << 16;
                off |= (uint32_t)read_u8(vm) << 24;
                offsets[pi] = off;
            }

            /* Read PIPELINE_END */
            {
                uint8_t end_op = read_u8(vm);
                if (end_op != OP_PIPELINE_END) {
                    fprintf(stderr, "opsh: expected PIPELINE_END, got 0x%02x\n", end_op);
                    vm->laststatus = 1;
                    vm->halted = true;
                    free(offsets);
                    goto pipeline_done;
                }
            }
            uint8_t pl_flags = read_u8(vm);
            bool negate = (pl_flags & 1) != 0;

            if (vm->no_fork) {
                vm->laststatus = 127;
                if (negate) {
                    vm->laststatus = (vm->laststatus == 0) ? 1 : 0;
                }
                free(offsets);
                goto pipeline_done;
            }

            /* Create pipes and fork children */
            pid_t *pids = xcalloc((size_t)cmd_count, sizeof(pid_t));
            int prev_read_fd = -1;

            for (pi = 0; pi < (int)cmd_count; pi++) {
                int pipefd[2] = {-1, -1};
                bool has_pipe = (pi < (int)cmd_count - 1);

                if (has_pipe) {
                    if (pipe(pipefd) < 0) {
                        fprintf(stderr, "opsh: pipe failed\n");
                        vm->laststatus = 1;
                        break;
                    }
                }

                fflush(stdout);
                fflush(stderr);
                pid_t pid = fork();
                if (pid < 0) {
                    fprintf(stderr, "opsh: fork failed\n");
                    if (has_pipe) {
                        close(pipefd[0]);
                        close(pipefd[1]);
                    }
                    vm->laststatus = 1;
                    break;
                }

                if (pid == 0) {
                    /* Child: restore signal defaults, clear traps */
                    signal_reset();
                    free(vm->exit_trap);
                    vm->exit_trap = NULL;
                    vm->event_sink = NULL; /* children don't emit events */
                    if (prev_read_fd >= 0) {
                        dup2(prev_read_fd, STDIN_FILENO);
                        close(prev_read_fd);
                    }
                    if (has_pipe) {
                        close(pipefd[0]);
                        dup2(pipefd[1], STDOUT_FILENO);
                        close(pipefd[1]);
                    }

                    vm->ip = (size_t)offsets[pi];
                    vm->halted = false;
                    vm->captured_stdout = NULL;
                    {
                        int child_status = vm_run(vm);
                        fflush(stdout);
                        _exit(child_status);
                    }
                }

                /* Parent */
                pids[pi] = pid;
                if (prev_read_fd >= 0) {
                    close(prev_read_fd);
                }
                if (has_pipe) {
                    close(pipefd[1]);
                    prev_read_fd = pipefd[0];
                } else {
                    prev_read_fd = -1;
                }
            }

            /* Clean up any remaining pipe FD */
            if (prev_read_fd >= 0) {
                close(prev_read_fd);
            }

            /* Wait for all children */
            {
                int last_status = 0;
                int wi;
                for (wi = 0; wi < (int)cmd_count; wi++) {
                    if (pids[wi] > 0) {
                        int wstatus;
                        while (waitpid(pids[wi], &wstatus, 0) < 0 && errno == EINTR) {
                            /* retry */
                        }
                        if (WIFEXITED(wstatus)) {
                            last_status = WEXITSTATUS(wstatus);
                        } else {
                            last_status = 1;
                        }
                    }
                }
                vm->laststatus = last_status;
            }

            if (negate) {
                vm->laststatus = (vm->laststatus == 0) ? 1 : 0;
            }

            free(pids);
            free(offsets);
        pipeline_done:
            break;
        }

        case OP_PIPELINE_CMD:
        case OP_PIPELINE_END:
            /* These are consumed by OP_PIPELINE; should not be reached independently */
            fprintf(stderr, "opsh: unexpected PIPELINE_CMD/END outside pipeline\n");
            vm->laststatus = 1;
            vm->halted = true;
            break;

        case OP_REDIR_SAVE: {
            if (vm->redir_depth >= VM_REDIR_STACK_MAX) {
                fprintf(stderr, "opsh: redirection stack overflow\n");
                vm->laststatus = 1;
                vm->halted = true;
                break;
            }
            redir_frame_t *frame = &vm->redir_stack[vm->redir_depth++];
            frame->count = 0;
            break;
        }

        case OP_REDIR_RESTORE: {
            if (vm->redir_depth <= 0) {
                break;
            }
            redir_frame_t *frame = &vm->redir_stack[--vm->redir_depth];
            int ri;
            /* Restore saved FDs in reverse order */
            for (ri = frame->count - 1; ri >= 0; ri--) {
                saved_fd_t *sf = &frame->entries[ri];
                if (sf->saved_fd >= 0) {
                    dup2(sf->saved_fd, sf->original_fd);
                    close(sf->saved_fd);
                } else {
                    /* FD was closed; close it again */
                    close(sf->original_fd);
                }
            }
            break;
        }

        case OP_REDIR_OPEN: {
            uint8_t fd = read_u8(vm);
            uint8_t rtype = read_u8(vm);
            value_t filename_val = vm_pop(vm);
            char *filename = value_to_string(&filename_val);
            value_destroy(&filename_val);

            int flags_val = 0;
            int new_fd = -1;

            switch ((io_redir_type_t)rtype) {
            case REDIR_IN:
                flags_val = O_RDONLY;
                break;
            case REDIR_OUT:
                flags_val = O_WRONLY | O_CREAT | O_TRUNC;
                break;
            case REDIR_APPEND:
                flags_val = O_WRONLY | O_CREAT | O_APPEND;
                break;
            case REDIR_CLOBBER:
                flags_val = O_WRONLY | O_CREAT | O_TRUNC;
                break;
            case REDIR_RDWR:
                flags_val = O_RDWR | O_CREAT;
                break;
            default:
                break;
            }

            new_fd = open(filename, flags_val, 0666);
            if (new_fd < 0) {
                fprintf(stderr, "opsh: %s: cannot open\n", filename);
                rcstr_release(filename);
                vm->laststatus = 1;
                break;
            }
            rcstr_release(filename);

            /* Save the original FD */
            if (vm->redir_depth > 0) {
                redir_frame_t *frame = &vm->redir_stack[vm->redir_depth - 1];
                if (frame->count < 16) {
                    int saved = dup(fd);
                    if (saved >= 0) {
                        frame->entries[frame->count].original_fd = fd;
                        frame->entries[frame->count].saved_fd = saved;
                        frame->count++;
                    }
                }
            }

            dup2(new_fd, fd);
            if (new_fd != (int)fd) {
                close(new_fd);
            }
            break;
        }

        case OP_REDIR_DUP: {
            uint8_t fd = read_u8(vm);
            uint8_t flags = read_u8(vm);
            (void)flags;
            value_t target_val = vm_pop(vm);
            char *target_str = value_to_string(&target_val);
            value_destroy(&target_val);
            int target_fd = atoi(target_str);
            rcstr_release(target_str);

            /* Save the original FD */
            if (vm->redir_depth > 0) {
                redir_frame_t *frame = &vm->redir_stack[vm->redir_depth - 1];
                if (frame->count < 16) {
                    int saved = dup(fd);
                    if (saved >= 0) {
                        frame->entries[frame->count].original_fd = fd;
                        frame->entries[frame->count].saved_fd = saved;
                        frame->count++;
                    }
                }
            }

            dup2(target_fd, fd);
            break;
        }

        case OP_REDIR_CLOSE: {
            uint8_t fd = read_u8(vm);

            /* Save the original FD */
            if (vm->redir_depth > 0) {
                redir_frame_t *frame = &vm->redir_stack[vm->redir_depth - 1];
                if (frame->count < 16) {
                    int saved = dup(fd);
                    frame->entries[frame->count].original_fd = fd;
                    frame->entries[frame->count].saved_fd = saved;
                    frame->count++;
                }
            }

            close(fd);
            break;
        }

        case OP_REDIR_HERE: {
            uint8_t fd = read_u8(vm);
            uint8_t here_flags = read_u8(vm);
            (void)here_flags;
            value_t content_val = vm_pop(vm);
            char *content = value_to_string(&content_val);
            value_destroy(&content_val);

            /* Use a temporary file to avoid pipe deadlock on large content */
            const char *tmpdir = getenv("TMPDIR");
            if (tmpdir == NULL) {
                tmpdir = "/tmp";
            }
            char tmppath[4096];
            snprintf(tmppath, sizeof(tmppath), "%s/opsh-here-XXXXXX", tmpdir);
            int tmpfd = mkstemp(tmppath);
            if (tmpfd < 0) {
                fprintf(stderr, "opsh: cannot create temp file for here-document\n");
                rcstr_release(content);
                vm->laststatus = 1;
                break;
            }
            unlink(tmppath); /* delete on close */

            /* Write content */
            {
                size_t len = strlen(content);
                size_t written = 0;
                while (written < len) {
                    ssize_t n = write(tmpfd, content + written, len - written);
                    if (n > 0) {
                        written += (size_t)n;
                    } else if (n < 0 && errno == EINTR) {
                        continue;
                    } else {
                        break;
                    }
                }
            }
            lseek(tmpfd, 0, SEEK_SET);
            rcstr_release(content);

            /* Save the original FD and redirect */
            if (vm->redir_depth > 0) {
                redir_frame_t *frame = &vm->redir_stack[vm->redir_depth - 1];
                if (frame->count < 16) {
                    int saved = dup(fd);
                    frame->entries[frame->count].original_fd = fd;
                    frame->entries[frame->count].saved_fd = saved;
                    frame->count++;
                }
            }

            dup2(tmpfd, fd);
            close(tmpfd);
            break;
        }

        case OP_IMPORT: {
            uint16_t name_idx = read_u16(vm);
            const char *module_name = vm->image->const_pool[name_idx];

            /* Check if already initialized */
            if (ht_get(&vm->modules_loaded, module_name) != NULL) {
                break; /* skip -- already loaded */
            }

            /* Find the module's init offset and run init in a fresh VM */
            {
                int mi;
                bool found = false;
                for (mi = 0; mi < vm->image->module_count; mi++) {
                    if (strcmp(vm->image->modules[mi].name, module_name) == 0) {
                        ht_set(&vm->modules_loaded, module_name, (void *)1);

                        /* Run module init in a fresh VM to avoid state corruption.
                         * The init VM shares the environment so module-level
                         * variable assignments are visible to the caller. */
                        vm_t init_vm;
                        vm_init(&init_vm, vm->image);
                        /* Share the environment */
                        environ_destroy(init_vm.env);
                        init_vm.env = vm->env;
                        init_vm.ip = vm->image->modules[mi].init_offset;
                        vm_run(&init_vm);
                        init_vm.env = NULL; /* don't double-free */
                        vm_destroy(&init_vm);

                        found = true;
                        break;
                    }
                }
                if (!found) {
                    fprintf(stderr, "opsh: module '%s' not found in image\n", module_name);
                    vm->laststatus = 1;
                }
            }
            break;
        }

        case OP_ERREXIT_PUSH:
            vm->errexit_suppressed++;
            break;

        case OP_ERREXIT_POP:
            if (vm->errexit_suppressed > 0) {
                vm->errexit_suppressed--;
            }
            break;

        case OP_HALT:
            vm_exit(vm, vm->laststatus);
            break;

        default:
            fprintf(stderr, "opsh: unimplemented opcode 0x%02x at offset %zu\n", op, vm->ip - 1);
            vm->laststatus = 1;
            vm->halted = true;
            break;
        }

        /* errexit (-e): exit if command failed and not suppressed */
        if (vm->opt_errexit && vm->errexit_suppressed == 0 && vm->laststatus != 0) {
            /* Only trigger on command execution opcodes */
            if (op == OP_EXEC_SIMPLE || op == OP_EXEC_BUILTIN || op == OP_EXEC_FUNC ||
                op == OP_PIPELINE || op == OP_SUBSHELL) {
                vm_exit(vm, vm->laststatus);
            }
        }
    }

    return vm->laststatus;
}
