#include "exec/variable.h"

#include "foundation/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static variable_t *variable_new(const char *name, value_t value, unsigned int flags)
{
    variable_t *var = xcalloc(1, sizeof(*var));
    var->name = xstrdup(name);
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

static bool check_readonly(environ_t *env, const char *name)
{
    variable_t *var = environ_get(env, name);
    if (var != NULL && (var->flags & VF_READONLY)) {
        fprintf(stderr, "opsh: %s: readonly variable\n", name);
        return true;
    }
    return false;
}

void environ_set(environ_t *env, const char *name, value_t value)
{
    environ_set_flags(env, name, value, 0);
}

void environ_set_flags(environ_t *env, const char *name, value_t value, unsigned int flags)
{
    variable_t *existing = ht_get(&env->vars, name);
    if (existing != NULL) {
        if (existing->flags & VF_READONLY) {
            fprintf(stderr, "opsh: %s: readonly variable\n", name);
            value_destroy(&value);
            return;
        }
        value_destroy(&existing->value);
        existing->value = value;
        existing->flags |= flags;
        return;
    }

    /* Check parent scopes for readonly */
    if (env->parent != NULL && check_readonly(env->parent, name)) {
        value_destroy(&value);
        return;
    }

    variable_t *var = variable_new(name, value, flags);
    ht_set(&env->vars, var->name, var);
}

void environ_assign(environ_t *env, const char *name, value_t value)
{
    environ_t *scope = env;
    while (scope != NULL) {
        variable_t *var = ht_get(&scope->vars, name);
        if (var != NULL) {
            if (var->flags & VF_READONLY) {
                fprintf(stderr, "opsh: %s: readonly variable\n", name);
                value_destroy(&value);
                return;
            }
            value_destroy(&var->value);
            var->value = value;
            return;
        }
        scope = scope->parent;
    }
    environ_set(env, name, value);
}

void environ_set_local(environ_t *env, const char *name, value_t value)
{
    variable_t *existing = ht_get(&env->vars, name);
    if (existing != NULL) {
        value_destroy(&existing->value);
        existing->value = value;
        return;
    }
    variable_t *var = variable_new(name, value, 0);
    ht_set(&env->vars, var->name, var);
}

void environ_export(environ_t *env, const char *name)
{
    variable_t *var = environ_get(env, name);
    if (var != NULL) {
        var->flags |= VF_EXPORT;
    }
}

struct export_ctx {
    hashtable_t seen; /* track which names we've already exported */
};

static int export_var_cb(const char *key, void *value, void *ctx)
{
    variable_t *var = value;
    struct export_ctx *ec = ctx;
    (void)key;

    if (!(var->flags & (VF_EXPORT | VF_PREFIX))) {
        return 0;
    }
    /* Skip internal shell variables (positional params, special vars) */
    if (var->name[0] >= '0' && var->name[0] <= '9') {
        return 0;
    }
    if (var->name[0] == '#' || var->name[0] == '?' || var->name[0] == '-') {
        return 0;
    }
    /* Skip if already exported from an inner scope */
    if (ht_get(&ec->seen, var->name) != NULL) {
        return 0;
    }
    ht_set(&ec->seen, var->name, (void *)1);

    if (var->value.type == VT_STRING) {
        setenv(var->name, var->value.data.string, 1);
    }
    return 0;
}

void environ_export_to_c(environ_t *env)
{
    struct export_ctx ctx;
    ht_init(&ctx.seen);

    /* Walk innermost-out so inner scopes win */
    environ_t *scope = env;
    while (scope != NULL) {
        ht_foreach(&scope->vars, export_var_cb, &ctx);
        scope = scope->parent;
    }

    ht_destroy(&ctx.seen);
}

void environ_unset(environ_t *env, const char *name)
{
    while (env != NULL) {
        variable_t *var = ht_get(&env->vars, name);
        if (var != NULL) {
            if (var->flags & VF_READONLY) {
                fprintf(stderr, "opsh: %s: readonly variable\n", name);
                return;
            }
            ht_remove(&env->vars, name);
            variable_free(var);
            return;
        }
        env = env->parent;
    }
}
