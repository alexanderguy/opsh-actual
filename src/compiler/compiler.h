#ifndef OPSH_COMPILER_COMPILER_H
#define OPSH_COMPILER_COMPILER_H

#include "foundation/hashtable.h"
#include "foundation/plist.h"
#include "parser/ast.h"
#include "vm/vm.h"

#include <stdbool.h>

#define MAX_LOOP_DEPTH 64
#define MAX_FUNC_COUNT 256

/*
 * Loop context for break/continue backpatching.
 */
typedef struct {
    size_t continue_target; /* bytecode offset of loop condition */
    plist_t break_patches;  /* plist of (size_t*) patch positions */
} loop_info_t;

/*
 * Function table entry.
 */
typedef struct {
    char *name;
    size_t bytecode_offset;
} func_entry_t;

/*
 * Compiler state.
 */
typedef struct {
    bytecode_image_t *image;
    int error_count;
    const char *filename;

    loop_info_t loop_stack[MAX_LOOP_DEPTH];
    int loop_depth;

    func_entry_t func_table[MAX_FUNC_COUNT];
    int func_count;

    /* Local variable tracking for compile-time scope resolution */
    int func_depth;     /* >0 when compiling inside a function body */
    hashtable_t locals; /* variable name -> (void*)1 when inside a function */
    bool locals_active; /* true when locals hashtable is initialized */
} compiler_t;

/* Compile a parsed program into a bytecode image.
 * Returns NULL on failure. Caller owns the returned image. */
bytecode_image_t *compile(sh_list_t *program, const char *filename);

#endif /* OPSH_COMPILER_COMPILER_H */
