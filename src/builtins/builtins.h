#ifndef OPSH_BUILTINS_BUILTINS_H
#define OPSH_BUILTINS_BUILTINS_H

#include "opsh/value.h"

#include <stddef.h>

/* Forward declaration to avoid circular include with vm.h */
struct vm;

/*
 * Builtin function signature.
 * argc includes the command name (argv[0]).
 * Returns the exit status.
 */
typedef int (*builtin_fn)(struct vm *vm, int argc, value_t *argv);

/*
 * Builtin registry entry.
 */
typedef struct {
    const char *name;
    builtin_fn fn;
} builtin_entry_t;

/* The global builtin table (NULL-name terminated) */
extern const builtin_entry_t builtin_table[];

/* Look up a builtin by name. Returns the index, or -1 if not found. */
int builtin_lookup(const char *name);

/* Number of registered builtins */
int builtin_count(void);

#endif /* OPSH_BUILTINS_BUILTINS_H */
