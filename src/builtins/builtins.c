#include "builtins/builtins.h"

#include "exec/variable.h"
#include "foundation/rcstr.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "vm/vm.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_fd1(const char *data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(STDOUT_FILENO, data + written, len - written);
        if (n <= 0) {
            break;
        }
        written += (size_t)n;
    }
}

static int builtin_echo(vm_t *vm, int argc, value_t *argv)
{
    int i;
    strbuf_t out;
    (void)vm;
    strbuf_init(&out);

    for (i = 1; i < argc; i++) {
        char *s = value_to_string(&argv[i]);
        if (i > 1) {
            strbuf_append_byte(&out, ' ');
        }
        strbuf_append_str(&out, s);
        rcstr_release(s);
    }
    strbuf_append_byte(&out, '\n');

    write_fd1(out.contents, out.length);
    strbuf_destroy(&out);
    return 0;
}

static int builtin_exit(vm_t *vm, int argc, value_t *argv)
{
    int code = 0;
    (void)vm;
    if (argc > 1) {
        code = (int)value_to_integer(&argv[1]);
    }
    return code;
}

static int builtin_true(vm_t *vm, int argc, value_t *argv)
{
    (void)vm;
    (void)argc;
    (void)argv;
    return 0;
}

static int builtin_false(vm_t *vm, int argc, value_t *argv)
{
    (void)vm;
    (void)argc;
    (void)argv;
    return 1;
}

static int builtin_colon(vm_t *vm, int argc, value_t *argv)
{
    (void)vm;
    (void)argc;
    (void)argv;
    return 0;
}

static int builtin_cd(vm_t *vm, int argc, value_t *argv)
{
    const char *dir = NULL;
    char *dir_str = NULL;

    if (argc > 1) {
        dir_str = value_to_string(&argv[1]);
        dir = dir_str;
    } else {
        variable_t *home = environ_get(vm->env, "HOME");
        if (home != NULL) {
            dir_str = value_to_string(&home->value);
            dir = dir_str;
        }
    }

    if (dir == NULL || dir[0] == '\0') {
        fprintf(stderr, "opsh: cd: HOME not set\n");
        rcstr_release(dir_str);
        return 1;
    }

    if (chdir(dir) != 0) {
        fprintf(stderr, "opsh: cd: %s: %s\n", dir, strerror(errno));
        rcstr_release(dir_str);
        return 1;
    }

    /* Update PWD */
    {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            environ_assign(vm->env, "PWD", value_string(rcstr_new(cwd)));
        }
    }

    rcstr_release(dir_str);
    return 0;
}

static int builtin_pwd(vm_t *vm, int argc, value_t *argv)
{
    (void)vm;
    (void)argc;
    (void)argv;
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "opsh: pwd: %s\n", strerror(errno));
        return 1;
    }
    strbuf_t out;
    strbuf_init(&out);
    strbuf_append_str(&out, cwd);
    strbuf_append_byte(&out, '\n');
    write_fd1(out.contents, out.length);
    strbuf_destroy(&out);
    return 0;
}

static int builtin_export(vm_t *vm, int argc, value_t *argv)
{
    int i;
    if (argc <= 1) {
        return 0;
    }
    for (i = 1; i < argc; i++) {
        char *arg = value_to_string(&argv[i]);
        char *eq = strchr(arg, '=');
        if (eq != NULL) {
            /* export NAME=VALUE */
            size_t name_len = (size_t)(eq - arg);
            char *name = xmalloc(name_len + 1);
            memcpy(name, arg, name_len);
            name[name_len] = '\0';
            environ_assign(vm->env, name, value_string(rcstr_new(eq + 1)));
            environ_export(vm->env, name);
            free(name);
        } else {
            /* export NAME */
            environ_export(vm->env, arg);
        }
        rcstr_release(arg);
    }
    return 0;
}

static int builtin_unset(vm_t *vm, int argc, value_t *argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        char *name = value_to_string(&argv[i]);
        environ_unset(vm->env, name);
        rcstr_release(name);
    }
    return 0;
}

static int builtin_readonly(vm_t *vm, int argc, value_t *argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        char *arg = value_to_string(&argv[i]);
        char *eq = strchr(arg, '=');
        if (eq != NULL) {
            size_t name_len = (size_t)(eq - arg);
            char *name = xmalloc(name_len + 1);
            memcpy(name, arg, name_len);
            name[name_len] = '\0';
            environ_set_flags(vm->env, name, value_string(rcstr_new(eq + 1)), VF_READONLY);
            free(name);
        } else {
            variable_t *var = environ_get(vm->env, arg);
            if (var != NULL) {
                var->flags |= VF_READONLY;
            } else {
                environ_set_flags(vm->env, arg, value_string(rcstr_new("")), VF_READONLY);
            }
        }
        rcstr_release(arg);
    }
    return 0;
}

static int builtin_local(vm_t *vm, int argc, value_t *argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        char *arg = value_to_string(&argv[i]);
        char *eq = strchr(arg, '=');
        if (eq != NULL) {
            size_t name_len = (size_t)(eq - arg);
            char *name = xmalloc(name_len + 1);
            memcpy(name, arg, name_len);
            name[name_len] = '\0';
            /* Set in current (function) scope, allowing shadow of readonly */
            environ_set_local(vm->env, name, value_string(rcstr_new(eq + 1)));
            free(name);
        } else {
            /* Declare local with empty value */
            environ_set_local(vm->env, arg, value_string(rcstr_new("")));
        }
        rcstr_release(arg);
    }
    return 0;
}

static int builtin_shift(vm_t *vm, int argc, value_t *argv)
{
    int n = 1;
    if (argc > 1) {
        n = (int)value_to_integer(&argv[1]);
    }

    /* Get current $# */
    variable_t *hash_var = environ_get(vm->env, "#");
    int param_count = 0;
    if (hash_var != NULL) {
        param_count = (int)value_to_integer(&hash_var->value);
    }

    if (n < 0 || n > param_count) {
        fprintf(stderr, "opsh: shift: shift count out of range\n");
        return 1;
    }

    /* Shift: $1=$((n+1)), $2=$((n+2)), etc. */
    int i;
    for (i = 1; i <= param_count - n; i++) {
        char src_name[16], dst_name[16];
        snprintf(src_name, sizeof(src_name), "%d", i + n);
        snprintf(dst_name, sizeof(dst_name), "%d", i);
        variable_t *src = environ_get(vm->env, src_name);
        if (src != NULL) {
            environ_set(vm->env, dst_name, value_clone(&src->value));
        }
    }
    /* Unset the trailing parameters */
    for (i = param_count - n + 1; i <= param_count; i++) {
        char name[16];
        snprintf(name, sizeof(name), "%d", i);
        environ_unset(vm->env, name);
    }
    /* Update $# */
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", param_count - n);
        environ_set(vm->env, "#", value_string(rcstr_new(buf)));
    }
    return 0;
}

/* test / [ builtin -- runtime expression parser */
static int test_unary_file(const char *op, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 1;
    }
    if (strcmp(op, "-f") == 0) {
        return S_ISREG(st.st_mode) ? 0 : 1;
    }
    if (strcmp(op, "-d") == 0) {
        return S_ISDIR(st.st_mode) ? 0 : 1;
    }
    if (strcmp(op, "-e") == 0) {
        return 0;
    }
    if (strcmp(op, "-s") == 0) {
        return st.st_size > 0 ? 0 : 1;
    }
    if (strcmp(op, "-r") == 0) {
        return access(path, R_OK) == 0 ? 0 : 1;
    }
    if (strcmp(op, "-w") == 0) {
        return access(path, W_OK) == 0 ? 0 : 1;
    }
    if (strcmp(op, "-x") == 0) {
        return access(path, X_OK) == 0 ? 0 : 1;
    }
    return 1;
}

static int builtin_test(vm_t *vm, int argc, value_t *argv)
{
    (void)vm;
    int nargs = argc - 1;
    char *cmd_name = NULL;

    if (nargs <= 0) {
        return 1;
    }

    cmd_name = value_to_string(&argv[0]);
    /* If invoked as [, strip trailing ] */
    if (strcmp(cmd_name, "[") == 0) {
        rcstr_release(cmd_name);
        char *last = value_to_string(&argv[argc - 1]);
        if (strcmp(last, "]") != 0) {
            fprintf(stderr, "opsh: [: missing ]\n");
            rcstr_release(last);
            return 2;
        }
        rcstr_release(last);
        nargs--; /* exclude ] */
    } else {
        rcstr_release(cmd_name);
    }

    if (nargs == 0) {
        return 1;
    }

    /* One argument: true if non-empty string */
    if (nargs == 1) {
        char *s = value_to_string(&argv[1]);
        int result = (s[0] != '\0') ? 0 : 1;
        rcstr_release(s);
        return result;
    }

    /* Two arguments: unary operator */
    if (nargs == 2) {
        char *op = value_to_string(&argv[1]);
        char *arg = value_to_string(&argv[2]);
        int result = 1;

        if (strcmp(op, "-n") == 0) {
            result = (arg[0] != '\0') ? 0 : 1;
        } else if (strcmp(op, "-z") == 0) {
            result = (arg[0] == '\0') ? 0 : 1;
        } else if (strcmp(op, "!") == 0) {
            result = (arg[0] != '\0') ? 1 : 0;
        } else if (op[0] == '-') {
            result = test_unary_file(op, arg);
        }

        rcstr_release(op);
        rcstr_release(arg);
        return result;
    }

    /* Three arguments: binary operator */
    if (nargs == 3) {
        char *left = value_to_string(&argv[1]);
        char *op = value_to_string(&argv[2]);
        char *right = value_to_string(&argv[3]);
        int result = 1;

        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            result = (strcmp(left, right) == 0) ? 0 : 1;
        } else if (strcmp(op, "!=") == 0) {
            result = (strcmp(left, right) != 0) ? 0 : 1;
        } else if (strcmp(op, "-eq") == 0) {
            result = (strtoll(left, NULL, 10) == strtoll(right, NULL, 10)) ? 0 : 1;
        } else if (strcmp(op, "-ne") == 0) {
            result = (strtoll(left, NULL, 10) != strtoll(right, NULL, 10)) ? 0 : 1;
        } else if (strcmp(op, "-lt") == 0) {
            result = (strtoll(left, NULL, 10) < strtoll(right, NULL, 10)) ? 0 : 1;
        } else if (strcmp(op, "-le") == 0) {
            result = (strtoll(left, NULL, 10) <= strtoll(right, NULL, 10)) ? 0 : 1;
        } else if (strcmp(op, "-gt") == 0) {
            result = (strtoll(left, NULL, 10) > strtoll(right, NULL, 10)) ? 0 : 1;
        } else if (strcmp(op, "-ge") == 0) {
            result = (strtoll(left, NULL, 10) >= strtoll(right, NULL, 10)) ? 0 : 1;
        }

        rcstr_release(left);
        rcstr_release(op);
        rcstr_release(right);
        return result;
    }

    /* Four arguments: ! expr */
    if (nargs == 4) {
        char *bang = value_to_string(&argv[1]);
        if (strcmp(bang, "!") == 0) {
            rcstr_release(bang);
            /* Recursively evaluate the inner 3-arg expression.
             * Use "test" as argv[0] to prevent double-]-stripping. */
            value_t inner_argv[4];
            inner_argv[0] = value_string(rcstr_new("test"));
            inner_argv[1] = argv[2];
            inner_argv[2] = argv[3];
            inner_argv[3] = argv[4];
            int inner = builtin_test(vm, 4, inner_argv);
            value_destroy(&inner_argv[0]);
            return (inner == 0) ? 1 : 0;
        }
        rcstr_release(bang);
    }

    return 1;
}

static int builtin_printf(vm_t *vm, int argc, value_t *argv)
{
    (void)vm;
    if (argc < 2) {
        return 0;
    }

    char *fmt = value_to_string(&argv[1]);
    int arg_idx = 2;
    strbuf_t out;
    strbuf_init(&out);

    const char *p = fmt;
    while (*p) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n':
                strbuf_append_byte(&out, '\n');
                p++;
                break;
            case 't':
                strbuf_append_byte(&out, '\t');
                p++;
                break;
            case '\\':
                strbuf_append_byte(&out, '\\');
                p++;
                break;
            case '\0':
                strbuf_append_byte(&out, '\\');
                break;
            default:
                strbuf_append_byte(&out, '\\');
                strbuf_append_byte(&out, *p);
                p++;
                break;
            }
        } else if (*p == '%') {
            p++;
            if (*p == 's') {
                if (arg_idx < argc) {
                    char *s = value_to_string(&argv[arg_idx++]);
                    strbuf_append_str(&out, s);
                    rcstr_release(s);
                }
                p++;
            } else if (*p == 'd') {
                if (arg_idx < argc) {
                    int64_t n = value_to_integer(&argv[arg_idx++]);
                    strbuf_append_printf(&out, "%" PRId64, n);
                }
                p++;
            } else if (*p == '%') {
                strbuf_append_byte(&out, '%');
                p++;
            } else {
                strbuf_append_byte(&out, '%');
            }
        } else {
            strbuf_append_byte(&out, *p);
            p++;
        }
    }

    write_fd1(out.contents, out.length);
    strbuf_destroy(&out);
    rcstr_release(fmt);
    return 0;
}

static int builtin_read(vm_t *vm, int argc, value_t *argv)
{
    strbuf_t line;
    strbuf_init(&line);

    /* Read a line from stdin */
    {
        char c;
        ssize_t n;
        while ((n = read(STDIN_FILENO, &c, 1)) == 1) {
            if (c == '\n') {
                break;
            }
            strbuf_append_byte(&line, c);
        }
        if (n <= 0 && line.length == 0) {
            strbuf_destroy(&line);
            return 1;
        }
    }

    if (argc <= 1) {
        /* No variable names: store in REPLY */
        char *tmp = strbuf_detach(&line);
        environ_assign(vm->env, "REPLY", value_string(rcstr_new(tmp)));
        free(tmp);
        return 0;
    }

    /* Split by IFS (default: space/tab/newline) and assign to variables */
    {
        int var_idx = 1;
        const char *p = line.contents;

        while (var_idx < argc) {
            /* Skip leading IFS whitespace */
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '\0') {
                break;
            }

            if (var_idx == argc - 1) {
                /* Last variable gets the remainder */
                char *name = value_to_string(&argv[var_idx]);
                environ_assign(vm->env, name, value_string(rcstr_new(p)));
                rcstr_release(name);
                var_idx++;
                break;
            }

            /* Find next IFS delimiter */
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }

            size_t len = (size_t)(p - start);
            char *name = value_to_string(&argv[var_idx]);
            char *val = rcstr_from_buf(start, len);
            environ_assign(vm->env, name, value_string(val));
            rcstr_release(name);
            var_idx++;
        }

        /* Set remaining variables to empty */
        while (var_idx < argc) {
            char *name = value_to_string(&argv[var_idx]);
            environ_assign(vm->env, name, value_string(rcstr_new("")));
            rcstr_release(name);
            var_idx++;
        }
    }

    strbuf_destroy(&line);
    return 0;
}

static int builtin_return(vm_t *vm, int argc, value_t *argv)
{
    int code = 0;
    if (argc > 1) {
        code = (int)value_to_integer(&argv[1]);
    }
    vm->laststatus = code;
    vm->return_requested = true;
    return code;
}

static int builtin_type(vm_t *vm, int argc, value_t *argv)
{
    int i;
    int result = 0;
    for (i = 1; i < argc; i++) {
        char *name = value_to_string(&argv[i]);
        int bi = builtin_lookup(name);
        if (bi >= 0) {
            strbuf_t out;
            strbuf_init(&out);
            strbuf_append_str(&out, name);
            strbuf_append_str(&out, " is a shell builtin\n");
            write_fd1(out.contents, out.length);
            strbuf_destroy(&out);
        } else {
            /* Check functions */
            int fi;
            bool found = false;
            for (fi = 0; fi < vm->func_count; fi++) {
                if (strcmp(vm->func_table[fi].name, name) == 0) {
                    strbuf_t out;
                    strbuf_init(&out);
                    strbuf_append_str(&out, name);
                    strbuf_append_str(&out, " is a function\n");
                    write_fd1(out.contents, out.length);
                    strbuf_destroy(&out);
                    found = true;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "opsh: type: %s: not found\n", name);
                result = 1;
            }
        }
        rcstr_release(name);
    }
    return result;
}

const builtin_entry_t builtin_table[] = {
    {"echo", builtin_echo},
    {"exit", builtin_exit},
    {"true", builtin_true},
    {"false", builtin_false},
    {":", builtin_colon},
    {"cd", builtin_cd},
    {"pwd", builtin_pwd},
    {"export", builtin_export},
    {"unset", builtin_unset},
    {"readonly", builtin_readonly},
    {"local", builtin_local},
    {"shift", builtin_shift},
    {"test", builtin_test},
    {"[", builtin_test},
    {"printf", builtin_printf},
    {"read", builtin_read},
    {"return", builtin_return},
    {"type", builtin_type},
    {NULL, NULL},
};

int builtin_lookup(const char *name)
{
    int i;
    for (i = 0; builtin_table[i].name != NULL; i++) {
        if (strcmp(builtin_table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

int builtin_count(void)
{
    int i = 0;
    while (builtin_table[i].name != NULL) {
        i++;
    }
    return i;
}
