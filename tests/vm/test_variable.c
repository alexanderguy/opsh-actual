#include "../tap.h"
#include "exec/variable.h"
#include "foundation/rcstr.h"
#include "opsh/value.h"

#include <stdlib.h>
#include <string.h>

static void test_basic_set_get(void)
{
    environ_t *env = environ_new(NULL, false);

    environ_set(env, "FOO", value_string(rcstr_new("bar")));
    variable_t *var = environ_get(env, "FOO");
    tap_ok(var != NULL, "set/get: variable found");
    tap_is_int(var->value.type, VT_STRING, "set/get: type is string");
    tap_is_str(var->value.data.string, "bar", "set/get: value is bar");

    tap_ok(environ_get(env, "NOTSET") == NULL, "set/get: missing returns NULL");

    environ_destroy(env);
}

static void test_overwrite(void)
{
    environ_t *env = environ_new(NULL, false);

    environ_set(env, "X", value_string(rcstr_new("old")));
    environ_set(env, "X", value_string(rcstr_new("new")));
    variable_t *var = environ_get(env, "X");
    tap_is_str(var->value.data.string, "new", "overwrite: value updated");

    environ_destroy(env);
}

static void test_scope_chain(void)
{
    environ_t *global = environ_new(NULL, false);
    environ_set(global, "G", value_string(rcstr_new("global")));

    environ_t *local = environ_new(global, false);
    environ_set(local, "L", value_string(rcstr_new("local")));

    /* Local scope sees both */
    tap_ok(environ_get(local, "L") != NULL, "scope: local var visible");
    tap_ok(environ_get(local, "G") != NULL, "scope: global var visible from local");

    /* Global scope doesn't see local */
    tap_ok(environ_get(global, "L") == NULL, "scope: local var not visible from global");

    /* Shadow a global variable */
    environ_set(local, "G", value_string(rcstr_new("shadowed")));
    tap_is_str(environ_get(local, "G")->value.data.string, "shadowed",
               "scope: shadowed value in local");
    tap_is_str(environ_get(global, "G")->value.data.string, "global",
               "scope: original value in global");

    environ_destroy(local);
    environ_destroy(global);
}

static void test_unset(void)
{
    environ_t *env = environ_new(NULL, false);
    environ_set(env, "DEL", value_string(rcstr_new("gone")));
    environ_unset(env, "DEL");
    tap_ok(environ_get(env, "DEL") == NULL, "unset: variable removed");

    /* Unset nonexistent is a no-op */
    environ_unset(env, "NOPE");
    tap_ok(1, "unset: nonexistent is no-op");

    environ_destroy(env);
}

static void test_export(void)
{
    environ_t *env = environ_new(NULL, false);
    environ_set(env, "E", value_string(rcstr_new("exported")));
    environ_export(env, "E");
    variable_t *var = environ_get(env, "E");
    tap_ok((var->flags & VF_EXPORT) != 0, "export: flag set");

    environ_destroy(env);
}

static void test_integer_value(void)
{
    environ_t *env = environ_new(NULL, false);
    environ_set(env, "N", value_integer(42));
    variable_t *var = environ_get(env, "N");
    tap_is_int(var->value.type, VT_INTEGER, "integer: type correct");
    tap_is_int((long long)var->value.data.integer, 42, "integer: value correct");

    environ_destroy(env);
}

static void test_temporary_scope(void)
{
    environ_t *global = environ_new(NULL, false);
    environ_set(global, "X", value_string(rcstr_new("original")));

    environ_t *temp = environ_new(global, true);
    tap_ok(temp->is_temporary, "temp scope: flag set");
    environ_set(temp, "X", value_string(rcstr_new("temp")));
    tap_is_str(environ_get(temp, "X")->value.data.string, "temp", "temp scope: shadowed value");

    environ_destroy(temp);
    tap_is_str(environ_get(global, "X")->value.data.string, "original",
               "temp scope: original restored after destroy");

    environ_destroy(global);
}

int main(void)
{
    tap_plan(18);

    test_basic_set_get();
    test_overwrite();
    test_scope_chain();
    test_unset();
    test_export();
    test_integer_value();
    test_temporary_scope();

    return tap_done();
}
