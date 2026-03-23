#include "../tap.h"
#include "foundation/util.h"
#include "vm/image_io.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_roundtrip_empty(void)
{
    bytecode_image_t *img = image_new();
    image_emit_u8(img, OP_HALT);

    FILE *f = tmpfile();
    tap_ok(f != NULL, "empty roundtrip: tmpfile");

    int wret = image_write_opsb(img, f);
    tap_is_int(wret, 0, "empty roundtrip: write ok");

    rewind(f);
    bytecode_image_t *loaded = image_read_opsb(f);
    fclose(f);

    tap_ok(loaded != NULL, "empty roundtrip: read ok");
    tap_is_int(loaded->code_size, img->code_size, "empty roundtrip: code size matches");
    tap_is_int(loaded->const_count, 0, "empty roundtrip: no constants");
    tap_is_int(loaded->func_count, 0, "empty roundtrip: no functions");

    image_free(loaded);
    image_free(img);
}

static void test_roundtrip_constants(void)
{
    bytecode_image_t *img = image_new();
    uint16_t c0 = image_add_const(img, "hello");
    uint16_t c1 = image_add_const(img, "world");
    uint16_t c2 = image_add_const(img, "");

    image_emit_u8(img, OP_PUSH_CONST);
    image_emit_u16(img, c0);
    image_emit_u8(img, OP_PUSH_CONST);
    image_emit_u16(img, c1);
    image_emit_u8(img, OP_PUSH_CONST);
    image_emit_u16(img, c2);
    image_emit_u8(img, OP_HALT);

    FILE *f = tmpfile();
    image_write_opsb(img, f);
    rewind(f);
    bytecode_image_t *loaded = image_read_opsb(f);
    fclose(f);

    tap_ok(loaded != NULL, "constants roundtrip: read ok");
    tap_is_int(loaded->const_count, 3, "constants roundtrip: 3 constants");
    tap_is_str(loaded->const_pool[0], "hello", "constants roundtrip: const 0");
    tap_is_str(loaded->const_pool[1], "world", "constants roundtrip: const 1");
    tap_is_str(loaded->const_pool[2], "", "constants roundtrip: const 2 (empty)");
    tap_is_int(loaded->code_size, img->code_size, "constants roundtrip: code size");

    /* Verify bytecode matches */
    tap_ok(memcmp(loaded->code, img->code, img->code_size) == 0,
           "constants roundtrip: bytecode matches");

    image_free(loaded);
    image_free(img);
}

static void test_roundtrip_functions(void)
{
    bytecode_image_t *img = image_new();

    /* Add a function entry */
    img->funcs = xcalloc(1, sizeof(vm_func_t));
    img->func_count = 1;
    img->funcs[0].name = xstrdup("myfunc");
    img->funcs[0].bytecode_offset = 42;
    img->funcs[0].image = NULL;

    /* Need the function name in the constant pool for serialization */
    image_add_const(img, "myfunc");
    image_emit_u8(img, OP_HALT);

    FILE *f = tmpfile();
    image_write_opsb(img, f);
    rewind(f);
    bytecode_image_t *loaded = image_read_opsb(f);
    fclose(f);

    tap_ok(loaded != NULL, "functions roundtrip: read ok");
    tap_is_int(loaded->func_count, 1, "functions roundtrip: 1 function");
    tap_is_str(loaded->funcs[0].name, "myfunc", "functions roundtrip: name");
    tap_is_int((int)loaded->funcs[0].bytecode_offset, 42, "functions roundtrip: offset");

    image_free(loaded);
    image_free(img);
}

static void test_invalid_magic(void)
{
    /* Write garbage and try to load */
    FILE *f = tmpfile();
    fwrite("NOT_OPSB_DATA", 1, 13, f);
    rewind(f);

    bytecode_image_t *loaded = image_read_opsb(f);
    fclose(f);

    tap_ok(loaded == NULL, "invalid magic: returns NULL");
}

static void test_truncated(void)
{
    /* Write a valid image then truncate it */
    bytecode_image_t *img = image_new();
    image_add_const(img, "test");
    image_emit_u8(img, OP_HALT);

    FILE *f = tmpfile();
    image_write_opsb(img, f);
    long size = ftell(f);

    /* Rewrite with only half the data */
    rewind(f);
    char *buf = malloc((size_t)size);
    fread(buf, 1, (size_t)size, f);
    fclose(f);

    f = tmpfile();
    fwrite(buf, 1, (size_t)(size / 2), f);
    rewind(f);
    free(buf);

    bytecode_image_t *loaded = image_read_opsb(f);
    fclose(f);

    tap_ok(loaded == NULL, "truncated: returns NULL");

    image_free(img);
}

static void test_const_dedup(void)
{
    bytecode_image_t *img = image_new();
    uint16_t c0 = image_add_const(img, "same");
    uint16_t c1 = image_add_const(img, "same");
    uint16_t c2 = image_add_const(img, "different");

    tap_is_int(c0, c1, "dedup: same string gets same index");
    tap_ok(c2 != c0, "dedup: different string gets different index");
    tap_is_int(img->const_count, 2, "dedup: only 2 unique constants");

    image_free(img);
}

int main(void)
{
    tap_plan(22);

    test_roundtrip_empty();
    test_roundtrip_constants();
    test_roundtrip_functions();
    test_invalid_magic();
    test_truncated();
    test_const_dedup();

    tap_done();
    return 0;
}
