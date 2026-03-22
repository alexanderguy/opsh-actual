#include "builtins/builtins.h"

#include "exec/signal.h"
#include "exec/variable.h"
#include "foundation/rcstr.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "vm/vm.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void write_fd1(const char *data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(STDOUT_FILENO, data + written, len - written);
        if (n > 0) {
            written += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
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

/* Shared file test used by both test/[ builtin and [[ ]] VM handler */
int test_file(const char *op, const char *path)
{
    /* -L and -h use lstat; everything else uses stat */
    struct stat st;
    int sr;
    if (strcmp(op, "-L") == 0 || strcmp(op, "-h") == 0) {
        sr = lstat(path, &st);
    } else {
        sr = stat(path, &st);
    }
    if (sr != 0) {
        return 1;
    }
    if (strcmp(op, "-f") == 0)
        return S_ISREG(st.st_mode) ? 0 : 1;
    if (strcmp(op, "-d") == 0)
        return S_ISDIR(st.st_mode) ? 0 : 1;
    if (strcmp(op, "-e") == 0)
        return 0;
    if (strcmp(op, "-s") == 0)
        return (st.st_size > 0) ? 0 : 1;
    if (strcmp(op, "-r") == 0)
        return (access(path, R_OK) == 0) ? 0 : 1;
    if (strcmp(op, "-w") == 0)
        return (access(path, W_OK) == 0) ? 0 : 1;
    if (strcmp(op, "-x") == 0)
        return (access(path, X_OK) == 0) ? 0 : 1;
    if (strcmp(op, "-L") == 0 || strcmp(op, "-h") == 0)
        return S_ISLNK(st.st_mode) ? 0 : 1;
    if (strcmp(op, "-p") == 0)
        return S_ISFIFO(st.st_mode) ? 0 : 1;
    if (strcmp(op, "-b") == 0)
        return S_ISBLK(st.st_mode) ? 0 : 1;
    if (strcmp(op, "-c") == 0)
        return S_ISCHR(st.st_mode) ? 0 : 1;
    if (strcmp(op, "-S") == 0)
        return S_ISSOCK(st.st_mode) ? 0 : 1;
    if (strcmp(op, "-g") == 0)
        return (st.st_mode & S_ISGID) ? 0 : 1;
    if (strcmp(op, "-u") == 0)
        return (st.st_mode & S_ISUID) ? 0 : 1;
    if (strcmp(op, "-k") == 0)
        return (st.st_mode & S_ISVTX) ? 0 : 1;
    if (strcmp(op, "-O") == 0)
        return (st.st_uid == geteuid()) ? 0 : 1;
    if (strcmp(op, "-G") == 0)
        return (st.st_gid == getegid()) ? 0 : 1;
    if (strcmp(op, "-N") == 0)
        return 0; /* always true if file exists */
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
            result = test_file(op, arg);
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
        bool got_eof = false;
        for (;;) {
            n = read(STDIN_FILENO, &c, 1);
            if (n == 1) {
                if (c == '\n') {
                    break;
                }
                strbuf_append_byte(&line, c);
            } else if (n == 0) {
                got_eof = true;
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                got_eof = true;
                break;
            }
        }
        if (got_eof && line.length == 0) {
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

static int signame_to_num(const char *name)
{
    if (strcmp(name, "EXIT") == 0) {
        return 0;
    }
    if (strcmp(name, "INT") == 0 || strcmp(name, "SIGINT") == 0) {
        return SIGINT;
    }
    if (strcmp(name, "TERM") == 0 || strcmp(name, "SIGTERM") == 0) {
        return SIGTERM;
    }
    if (strcmp(name, "HUP") == 0 || strcmp(name, "SIGHUP") == 0) {
        return SIGHUP;
    }
    if (strcmp(name, "QUIT") == 0 || strcmp(name, "SIGQUIT") == 0) {
        return SIGQUIT;
    }
    if (strcmp(name, "KILL") == 0 || strcmp(name, "SIGKILL") == 0) {
        return SIGKILL;
    }
    if (strcmp(name, "USR1") == 0 || strcmp(name, "SIGUSR1") == 0) {
        return SIGUSR1;
    }
    if (strcmp(name, "USR2") == 0 || strcmp(name, "SIGUSR2") == 0) {
        return SIGUSR2;
    }
    if (strcmp(name, "PIPE") == 0 || strcmp(name, "SIGPIPE") == 0) {
        return SIGPIPE;
    }
    if (strcmp(name, "ALRM") == 0 || strcmp(name, "SIGALRM") == 0) {
        return SIGALRM;
    }
    /* Try numeric */
    char *endp;
    long num = strtol(name, &endp, 10);
    if (*endp == '\0' && num >= 0 && num < 32) {
        return (int)num;
    }
    return -1;
}

static int builtin_trap(vm_t *vm, int argc, value_t *argv)
{
    if (argc < 2) {
        return 0;
    }

    /* trap - SIGNAL: reset to default */
    /* trap '' SIGNAL: ignore */
    /* trap 'cmd' SIGNAL: register handler */
    char *action = value_to_string(&argv[1]);

    if (argc == 2) {
        /* trap -l or trap with just a signal name -- not supported */
        rcstr_release(action);
        return 0;
    }

    int i;
    for (i = 2; i < argc; i++) {
        char *signame = value_to_string(&argv[i]);
        int signum = signame_to_num(signame);
        if (signum < 0) {
            fprintf(stderr, "opsh: trap: %s: invalid signal\n", signame);
            rcstr_release(signame);
            rcstr_release(action);
            return 1;
        }

        if (signum == 0) {
            /* EXIT trap */
            free(vm->exit_trap);
            if (strcmp(action, "-") == 0) {
                vm->exit_trap = NULL;
            } else {
                vm->exit_trap = xstrdup(action);
            }
        } else if (signum < 32) {
            free(vm->trap_handlers[signum]);
            if (strcmp(action, "-") == 0) {
                vm->trap_handlers[signum] = NULL;
            } else {
                vm->trap_handlers[signum] = xstrdup(action);
            }
        }
        rcstr_release(signame);
    }

    rcstr_release(action);
    return 0;
}

static int builtin_eval(vm_t *vm, int argc, value_t *argv)
{
    if (argc <= 1) {
        return 0;
    }

    /* Concatenate all arguments with spaces */
    strbuf_t cmd;
    strbuf_init(&cmd);
    int i;
    for (i = 1; i < argc; i++) {
        if (i > 1) {
            strbuf_append_byte(&cmd, ' ');
        }
        char *s = value_to_string(&argv[i]);
        strbuf_append_str(&cmd, s);
        free(s);
    }

    char *str = strbuf_detach(&cmd);
    int status = vm_exec_string(vm, str, "eval");
    free(str);
    return status;
}

static int builtin_dot(vm_t *vm, int argc, value_t *argv)
{
    if (argc < 2) {
        fprintf(stderr, "opsh: .: filename argument required\n");
        return 2;
    }

    char *filename = value_to_string(&argv[1]);
    char *source = read_file(filename);
    if (source == NULL) {
        fprintf(stderr, "opsh: .: %s: not found\n", filename);
        free(filename);
        return 1;
    }

    int status = vm_exec_string(vm, source, filename);
    free(source);
    free(filename);
    return status;
}

static int builtin_command(vm_t *vm, int argc, value_t *argv)
{
    if (argc < 2) {
        return 0;
    }

    char *arg1 = value_to_string(&argv[1]);

    /* command -v name: print how name would be resolved */
    if (strcmp(arg1, "-v") == 0) {
        free(arg1);
        if (argc < 3) {
            return 1;
        }
        char *name = value_to_string(&argv[2]);
        int bi = builtin_lookup(name);
        if (bi >= 0) {
            strbuf_t out;
            strbuf_init(&out);
            strbuf_append_str(&out, name);
            strbuf_append_byte(&out, '\n');
            write_fd1(out.contents, out.length);
            strbuf_destroy(&out);
            free(name);
            return 0;
        }
        /* Check functions */
        int fi;
        for (fi = 0; fi < vm->func_count; fi++) {
            if (strcmp(vm->func_table[fi].name, name) == 0) {
                strbuf_t out;
                strbuf_init(&out);
                strbuf_append_str(&out, name);
                strbuf_append_byte(&out, '\n');
                write_fd1(out.contents, out.length);
                strbuf_destroy(&out);
                free(name);
                return 0;
            }
        }
        /* Search PATH for external command */
        {
            const char *path_env = getenv("PATH");
            if (path_env != NULL) {
                const char *p = path_env;
                while (*p) {
                    const char *sep = strchr(p, ':');
                    size_t dir_len = sep ? (size_t)(sep - p) : strlen(p);
                    if (dir_len > 0) {
                        char path[4096];
                        snprintf(path, sizeof(path), "%.*s/%s", (int)dir_len, p, name);
                        if (access(path, X_OK) == 0) {
                            strbuf_t out;
                            strbuf_init(&out);
                            strbuf_append_str(&out, path);
                            strbuf_append_byte(&out, '\n');
                            write_fd1(out.contents, out.length);
                            strbuf_destroy(&out);
                            free(name);
                            return 0;
                        }
                    }
                    p = sep ? sep + 1 : p + strlen(p);
                }
            }
        }
        free(name);
        return 1;
    }

    free(arg1);

    /* command name args...: run bypassing function lookup */
    char *cmd_name = value_to_string(&argv[1]);

    /* Try builtins first */
    int bi = builtin_lookup(cmd_name);
    if (bi >= 0) {
        const char *bname = builtin_table[bi].name;
        free(cmd_name);
        int status = builtin_table[bi].fn(vm, argc - 1, argv + 1);
        vm->laststatus = status;
        if (strcmp(bname, "exit") == 0) {
            vm->exit_requested = true;
            vm_exit(vm, status);
        }
        return status;
    }

    /* External command: fork/execvp */
    {
        int i;
        char **exec_argv = xcalloc((size_t)argc, sizeof(char *));
        exec_argv[0] = cmd_name;
        for (i = 2; i < argc; i++) {
            exec_argv[i - 1] = value_to_string(&argv[i]);
        }
        exec_argv[argc - 1] = NULL;

        fflush(stdout);
        fflush(stderr);
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "opsh: fork: %s\n", strerror(errno));
            for (i = 1; i < argc - 1; i++) {
                rcstr_release(exec_argv[i]);
            }
            free(exec_argv);
            free(cmd_name);
            return 126;
        }
        if (pid == 0) {
            signal_reset();
            execvp(exec_argv[0], exec_argv);
            int err = errno;
            fprintf(stderr, "opsh: %s: %s\n", exec_argv[0], strerror(err));
            _exit(err == ENOENT ? 127 : 126);
        }

        /* Parent */
        int wstatus = 0;
        pid_t wp;
        while ((wp = waitpid(pid, &wstatus, 0)) < 0 && errno == EINTR) {
            /* retry */
        }
        int status;
        if (wp < 0) {
            status = 127;
        } else if (WIFEXITED(wstatus)) {
            status = WEXITSTATUS(wstatus);
        } else if (WIFSIGNALED(wstatus)) {
            status = 128 + WTERMSIG(wstatus);
        } else {
            status = 1;
        }

        for (i = 1; i < argc - 1; i++) {
            rcstr_release(exec_argv[i]);
        }
        free(exec_argv);
        free(cmd_name);
        return status;
    }
}

static int builtin_exec(vm_t *vm, int argc, value_t *argv)
{
    if (argc < 2) {
        /* exec without args: redirections are made permanent by the
         * compiler suppressing OP_REDIR_RESTORE. Nothing to do here. */
        return 0;
    }

    char **exec_argv = xcalloc((size_t)argc, sizeof(char *));
    int i;
    for (i = 1; i < argc; i++) {
        exec_argv[i - 1] = value_to_string(&argv[i]);
    }
    exec_argv[argc - 1] = NULL;

    signal_reset();
    execvp(exec_argv[0], exec_argv);

    int err = errno;
    fprintf(stderr, "opsh: exec: %s: %s\n", exec_argv[0], strerror(err));
    /* Restore signal handlers since exec failed */
    signal_init();

    for (i = 0; i < argc - 1; i++) {
        rcstr_release(exec_argv[i]);
    }
    free(exec_argv);

    (void)vm;
    return (err == ENOENT) ? 127 : 126;
}

static int builtin_wait(vm_t *vm, int argc, value_t *argv)
{
    (void)vm;
    if (argc < 2) {
        /* Wait for all children */
        int last_status = 0;
        int wstatus;
        pid_t wp;
        while ((wp = waitpid(-1, &wstatus, 0)) > 0 || (wp < 0 && errno == EINTR)) {
            if (wp < 0) {
                continue;
            }
            if (WIFEXITED(wstatus)) {
                last_status = WEXITSTATUS(wstatus);
            } else if (WIFSIGNALED(wstatus)) {
                last_status = 128 + WTERMSIG(wstatus);
            }
        }
        return last_status;
    }

    /* Wait for specific PID */
    char *pid_str = value_to_string(&argv[1]);
    char *endp;
    long pval = strtol(pid_str, &endp, 10);
    if (*endp != '\0') {
        fprintf(stderr, "opsh: wait: %s: invalid pid\n", pid_str);
        free(pid_str);
        return 2;
    }
    pid_t pid = (pid_t)pval;
    free(pid_str);

    int wstatus;
    pid_t wp;
    while ((wp = waitpid(pid, &wstatus, 0)) < 0 && errno == EINTR) {
        /* retry */
    }
    if (wp < 0) {
        return 127;
    }
    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
    }
    if (WIFSIGNALED(wstatus)) {
        return 128 + WTERMSIG(wstatus);
    }
    return 1;
}

static int builtin_kill(vm_t *vm, int argc, value_t *argv)
{
    (void)vm;
    if (argc < 2) {
        fprintf(stderr, "opsh: kill: usage: kill [-signal] pid...\n");
        return 2;
    }

    char *arg1 = value_to_string(&argv[1]);

    /* kill -l: list signals */
    if (strcmp(arg1, "-l") == 0) {
        free(arg1);
        write_fd1("HUP INT QUIT TERM KILL USR1 USR2\n", 33);
        return 0;
    }

    /* Parse signal: default is SIGTERM */
    int signo = SIGTERM;
    int pid_start = 1;

    if (arg1[0] == '-' && arg1[1] != '\0') {
        /* -signal or -SIGNAME */
        int sn = signame_to_num(arg1 + 1);
        if (sn < 0) {
            fprintf(stderr, "opsh: kill: %s: invalid signal\n", arg1 + 1);
            free(arg1);
            return 2;
        }
        signo = sn;
        pid_start = 2;
    }
    free(arg1);

    if (pid_start >= argc) {
        fprintf(stderr, "opsh: kill: no pid specified\n");
        return 2;
    }

    int result = 0;
    int i;
    for (i = pid_start; i < argc; i++) {
        char *ps = value_to_string(&argv[i]);
        char *endp;
        long pval = strtol(ps, &endp, 10);
        if (*endp != '\0') {
            fprintf(stderr, "opsh: kill: %s: invalid pid\n", ps);
            free(ps);
            result = 1;
            continue;
        }
        pid_t pid = (pid_t)pval;
        free(ps);
        if (kill(pid, signo) != 0) {
            fprintf(stderr, "opsh: kill: %d: %s\n", (int)pid, strerror(errno));
            result = 1;
        }
    }
    return result;
}

static int builtin_umask(vm_t *vm, int argc, value_t *argv)
{
    (void)vm;
    if (argc < 2) {
        mode_t m = umask(0);
        umask(m);
        char buf[16];
        snprintf(buf, sizeof(buf), "%04o\n", (unsigned)m);
        write_fd1(buf, strlen(buf));
        return 0;
    }

    char *arg = value_to_string(&argv[1]);
    char *endp;
    unsigned long val = strtoul(arg, &endp, 8);
    if (*endp != '\0' || val > 0777) {
        fprintf(stderr, "opsh: umask: %s: invalid mask\n", arg);
        free(arg);
        return 1;
    }
    umask((mode_t)val);
    free(arg);
    return 0;
}

static void update_option_flags(vm_t *vm)
{
    char *p = vm->option_flags;
    if (vm->opt_errexit) {
        *p++ = 'e';
    }
    if (vm->opt_nounset) {
        *p++ = 'u';
    }
    if (vm->opt_xtrace) {
        *p++ = 'x';
    }
    *p = '\0';
}

static int builtin_set(vm_t *vm, int argc, value_t *argv)
{
    if (argc < 2) {
        return 0;
    }

    int i;
    for (i = 1; i < argc; i++) {
        char *arg = value_to_string(&argv[i]);

        /* set -- args: replace positional parameters */
        if (strcmp(arg, "--") == 0) {
            free(arg);
            /* Clear existing positional params */
            variable_t *hash_var = environ_get(vm->env, "#");
            int old_count = 0;
            if (hash_var != NULL) {
                old_count = (int)value_to_integer(&hash_var->value);
            }
            {
                int pi;
                for (pi = 1; pi <= old_count; pi++) {
                    char name[16];
                    snprintf(name, sizeof(name), "%d", pi);
                    environ_unset(vm->env, name);
                }
            }
            /* Set new positional params from remaining args */
            int new_count = argc - i - 1;
            if (new_count > 0) {
                char **new_args = xcalloc((size_t)new_count, sizeof(char *));
                int ai;
                for (ai = 0; ai < new_count; ai++) {
                    new_args[ai] = value_to_string(&argv[i + 1 + ai]);
                }
                vm_set_args(vm, new_count, new_args);
                for (ai = 0; ai < new_count; ai++) {
                    free(new_args[ai]);
                }
                free(new_args);
            } else {
                char *zero = xstrdup("0");
                environ_set(vm->env, "#", value_string(zero));
            }
            /* Reset OPTIND */
            environ_assign(vm->env, "OPTIND", value_string(xstrdup("1")));
            update_option_flags(vm);
            return 0;
        }

        /* Option flags: -eux or +eux */
        if ((arg[0] == '-' || arg[0] == '+') && arg[1] != '\0') {
            bool enable = (arg[0] == '-');
            const char *p = arg + 1;
            while (*p) {
                switch (*p) {
                case 'e':
                    vm->opt_errexit = enable;
                    break;
                case 'u':
                    vm->opt_nounset = enable;
                    break;
                case 'x':
                    vm->opt_xtrace = enable;
                    break;
                default:
                    fprintf(stderr, "opsh: set: %c%c: invalid option\n", arg[0], *p);
                    free(arg);
                    return 2;
                }
                p++;
            }
            free(arg);
            continue;
        }

        free(arg);
    }

    update_option_flags(vm);
    return 0;
}

static int builtin_getopts(vm_t *vm, int argc, value_t *argv)
{
    if (argc < 3) {
        fprintf(stderr, "opsh: getopts: usage: getopts optstring varname [args]\n");
        return 2;
    }

    char *optstring = value_to_string(&argv[1]);
    char *varname = value_to_string(&argv[2]);

    /* Get OPTIND */
    variable_t *optind_var = environ_get(vm->env, "OPTIND");
    int optind = 1;
    if (optind_var != NULL) {
        optind = (int)value_to_integer(&optind_var->value);
    }

    /* Determine the argument source */
    int src_argc;
    value_t *src_argv;
    if (argc > 3) {
        src_argc = argc - 3;
        src_argv = argv + 3;
    } else {
        /* Use positional parameters */
        variable_t *hash_var = environ_get(vm->env, "#");
        int param_count = 0;
        if (hash_var != NULL) {
            param_count = (int)value_to_integer(&hash_var->value);
        }
        src_argc = param_count;
        src_argv = NULL; /* use environ_get for $1..$N */
    }

    bool silent = (optstring[0] == ':');
    const char *opts = silent ? optstring + 1 : optstring;

    if (optind < 1 || optind > src_argc) {
        /* No more options */
        environ_assign(vm->env, varname, value_string(xstrdup("?")));
        free(optstring);
        free(varname);
        return 1;
    }

    /* Get the current argument */
    char *cur_arg;
    if (src_argv != NULL) {
        cur_arg = value_to_string(&src_argv[optind - 1]);
    } else {
        char pname[16];
        snprintf(pname, sizeof(pname), "%d", optind);
        variable_t *pv = environ_get(vm->env, pname);
        cur_arg = pv ? value_to_string(&pv->value) : xstrdup("");
    }

    /* Track position within bundled options (e.g., -abc) */
    int optpos = 1; /* default: first char after - */
    {
        variable_t *pos_var = environ_get(vm->env, "_OPTPOS");
        if (pos_var != NULL) {
            int pv = (int)value_to_integer(&pos_var->value);
            if (pv > 1) {
                optpos = pv;
            }
        }
    }

    if (cur_arg[0] != '-' || cur_arg[1] == '\0' || strcmp(cur_arg, "--") == 0) {
        environ_assign(vm->env, varname, value_string(xstrdup("?")));
        environ_assign(vm->env, "_OPTPOS", value_string(xstrdup("1")));
        free(cur_arg);
        free(optstring);
        free(varname);
        return 1;
    }

    char opt_ch = cur_arg[optpos];
    if (opt_ch == '\0') {
        /* Past end of this arg, move to next */
        environ_assign(vm->env, "_OPTPOS", value_string(xstrdup("1")));
        environ_assign(vm->env, varname, value_string(xstrdup("?")));
        free(cur_arg);
        free(optstring);
        free(varname);
        return 1;
    }
    const char *found = strchr(opts, opt_ch);

    if (found == NULL) {
        /* Invalid option */
        if (!silent) {
            fprintf(stderr, "opsh: illegal option -- %c\n", opt_ch);
        }
        char optstr[2] = {opt_ch, '\0'};
        environ_assign(vm->env, varname, value_string(xstrdup("?")));
        if (silent) {
            environ_assign(vm->env, "OPTARG", value_string(xstrdup(optstr)));
        }
    } else if (found[1] == ':') {
        /* Option requires argument */
        if (cur_arg[optpos + 1] != '\0') {
            /* Argument attached: -fvalue or bundled -bfvalue */
            char optstr[2] = {opt_ch, '\0'};
            environ_assign(vm->env, varname, value_string(xstrdup(optstr)));
            environ_assign(vm->env, "OPTARG", value_string(xstrdup(cur_arg + optpos + 1)));
        } else if (optind < src_argc) {
            /* Argument is next word */
            optind++;
            char *next_arg;
            if (src_argv != NULL) {
                next_arg = value_to_string(&src_argv[optind - 1]);
            } else {
                char pname[16];
                snprintf(pname, sizeof(pname), "%d", optind);
                variable_t *pv = environ_get(vm->env, pname);
                next_arg = pv ? value_to_string(&pv->value) : xstrdup("");
            }
            char optstr[2] = {opt_ch, '\0'};
            environ_assign(vm->env, varname, value_string(xstrdup(optstr)));
            environ_assign(vm->env, "OPTARG", value_string(next_arg));
        } else {
            /* Missing argument */
            if (!silent) {
                fprintf(stderr, "opsh: option requires an argument -- %c\n", opt_ch);
            }
            if (silent) {
                char optstr[2] = {opt_ch, '\0'};
                environ_assign(vm->env, varname, value_string(xstrdup(":")));
                environ_assign(vm->env, "OPTARG", value_string(xstrdup(optstr)));
            } else {
                environ_assign(vm->env, varname, value_string(xstrdup("?")));
            }
        }
    } else {
        /* Simple option, no argument */
        char optstr[2] = {opt_ch, '\0'};
        environ_assign(vm->env, varname, value_string(xstrdup(optstr)));
        environ_assign(vm->env, "OPTARG", value_string(xstrdup("")));
    }

    /* Advance position within bundled options or move to next arg */
    if (opt_ch != '\0' && cur_arg[optpos + 1] != '\0' && (found == NULL || found[1] != ':')) {
        /* More options bundled in this arg */
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", optpos + 1);
        environ_assign(vm->env, "_OPTPOS", value_string(xstrdup(buf)));
    } else {
        /* Move to next argument */
        environ_assign(vm->env, "_OPTPOS", value_string(xstrdup("1")));
        optind++;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", optind);
        environ_assign(vm->env, "OPTIND", value_string(xstrdup(buf)));
    }

    free(cur_arg);
    free(optstring);
    free(varname);
    return 0;
}

const builtin_entry_t builtin_table[] = {
    {"echo", builtin_echo},         {"exit", builtin_exit},       {"true", builtin_true},
    {"false", builtin_false},       {":", builtin_true},          {"cd", builtin_cd},
    {"pwd", builtin_pwd},           {"export", builtin_export},   {"unset", builtin_unset},
    {"readonly", builtin_readonly}, {"local", builtin_local},     {"shift", builtin_shift},
    {"test", builtin_test},         {"[", builtin_test},          {"printf", builtin_printf},
    {"read", builtin_read},         {"return", builtin_return},   {"type", builtin_type},
    {"trap", builtin_trap},         {"eval", builtin_eval},       {".", builtin_dot},
    {"source", builtin_dot},        {"command", builtin_command}, {"exec", builtin_exec},
    {"wait", builtin_wait},         {"kill", builtin_kill},       {"umask", builtin_umask},
    {"set", builtin_set},           {"getopts", builtin_getopts}, {NULL, NULL},
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
