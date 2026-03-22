#ifndef OPSH_VM_IMAGE_IO_H
#define OPSH_VM_IMAGE_IO_H

#include "vm/vm.h"

#include <stdio.h>

/*
 * .opsb file format (little-endian throughout):
 *
 * Header (16 bytes):
 *   4 bytes: magic "OPSH"
 *   2 bytes: format version (u16, currently 1)
 *   2 bytes: flags (u16, reserved)
 *   4 bytes: section count (u32)
 *   4 bytes: reserved
 *
 * Sections (repeated section_count times):
 *   1 byte:  section type
 *   4 bytes: section length in bytes (u32)
 *   N bytes: section data
 *
 * Section types:
 *   0x01: CONST_POOL  - (u16 count) + (u32 len, bytes)* for each string
 *   0x02: BYTECODE    - raw bytecode bytes
 *   0x03: FUNC_TABLE  - (u16 count) + (u16 name_idx, u32 offset)* per func
 *   0x04: MODULE_TABLE - (u16 count) + (u16 name_idx, u32 offset)* per module
 */

#define OPSB_MAGIC "OPSH"
#define OPSB_VERSION 1

#define SECT_CONST_POOL 0x01
#define SECT_BYTECODE 0x02
#define SECT_FUNC_TABLE 0x03
#define SECT_MODULE_TABLE 0x04

/* Serialize a bytecode image to a file. Returns 0 on success. */
int image_write_opsb(const bytecode_image_t *img, FILE *out);

/* Deserialize a bytecode image from a file. Returns NULL on error. */
bytecode_image_t *image_read_opsb(FILE *in);

#endif /* OPSH_VM_IMAGE_IO_H */
