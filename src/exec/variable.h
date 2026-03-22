#ifndef OPSH_EXEC_VARIABLE_H
#define OPSH_EXEC_VARIABLE_H

#include "foundation/hashtable.h"
#include "opsh/value.h"

#include <stdbool.h>

/*
 * Variable flags.
 */
#define VF_EXPORT (1 << 0)
#define VF_READONLY (1 << 1)

/*
 * A single variable entry.
 */
typedef struct variable {
    char *name; /* owned copy of the variable name (used as hashtable key) */
    value_t value;
    unsigned int flags;
    bool dirty;       /* for $REPLY: set when written, checked on RET */
    bool host_origin; /* true if imported from host environment */
} variable_t;

/*
 * A variable scope. Scopes form a linked list (stack).
 * Each scope has a hashtable mapping variable names to variable_t.
 */
typedef struct environ {
    hashtable_t vars; /* name -> variable_t* (owned) */
    struct environ *parent;
    bool is_temporary; /* true for temporary assignment scopes */
} environ_t;

/* Create a new scope, optionally linked to a parent */
environ_t *environ_new(environ_t *parent, bool is_temporary);

/* Destroy a scope and all its variables (does NOT touch parent) */
void environ_destroy(environ_t *env);

/* Look up a variable by name, walking the scope chain. Returns NULL if not found. */
variable_t *environ_get(environ_t *env, const char *name);

/* Set a variable in the current (top) scope. Creates it if it doesn't exist.
 * Takes ownership of the value. */
void environ_set(environ_t *env, const char *name, value_t value);

/* Set a variable, walking the scope chain to find an existing definition.
 * If found in a parent scope, modifies it there. Otherwise creates in
 * the current scope. This is the POSIX assignment behavior. */
void environ_assign(environ_t *env, const char *name, value_t value);

/* Set a variable in the current scope with specific flags. */
void environ_set_flags(environ_t *env, const char *name, value_t value, unsigned int flags);

/* Mark a variable for export. Searches the scope chain. */
void environ_export(environ_t *env, const char *name);

/* Remove a variable from the innermost scope that contains it. */
void environ_unset(environ_t *env, const char *name);

#endif /* OPSH_EXEC_VARIABLE_H */
