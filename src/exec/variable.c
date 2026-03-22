#include "exec/variable.h"

#include "foundation/util.h"

#include <stdlib.h>
#include <string.h>

static variable_t *variable_new(const char *name, value_t value, unsigned int flags)
{
    variable_t *var = xcalloc(1, sizeof(*var));
    var->name = xmalloc(strlen(name) + 1);
    strcpy(var->name, name);
    var->value = value;
    var->flags = flags;
    var->dirty = false;
    var->host_origin = false;
    return var;
}

static void variable_free(variable_t *var)
{
    if (var == NULL) {
        return;
    }
    value_destroy(&var->value);
    free(var->name);
    free(var);
}

environ_t *environ_new(environ_t *parent, bool is_temporary)
{
    environ_t *env = xcalloc(1, sizeof(*env));
    ht_init(&env->vars);
    env->parent = parent;
    env->is_temporary = is_temporary;
    return env;
}

static int free_var_cb(const char *key, void *value, void *ctx)
{
    (void)key;
    (void)ctx;
    variable_free(value);
    return 0;
}

void environ_destroy(environ_t *env)
{
    if (env == NULL) {
        return;
    }
    ht_foreach(&env->vars, free_var_cb, NULL);
    ht_destroy(&env->vars);
    free(env);
}

variable_t *environ_get(environ_t *env, const char *name)
{
    while (env != NULL) {
        variable_t *var = ht_get(&env->vars, name);
        if (var != NULL) {
            return var;
        }
        env = env->parent;
    }
    return NULL;
}

void environ_set(environ_t *env, const char *name, value_t value)
{
    environ_set_flags(env, name, value, 0);
}

void environ_set_flags(environ_t *env, const char *name, value_t value, unsigned int flags)
{
    variable_t *existing = ht_get(&env->vars, name);
    if (existing != NULL) {
        value_destroy(&existing->value);
        existing->value = value;
        existing->flags |= flags;
        return;
    }

    variable_t *var = variable_new(name, value, flags);
    /* The hashtable borrows the key pointer; we use var->name which we own */
    ht_set(&env->vars, var->name, var);
}

void environ_assign(environ_t *env, const char *name, value_t value)
{
    /* Walk the scope chain to find an existing variable */
    environ_t *scope = env;
    while (scope != NULL) {
        variable_t *var = ht_get(&scope->vars, name);
        if (var != NULL) {
            value_destroy(&var->value);
            var->value = value;
            return;
        }
        scope = scope->parent;
    }
    /* Not found anywhere -- create in the current scope */
    environ_set(env, name, value);
}

void environ_export(environ_t *env, const char *name)
{
    variable_t *var = environ_get(env, name);
    if (var != NULL) {
        var->flags |= VF_EXPORT;
    }
}

void environ_unset(environ_t *env, const char *name)
{
    while (env != NULL) {
        variable_t *var = ht_remove(&env->vars, name);
        if (var != NULL) {
            variable_free(var);
            return;
        }
        env = env->parent;
    }
}
