#include "tap.h"

int main(void)
{
    tap_plan(6);

    tap_ok(1, "true is ok");
    tap_ok(!0, "not-zero is ok");

    tap_is_int(42, 42, "integers match");
    tap_is_int(-1, -1, "negative integers match");

    tap_is_str("hello", "hello", "strings match");
    tap_is_str(NULL, NULL, "null strings match");

    return tap_done();
}
