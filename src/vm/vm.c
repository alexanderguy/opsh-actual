#include "vm/vm.h"

#include "builtins/builtins.h"
#include "foundation/rcstr.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "parser/ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    vm->env = environ_new(NULL, false);
    vm->laststatus = 0;
    vm->halted = false;
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
            environ_set(vm->env, name, val);
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
                /* Special case: exit halts the VM */
                if (strcmp(builtin_table[builtin_idx].name, "exit") == 0) {
                    vm->halted = true;
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

        case OP_REDIR_SAVE:
        case OP_REDIR_RESTORE:
            /* Stubs for Phase 3c; no-ops until Phase 4f */
            break;

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
