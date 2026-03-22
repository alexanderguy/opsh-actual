#ifndef OPSH_COMPILER_BYTECODE_H
#define OPSH_COMPILER_BYTECODE_H

#include <stdint.h>

/*
 * Opcode definitions for the opsh bytecode VM.
 *
 * Instructions are variable-width: 1-byte opcode followed by zero or more
 * operands. Multi-byte operands are little-endian.
 *
 * Operand types:
 *   u8  -- 1-byte unsigned
 *   u16 -- 2-byte unsigned (constant pool index, name index)
 *   i32 -- 4-byte signed (jump offsets, relative to current ip)
 *   u32 -- 4-byte unsigned (bytecode segment offsets)
 */
typedef enum {
    /* Stack operations */
    OP_PUSH_CONST = 0x01, /* u16 pool_idx -> push string */
    OP_PUSH_INT = 0x02,   /* i32 value -> push integer */
    OP_POP = 0x03,        /* value -> discard */
    OP_DUP = 0x04,        /* a -> a a */

    /* Variable operations */
    OP_GET_VAR = 0x10,        /* u16 name_idx -> push value */
    OP_SET_VAR = 0x11,        /* u16 name_idx; value -> */
    OP_GET_LOCAL = 0x19,      /* u16 name_idx -> push value (current scope only) */
    OP_SET_LOCAL = 0x1A,      /* u16 name_idx; value -> (current scope only) */
    OP_GET_ARRAY = 0x12,      /* u16 name_idx; index -> value */
    OP_SET_ARRAY = 0x13,      /* u16 name_idx; value index -> */
    OP_SET_ARRAY_BULK = 0x14, /* u16 name_idx, u16 count; vals -> */
    OP_EXPORT = 0x15,         /* u16 name_idx */
    OP_PUSH_SCOPE = 0x16,     /* u8 flags (0=local, 1=temp) */
    OP_POP_SCOPE = 0x17,
    OP_GET_SPECIAL = 0x18, /* u8 which -> push value */

    /* String and expansion operations */
    OP_CONCAT = 0x20, /* u16 count; strs -> result */
    /*
     * EXPAND_PARAM: u16 name_idx, u8 op, u8 flags
     *
     * Stack protocol by op:
     *   PE_NONE (0):   -> value          (no extra stack input)
     *   PE_DEFAULT (1):  word -> value     (pops 1: default word)
     *   PE_ALTERNATE (2):   word -> value     (pops 1: alternate word)
     *   PE_ASSIGN (3): word -> value     (pops 1: assign word; side effect: sets var)
     *   PE_ERROR (4):  word -> value     (pops 1: error message word)
     *   PE_TRIM (5):  pattern -> value  (pops 1: match pattern)
     *   PE_REPLACE (6):  repl pattern -> value (pops 2: pattern then replacement)
     *
     * When flags has PE_STRLEN: -> value (no extra; returns string length)
     */
    OP_EXPAND_PARAM = 0x21,
    OP_EXPAND_ARITH = 0x22,  /* expr_str -> value */
    OP_SPLIT_FIELDS = 0x23,  /* value -> values... count */
    OP_GLOB = 0x24,          /* pattern -> values... count */
    OP_QUOTE_REMOVE = 0x25,  /* value -> value */
    OP_EXPAND_TILDE = 0x26,  /* value -> value */
    OP_COLLECT_WORDS = 0x27, /* (val count)... ngroups -> args... argc */

    /* Control flow */
    OP_JMP = 0x30,       /* i32 offset */
    OP_JMP_TRUE = 0x31,  /* i32 offset; jump if laststatus == 0 */
    OP_JMP_FALSE = 0x32, /* i32 offset; jump if laststatus != 0 */
    OP_JMP_NONE = 0x33,  /* i32 offset; pop, jump if VT_NONE */
    OP_RET = 0x34,
    OP_LOOP_ENTER = 0x35,
    OP_LOOP_EXIT = 0x36,
    OP_BREAK = 0x37,         /* u8 depth */
    OP_CONTINUE = 0x38,      /* u8 depth */
    OP_GET_NEXT_ITER = 0x39, /* -> value (VT_NONE when exhausted) */
    OP_INIT_ITER = 0x3A,     /* u16 group_count */

    /* Command execution */
    OP_EXEC_SIMPLE = 0x40,  /* u8 flags */
    OP_EXEC_BUILTIN = 0x41, /* u16 builtin_idx */
    OP_EXEC_FUNC = 0x42,    /* u16 func_idx */
    OP_PIPELINE = 0x43,     /* u16 cmd_count */
    OP_PIPELINE_CMD = 0x44, /* u32 bytecode_offset */
    OP_PIPELINE_END = 0x45, /* u8 flags */
    OP_CMD_SUBST = 0x46,    /* u32 bytecode_offset */
    OP_SUBSHELL = 0x47,     /* u32 bytecode_offset */

    /* Redirection */
    OP_REDIR_SAVE = 0x50,
    OP_REDIR_RESTORE = 0x51,
    OP_REDIR_OPEN = 0x52,  /* u8 fd, u8 type */
    OP_REDIR_DUP = 0x53,   /* u8 target_fd, u8 source_fd */
    OP_REDIR_CLOSE = 0x54, /* u8 fd */
    OP_REDIR_HERE = 0x55,  /* u8 fd, u8 flags */

    /* Test operations (for [[ ]]) */
    OP_TEST_UNARY = 0x60,  /* u8 op */
    OP_TEST_BINARY = 0x61, /* u8 op */
    OP_NEGATE_STATUS = 0x62,
    OP_PATTERN_MATCH = 0x63,

    /* Module operations */
    OP_IMPORT = 0x70,    /* u16 module_name_idx */
    OP_CAP_CHECK = 0x71, /* u8 capability */

    /* Sentinel */
    OP_HALT = 0xFF,
} opcode_t;

/* Special variable identifiers for GET_SPECIAL */
typedef enum {
    SPECIAL_QUESTION = 0, /* $? */
    SPECIAL_HASH = 1,     /* $# */
    SPECIAL_AT = 2,       /* $@ */
    SPECIAL_STAR = 3,     /* $* */
    SPECIAL_DOLLAR = 4,   /* $$ */
    SPECIAL_BANG = 5,     /* $! */
    SPECIAL_DASH = 6,     /* $- */
    SPECIAL_ZERO = 7,     /* $0 */
} special_var_t;

/* Test operation identifiers for OP_TEST_UNARY / OP_TEST_BINARY */
typedef enum {
    /* Unary file tests */
    TEST_F = 0,   /* -f regular file */
    TEST_D = 1,   /* -d directory */
    TEST_E = 2,   /* -e exists */
    TEST_S = 3,   /* -s non-empty */
    TEST_R = 4,   /* -r readable */
    TEST_W = 5,   /* -w writable */
    TEST_X = 6,   /* -x executable */
    TEST_L = 7,   /* -L / -h symlink */
    TEST_P = 8,   /* -p pipe */
    TEST_B = 9,   /* -b block device */
    TEST_C = 10,  /* -c char device */
    TEST_SS = 11, /* -S socket */
    TEST_G = 12,  /* -g setgid */
    TEST_U = 13,  /* -u setuid */
    TEST_K = 14,  /* -k sticky */
    TEST_O = 15,  /* -O owned by euid */
    TEST_GG = 16, /* -G owned by egid */
    TEST_T = 17,  /* -t fd is terminal */
    TEST_NT = 18, /* -N modified since last read */
    /* Unary string tests */
    TEST_N = 20, /* -n non-empty string */
    TEST_Z = 21, /* -z empty string */
    /* Binary string tests */
    TEST_SEQ = 30, /* == or = */
    TEST_SNE = 31, /* != */
    /* Binary numeric tests */
    TEST_EQ = 40, /* -eq */
    TEST_NE = 41, /* -ne */
    TEST_LT = 42, /* -lt */
    TEST_LE = 43, /* -le */
    TEST_GT = 44, /* -gt */
    TEST_GE = 45, /* -ge */
    /* Binary file tests */
    TEST_FNT = 50, /* -nt newer than */
    TEST_FOT = 51, /* -ot older than */
    TEST_FEF = 52, /* -ef same file */
} test_op_t;

/* EXEC_SIMPLE flags */
#define EXEC_FLAG_FORK (1 << 0)
#define EXEC_FLAG_CAPTURE_OUT (1 << 1)

/* Instruction size helpers */
static inline int opcode_operand_size(opcode_t op)
{
    switch (op) {
    case OP_PUSH_CONST:
    case OP_GET_VAR:
    case OP_SET_VAR:
    case OP_GET_LOCAL:
    case OP_SET_LOCAL:
    case OP_GET_ARRAY:
    case OP_SET_ARRAY:
    case OP_EXPORT:
    case OP_CONCAT:
    case OP_EXEC_BUILTIN:
    case OP_EXEC_FUNC:
    case OP_PIPELINE:
    case OP_IMPORT:
    case OP_INIT_ITER:
        return 2; /* u16 */
    case OP_SET_ARRAY_BULK:
        return 4; /* u16 + u16 */
    case OP_EXPAND_PARAM:
        return 4; /* u16 + u8 + u8 */
    case OP_PUSH_INT:
    case OP_JMP:
    case OP_JMP_TRUE:
    case OP_JMP_FALSE:
    case OP_JMP_NONE:
        return 4; /* i32 */
    case OP_PIPELINE_CMD:
    case OP_CMD_SUBST:
    case OP_SUBSHELL:
        return 4; /* u32 */
    case OP_PUSH_SCOPE:
    case OP_GET_SPECIAL:
    case OP_EXEC_SIMPLE:
    case OP_BREAK:
    case OP_CONTINUE:
    case OP_PIPELINE_END:
    case OP_CAP_CHECK:
        return 1; /* u8 */
    case OP_REDIR_OPEN:
    case OP_REDIR_DUP:
    case OP_REDIR_HERE:
        return 2; /* u8 + u8 */
    case OP_REDIR_CLOSE:
    case OP_TEST_UNARY:
    case OP_TEST_BINARY:
        return 1; /* u8 */
    default:
        return 0; /* no operands */
    }
}

#endif /* OPSH_COMPILER_BYTECODE_H */
