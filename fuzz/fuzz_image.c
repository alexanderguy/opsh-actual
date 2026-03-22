/*
 * libfuzzer harness for .opsb deserialization.
 *
 * Feeds arbitrary bytes to image_read_opsb via fmemopen.
 * Catches crashes and memory errors in the binary format parser.
 */
#include "vm/image_io.h"
#include "vm/vm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FILE *f = fmemopen((void *)data, size, "rb");
    if (!f)
        return 0;

    bytecode_image_t *img = image_read_opsb(f);
    if (img)
        image_free(img);

    fclose(f);
    return 0;
}
