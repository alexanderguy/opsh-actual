#include "vm/vm.h"

#include "builtins/builtins.h"
#include "foundation/rcstr.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "parser/ast.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    vm->ip = 0;
    vm->stack_top = 0;
    vm->call_depth = 0;
    vm->loop_depth = 0;
    vm->func_table = image ? image->funcs : NULL;
    vm->func_count = image ? image->func_count : 0;
    vm->env = environ_new(NULL, false);
    vm->laststatus = 0;
    vm->halted = false;
}

void vm_register_func(vm_t *vm, const char *name, size_t offset)
{
    vm->func_table = xrealloc(vm->func_table, ((size_t)vm->func_count + 1) * sizeof(vm_func_t));
    vm->func_table[vm->func_count].name = name;
    vm->func_table[vm->func_count].bytecode_offset = offset;
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

    vm->func_table = NULL;
    vm->func_count = 0;

    free(vm->captured_stdout);
    vm->captured_stdout = NULL;
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
            default:
                vm_push(vm, value_none());
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
                if (var != NULL && var->value.type == VT_STRING) {
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
                value_t pattern = vm_pop(vm);
                /* Pattern matching is deferred; for now just return the value */
                value_destroy(&pattern);
                if (var_val != NULL) {
                    vm_push(vm, value_string(var_val));
                } else {
                    vm_push(vm, value_string(rcstr_new("")));
                }
                break;
            }
            case PE_REPLACE: {
                /* ${var/pat/rep} or ${var//pat/rep} */
                value_t replacement = vm_pop(vm);
                value_t pattern = vm_pop(vm);
                /* Substitution is deferred; for now just return the value */
                value_destroy(&pattern);
                value_destroy(&replacement);
                if (var_val != NULL) {
                    vm_push(vm, value_string(var_val));
                } else {
                    vm_push(vm, value_string(rcstr_new("")));
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

            /* Simple integer arithmetic evaluator */
            /* For now, handle simple integer literals and basic +,-,*,/ */
            int64_t result = 0;
            char *p = expr_str;
            char *endp;

            /* Skip whitespace */
            while (*p == ' ' || *p == '\t') {
                p++;
            }

            if (*p != '\0') {
                result = strtoll(p, &endp, 10);
                /* Handle basic binary operations */
                while (*endp != '\0') {
                    while (*endp == ' ' || *endp == '\t') {
                        endp++;
                    }
                    char oper = *endp;
                    if (oper != '+' && oper != '-' && oper != '*' && oper != '/' && oper != '%') {
                        break;
                    }
                    endp++;
                    while (*endp == ' ' || *endp == '\t') {
                        endp++;
                    }
                    int64_t rhs = strtoll(endp, &endp, 10);
                    switch (oper) {
                    case '+':
                        result += rhs;
                        break;
                    case '-':
                        result -= rhs;
                        break;
                    case '*':
                        result *= rhs;
                        break;
                    case '/':
                        if (rhs != 0) {
                            result /= rhs;
                        }
                        break;
                    case '%':
                        if (rhs != 0) {
                            result %= rhs;
                        }
                        break;
                    }
                }
            }

            rcstr_release(expr_str);

            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)result);
            vm_push(vm, value_string(rcstr_new(buf)));
            break;
        }

        case OP_QUOTE_REMOVE:
        case OP_EXPAND_TILDE:
            /* These are handled at compile time for now; no-ops at runtime */
            break;

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

            if (builtin_idx < (uint16_t)builtin_count() && builtin_table[builtin_idx].fn != NULL) {
                int status = builtin_table[builtin_idx].fn(vm, argc, argv);
                vm->laststatus = status;
                /* Special cases */
                if (strcmp(builtin_table[builtin_idx].name, "exit") == 0) {
                    vm->halted = true;
                }
                /* return builtin triggers function return */
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
                    } else {
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

            /* Try runtime function lookup first */
            if (argc > 0) {
                char *cmd_name = value_to_string(&argv[0]);
                int fi;
                for (fi = 0; fi < vm->func_count; fi++) {
                    if (strcmp(vm->func_table[fi].name, cmd_name) == 0) {
                        rcstr_release(cmd_name);

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

                        vm->ip = vm->func_table[fi].bytecode_offset;
                        goto exec_simple_done;
                    }
                }

                /* Also try builtins at runtime */
                int bi = builtin_lookup(cmd_name);
                if (bi >= 0) {
                    rcstr_release(cmd_name);
                    int status = builtin_table[bi].fn(vm, argc, argv);
                    vm->laststatus = status;
                    if (strcmp(builtin_table[bi].name, "exit") == 0) {
                        vm->halted = true;
                    }
                    for (i = 0; i < argc; i++) {
                        value_destroy(&argv[i]);
                    }
                    free(argv);
                    goto exec_simple_done;
                }

                fprintf(stderr, "opsh: %s: command not found\n", cmd_name);
                rcstr_release(cmd_name);
            }

            vm->laststatus = 127;
            for (i = 0; i < argc; i++) {
                value_destroy(&argv[i]);
            }
            free(argv);
        exec_simple_done:
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

            /* Restore IP */
            vm->ip = frame->return_ip;
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

            /* Simple glob matching: * matches anything, ? matches one char */
            vm->laststatus = 1; /* assume no match */
            {
                const char *p = pattern;
                const char *s = subject;
                const char *star_p = NULL;
                const char *star_s = NULL;

                while (*s) {
                    if (*p == '*') {
                        star_p = p++;
                        star_s = s;
                    } else if (*p == '?' || *p == *s) {
                        p++;
                        s++;
                    } else if (star_p != NULL) {
                        p = star_p + 1;
                        s = ++star_s;
                    } else {
                        break;
                    }
                }
                while (*p == '*') {
                    p++;
                }
                if (*p == '\0' && *s == '\0') {
                    vm->laststatus = 0; /* match */
                }
            }

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
                /* Child: redirect stdout to pipe write end */
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);

                /* Execute the sub-segment */
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
                while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                    strbuf_append_bytes(&captured, buf, (size_t)n);
                }
            }
            close(pipefd[0]);

            int wstatus;
            waitpid(pid, &wstatus, 0);
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
                    /* Child */
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
                        waitpid(pids[wi], &wstatus, 0);
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
                    if (n <= 0) {
                        break;
                    }
                    written += (size_t)n;
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

        case OP_HALT:
            vm->halted = true;
            break;

        default:
            fprintf(stderr, "opsh: unimplemented opcode 0x%02x at offset %zu\n", op, vm->ip - 1);
            vm->laststatus = 1;
            vm->halted = true;
            break;
        }
    }

    return vm->laststatus;
}
