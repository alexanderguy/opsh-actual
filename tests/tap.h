#ifndef TAP_H
#define TAP_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAP_UNUSED __attribute__((unused))

static int tap__test_num TAP_UNUSED = 0;
static int tap__plan TAP_UNUSED = 0;
static int tap__failures TAP_UNUSED = 0;

TAP_UNUSED static void tap_plan(int n)
{
    tap__plan = n;
    printf("1..%d\n", n);
}

TAP_UNUSED static void tap_ok(int expr, const char *desc)
{
    tap__test_num++;
    if (expr) {
        printf("ok %d - %s\n", tap__test_num, desc);
    } else {
        printf("not ok %d - %s\n", tap__test_num, desc);
        tap__failures++;
    }
}

TAP_UNUSED static void tap_is_int(long long got, long long expected, const char *desc)
{
    tap__test_num++;
    if (got == expected) {
        printf("ok %d - %s\n", tap__test_num, desc);
    } else {
        printf("not ok %d - %s\n", tap__test_num, desc);
        printf("#   got:      %lld\n", got);
        printf("#   expected: %lld\n", expected);
        tap__failures++;
    }
}

TAP_UNUSED static void tap_is_str(const char *got, const char *expected, const char *desc)
{
    tap__test_num++;
    int pass = (got == NULL && expected == NULL) ||
               (got != NULL && expected != NULL && strcmp(got, expected) == 0);
    if (pass) {
        printf("ok %d - %s\n", tap__test_num, desc);
    } else {
        printf("not ok %d - %s\n", tap__test_num, desc);
        printf("#   got:      \"%s\"\n", got ? got : "(null)");
        printf("#   expected: \"%s\"\n", expected ? expected : "(null)");
        tap__failures++;
    }
}

TAP_UNUSED static void tap_skip(int n, const char *reason)
{
    int i;
    for (i = 0; i < n; i++) {
        tap__test_num++;
        printf("ok %d - # SKIP %s\n", tap__test_num, reason);
    }
}

TAP_UNUSED static void tap_bail_out(const char *msg)
{
    printf("Bail out! %s\n", msg);
    exit(1);
}

TAP_UNUSED static void tap_diag(const char *msg)
{
    printf("# %s\n", msg);
}

TAP_UNUSED static int tap_done(void)
{
    if (tap__test_num != tap__plan) {
        printf("# planned %d tests but ran %d\n", tap__plan, tap__test_num);
        return 1;
    }
    if (tap__failures > 0) {
        printf("# %d test(s) failed\n", tap__failures);
        return 1;
    }
    return 0;
}

#endif /* TAP_H */
