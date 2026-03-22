#include "builtins/builtins.h"

#include "foundation/rcstr.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    /* Always write to fd 1 so redirections take effect */
    {
        size_t written = 0;
        while (written < out.length) {
            ssize_t n = write(STDOUT_FILENO, out.contents + written, out.length - written);
            if (n <= 0) {
                break;
            }
            written += (size_t)n;
        }
    }

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

const builtin_entry_t builtin_table[] = {
    {"echo", builtin_echo},   {"exit", builtin_exit}, {"true", builtin_true},
    {"false", builtin_false}, {":", builtin_colon},   {NULL, NULL},
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
