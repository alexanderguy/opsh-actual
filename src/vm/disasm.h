#ifndef OPSH_VM_DISASM_H
#define OPSH_VM_DISASM_H

#include "vm/vm.h"

#include <stdio.h>

/* Disassemble an entire bytecode image to the given file (typically stdout/stderr) */
void disasm_image(const bytecode_image_t *img, FILE *out);

/* Disassemble a single instruction at the given offset. Returns the next offset. */
size_t disasm_instruction(const bytecode_image_t *img, size_t offset, FILE *out);

#endif /* OPSH_VM_DISASM_H */
