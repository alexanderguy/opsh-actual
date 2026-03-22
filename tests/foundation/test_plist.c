#include "../tap.h"
#include "foundation/plist.h"

int main(void)
{
    plist_t list;
    char a = 'a', b = 'b', c = 'c', d = 'd';

    tap_plan(10);

    plist_init(&list);
    tap_is_int((long long)list.length, 0, "init: length is 0");

    /* Add elements */
    plist_add(&list, &a);
    plist_add(&list, &b);
    plist_add(&list, &c);
    tap_is_int((long long)list.length, 3, "add: length is 3");
    tap_ok(plist_get(&list, 0) == &a, "add: element 0 is a");
    tap_ok(plist_get(&list, 1) == &b, "add: element 1 is b");
    tap_ok(plist_get(&list, 2) == &c, "add: element 2 is c");

    /* Remove from middle */
    {
        void *removed = plist_remove(&list, 1);
        tap_ok(removed == &b, "remove: returns removed element");
        tap_is_int((long long)list.length, 2, "remove: length is 2");
        tap_ok(plist_get(&list, 0) == &a, "remove: element 0 is a");
        tap_ok(plist_get(&list, 1) == &c, "remove: element 1 is c (shifted)");
    }

    /* Clear */
    plist_add(&list, &d);
    plist_clear(&list);
    tap_is_int((long long)list.length, 0, "clear: length is 0");

    plist_destroy(&list);

    return tap_done();
}
