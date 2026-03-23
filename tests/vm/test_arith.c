#include "../tap.h"
#include "exec/variable.h"
#include "foundation/rcstr.h"
#include "vm/arith.h"

#include <stdlib.h>
#include <string.h>

static int64_t eval(const char *expr, environ_t *env)
{
    arith_error_t err = ARITH_OK;
    int64_t result = arith_eval(expr, env, &err);
    (void)err;
    return result;
}

static arith_error_t eval_err(const char *expr, environ_t *env)
{
    arith_error_t err = ARITH_OK;
    arith_eval(expr, env, &err);
    return err;
}

static void test_basic_ops(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int((int)eval("2+3)", env), 5, "2+3 = 5");
    tap_is_int((int)eval("10-3)", env), 7, "10-3 = 7");
    tap_is_int((int)eval("4*5)", env), 20, "4*5 = 20");
    tap_is_int((int)eval("15/4)", env), 3, "15/4 = 3");
    tap_is_int((int)eval("17%5)", env), 2, "17%5 = 2");

    environ_destroy(env);
}

static void test_precedence(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int((int)eval("2+3*4)", env), 14, "2+3*4 = 14 (mul before add)");
    tap_is_int((int)eval("(1+2)*3)", env), 9, "(1+2)*3 = 9 (parens)");
    tap_is_int((int)eval("2*3+4*5)", env), 26, "2*3+4*5 = 26");
    tap_is_int((int)eval("10-2-3)", env), 5, "10-2-3 = 5 (left assoc)");

    environ_destroy(env);
}

static void test_variables(void)
{
    environ_t *env = environ_new(NULL, false);
    environ_set(env, "x", value_string(rcstr_new("5")));
    environ_set(env, "y", value_string(rcstr_new("3")));

    tap_is_int((int)eval("x+1)", env), 6, "x+1 = 6");
    tap_is_int((int)eval("x*y)", env), 15, "x*y = 15");
    tap_is_int((int)eval("x+y*2)", env), 11, "x+y*2 = 11");

    environ_destroy(env);
}

static void test_assignment(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int((int)eval("x=5)", env), 5, "x=5 returns 5");
    variable_t *v = environ_get(env, "x");
    tap_ok(v != NULL, "x is set");

    tap_is_int((int)eval("x+=3)", env), 8, "x+=3 returns 8");
    tap_is_int((int)eval("x)", env), 8, "x is now 8");

    environ_destroy(env);
}

static void test_unary(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int((int)eval("-5)", env), -5, "-5 = -5");
    tap_is_int((int)eval("!0)", env), 1, "!0 = 1");
    tap_is_int((int)eval("!1)", env), 0, "!1 = 0");
    tap_is_int((int)eval("~0)", env), -1, "~0 = -1");

    environ_destroy(env);
}

static void test_comparison(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int((int)eval("5==5)", env), 1, "5==5 = 1");
    tap_is_int((int)eval("5!=3)", env), 1, "5!=3 = 1");
    tap_is_int((int)eval("3<5)", env), 1, "3<5 = 1");
    tap_is_int((int)eval("5>3)", env), 1, "5>3 = 1");
    tap_is_int((int)eval("5<=5)", env), 1, "5<=5 = 1");
    tap_is_int((int)eval("5>=6)", env), 0, "5>=6 = 0");

    environ_destroy(env);
}

static void test_logical(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int((int)eval("1&&1)", env), 1, "1&&1 = 1");
    tap_is_int((int)eval("1&&0)", env), 0, "1&&0 = 0");
    tap_is_int((int)eval("0||1)", env), 1, "0||1 = 1");
    tap_is_int((int)eval("0||0)", env), 0, "0||0 = 0");

    environ_destroy(env);
}

static void test_short_circuit(void)
{
    environ_t *env = environ_new(NULL, false);
    environ_set(env, "x", value_string(rcstr_new("5")));

    eval("0 && (x=99))", env);
    tap_is_int((int)eval("x)", env), 5, "&& short-circuit: x unchanged");

    eval("1 || (x=99))", env);
    tap_is_int((int)eval("x)", env), 5, "|| short-circuit: x unchanged");

    environ_destroy(env);
}

static void test_ternary(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int((int)eval("1 ? 2 : 3)", env), 2, "1?2:3 = 2");
    tap_is_int((int)eval("0 ? 2 : 3)", env), 3, "0?2:3 = 3");

    environ_destroy(env);
}

static void test_bitwise(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int((int)eval("5&3)", env), 1, "5&3 = 1");
    tap_is_int((int)eval("5|3)", env), 7, "5|3 = 7");
    tap_is_int((int)eval("5^3)", env), 6, "5^3 = 6");
    tap_is_int((int)eval("1<<3)", env), 8, "1<<3 = 8");
    tap_is_int((int)eval("-8>>2)", env), -2, "-8>>2 = -2 (arithmetic)");

    environ_destroy(env);
}

static void test_increment(void)
{
    environ_t *env = environ_new(NULL, false);
    environ_set(env, "x", value_string(rcstr_new("5")));

    tap_is_int((int)eval("++x)", env), 6, "++x returns 6");
    tap_is_int((int)eval("x)", env), 6, "x is now 6");
    tap_is_int((int)eval("x++)", env), 6, "x++ returns 6");
    tap_is_int((int)eval("x)", env), 7, "x is now 7");

    environ_destroy(env);
}

static void test_comma(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int((int)eval("1, 2, 3)", env), 3, "1,2,3 = 3");

    environ_destroy(env);
}

static void test_hex_octal(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int((int)eval("0xFF)", env), 255, "0xFF = 255");
    tap_is_int((int)eval("010)", env), 8, "010 = 8 (octal)");

    environ_destroy(env);
}

static void test_errors(void)
{
    environ_t *env = environ_new(NULL, false);

    tap_is_int(eval_err("1/0)", env), ARITH_ERR_DIV_ZERO, "div by zero");
    tap_is_int(eval_err("1%0)", env), ARITH_ERR_DIV_ZERO, "mod by zero");
    tap_is_int(eval_err("@)", env), ARITH_ERR_SYNTAX, "syntax error");

    /* Recursive depth */
    environ_set(env, "a", value_string(rcstr_new("a")));
    tap_is_int(eval_err("a)", env), ARITH_ERR_DEPTH, "recursion depth");

    environ_destroy(env);
}

static void test_empty(void)
{
    environ_t *env = environ_new(NULL, false);

    /* Empty expression (with trailing paren from lexer) */
    tap_is_int((int)eval(")", env), 0, "empty = 0");
    tap_is_int(eval_err(")", env), ARITH_OK, "empty is OK");

    environ_destroy(env);
}

int main(void)
{
    tap_plan(52);

    test_basic_ops();
    test_precedence();
    test_variables();
    test_assignment();
    test_unary();
    test_comparison();
    test_logical();
    test_short_circuit();
    test_ternary();
    test_bitwise();
    test_increment();
    test_comma();
    test_hex_octal();
    test_errors();
    test_empty();

    tap_done();
    return 0;
}
