#ifndef OPSH_COMPILER_COMPILER_H
#define OPSH_COMPILER_COMPILER_H

#include "foundation/hashtable.h"
#include "parser/ast.h"
#include "vm/vm.h"

#include <stdbool.h>

/*
 * Compiler state.
 */
typedef struct {
    bytecode_image_t *image;
    int error_count;
    const char *filename;

    /* Local variable tracking for compile-time scope resolution */
    int func_depth;     /* >0 when compiling inside a function body */
    hashtable_t locals; /* variable name -> (void*)1 when inside a function */
    bool locals_active; /* true when locals hashtable is initialized */
} compiler_t;

/* Compile a parsed program into a bytecode image.
 * Returns NULL on failure. Caller owns the returned image. */
bytecode_image_t *compile(sh_list_t *program, const char *filename);

#endif /* OPSH_COMPILER_COMPILER_H */
