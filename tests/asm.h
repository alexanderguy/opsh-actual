#ifndef OPSH_TESTS_ASM_H
#define OPSH_TESTS_ASM_H

#include "compiler/bytecode.h"
#include "vm/vm.h"

/*
 * Bytecode assembly macros for hand-writing VM test programs.
 *
 * Usage:
 *   bytecode_image_t *img = image_new();
 *   uint16_t c_echo = image_add_const(img, "echo");
 *   uint16_t c_hello = image_add_const(img, "hello");
 *
 *   ASM_PUSH_CONST(img, c_echo);
 *   ASM_PUSH_CONST(img, c_hello);
 *   ASM_PUSH_INT(img, 2);       // argc = 2
 *   ASM_EXEC_BUILTIN(img, 0);   // echo
 *   ASM_HALT(img);
 */

#define ASM_OP(img, opcode) image_emit_u8((img), (uint8_t)(opcode))

#define ASM_PUSH_CONST(img, idx)                                                                   \
    do {                                                                                           \
        ASM_OP((img), OP_PUSH_CONST);                                                              \
        image_emit_u16((img), (idx));                                                              \
    } while (0)

#define ASM_PUSH_INT(img, val)                                                                     \
    do {                                                                                           \
        ASM_OP((img), OP_PUSH_INT);                                                                \
        image_emit_i32((img), (int32_t)(val));                                                     \
    } while (0)

#define ASM_POP(img) ASM_OP((img), OP_POP)
#define ASM_DUP(img) ASM_OP((img), OP_DUP)

#define ASM_CONCAT(img, count)                                                                     \
    do {                                                                                           \
        ASM_OP((img), OP_CONCAT);                                                                  \
        image_emit_u16((img), (uint16_t)(count));                                                  \
    } while (0)

#define ASM_GET_SPECIAL(img, which)                                                                \
    do {                                                                                           \
        ASM_OP((img), OP_GET_SPECIAL);                                                             \
        image_emit_u8((img), (uint8_t)(which));                                                    \
    } while (0)

#define ASM_EXEC_BUILTIN(img, idx)                                                                 \
    do {                                                                                           \
        ASM_OP((img), OP_EXEC_BUILTIN);                                                            \
        image_emit_u16((img), (uint16_t)(idx));                                                    \
    } while (0)

#define ASM_JMP(img, offset)                                                                       \
    do {                                                                                           \
        ASM_OP((img), OP_JMP);                                                                     \
        image_emit_i32((img), (int32_t)(offset));                                                  \
    } while (0)

#define ASM_JMP_TRUE(img, offset)                                                                  \
    do {                                                                                           \
        ASM_OP((img), OP_JMP_TRUE);                                                                \
        image_emit_i32((img), (int32_t)(offset));                                                  \
    } while (0)

#define ASM_JMP_FALSE(img, offset)                                                                 \
    do {                                                                                           \
        ASM_OP((img), OP_JMP_FALSE);                                                               \
        image_emit_i32((img), (int32_t)(offset));                                                  \
    } while (0)

#define ASM_JMP_NONE(img, offset)                                                                  \
    do {                                                                                           \
        ASM_OP((img), OP_JMP_NONE);                                                                \
        image_emit_i32((img), (int32_t)(offset));                                                  \
    } while (0)

#define ASM_INIT_ITER(img, group_count)                                                            \
    do {                                                                                           \
        ASM_OP((img), OP_INIT_ITER);                                                               \
        image_emit_u16((img), (uint16_t)(group_count));                                            \
    } while (0)

#define ASM_GET_NEXT_ITER(img) ASM_OP((img), OP_GET_NEXT_ITER)

#define ASM_REDIR_SAVE(img) ASM_OP((img), OP_REDIR_SAVE)
#define ASM_REDIR_RESTORE(img) ASM_OP((img), OP_REDIR_RESTORE)

#define ASM_HALT(img) ASM_OP((img), OP_HALT)

#define ASM_RET(img) ASM_OP((img), OP_RET)

/* Builtin indices */
#define BUILTIN_ECHO 0
#define BUILTIN_EXIT 1

#endif /* OPSH_TESTS_ASM_H */
