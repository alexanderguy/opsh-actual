#include "vm/disasm.h"

#include <stdio.h>

static uint8_t peek_u8(const bytecode_image_t *img, size_t offset)
{
    if (offset >= img->code_size) {
        return 0;
    }
    return img->code[offset];
}

static uint16_t peek_u16(const bytecode_image_t *img, size_t offset)
{
    uint16_t lo = peek_u8(img, offset);
    uint16_t hi = peek_u8(img, offset + 1);
    return lo | (hi << 8);
}

static int32_t peek_i32(const bytecode_image_t *img, size_t offset)
{
    uint32_t b0 = peek_u8(img, offset);
    uint32_t b1 = peek_u8(img, offset + 1);
    uint32_t b2 = peek_u8(img, offset + 2);
    uint32_t b3 = peek_u8(img, offset + 3);
    return (int32_t)(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
}

static const char *opcode_name(uint8_t op)
{
    switch ((opcode_t)op) {
    case OP_PUSH_CONST:
        return "PUSH_CONST";
    case OP_PUSH_INT:
        return "PUSH_INT";
    case OP_POP:
        return "POP";
    case OP_DUP:
        return "DUP";
    case OP_GET_VAR:
        return "GET_VAR";
    case OP_SET_VAR:
        return "SET_VAR";
    case OP_GET_LOCAL:
        return "GET_LOCAL";
    case OP_SET_LOCAL:
        return "SET_LOCAL";
    case OP_GET_ARRAY:
        return "GET_ARRAY";
    case OP_SET_ARRAY:
        return "SET_ARRAY";
    case OP_SET_ARRAY_BULK:
        return "SET_ARRAY_BULK";
    case OP_EXPORT:
        return "EXPORT";
    case OP_PUSH_SCOPE:
        return "PUSH_SCOPE";
    case OP_POP_SCOPE:
        return "POP_SCOPE";
    case OP_GET_SPECIAL:
        return "GET_SPECIAL";
    case OP_CONCAT:
        return "CONCAT";
    case OP_EXPAND_PARAM:
        return "EXPAND_PARAM";
    case OP_EXPAND_ARITH:
        return "EXPAND_ARITH";
    case OP_SPLIT_FIELDS:
        return "SPLIT_FIELDS";
    case OP_COLLECT_WORDS:
        return "COLLECT_WORDS";
    case OP_GLOB:
        return "GLOB";
    case OP_QUOTE_REMOVE:
        return "QUOTE_REMOVE";
    case OP_EXPAND_TILDE:
        return "EXPAND_TILDE";
    case OP_JMP:
        return "JMP";
    case OP_JMP_TRUE:
        return "JMP_TRUE";
    case OP_JMP_FALSE:
        return "JMP_FALSE";
    case OP_JMP_NONE:
        return "JMP_NONE";
    case OP_RET:
        return "RET";
    case OP_LOOP_ENTER:
        return "LOOP_ENTER";
    case OP_LOOP_EXIT:
        return "LOOP_EXIT";
    case OP_BREAK:
        return "BREAK";
    case OP_CONTINUE:
        return "CONTINUE";
    case OP_GET_NEXT_ITER:
        return "GET_NEXT_ITER";
    case OP_INIT_ITER:
        return "INIT_ITER";
    case OP_EXEC_SIMPLE:
        return "EXEC_SIMPLE";
    case OP_EXEC_BUILTIN:
        return "EXEC_BUILTIN";
    case OP_EXEC_FUNC:
        return "EXEC_FUNC";
    case OP_PIPELINE:
        return "PIPELINE";
    case OP_PIPELINE_CMD:
        return "PIPELINE_CMD";
    case OP_PIPELINE_END:
        return "PIPELINE_END";
    case OP_CMD_SUBST:
        return "CMD_SUBST";
    case OP_SUBSHELL:
        return "SUBSHELL";
    case OP_BACKGROUND:
        return "BACKGROUND";
    case OP_REDIR_SAVE:
        return "REDIR_SAVE";
    case OP_REDIR_RESTORE:
        return "REDIR_RESTORE";
    case OP_REDIR_OPEN:
        return "REDIR_OPEN";
    case OP_REDIR_DUP:
        return "REDIR_DUP";
    case OP_REDIR_CLOSE:
        return "REDIR_CLOSE";
    case OP_REDIR_HERE:
        return "REDIR_HERE";
    case OP_TEST_UNARY:
        return "TEST_UNARY";
    case OP_TEST_BINARY:
        return "TEST_BINARY";
    case OP_NEGATE_STATUS:
        return "NEGATE_STATUS";
    case OP_PATTERN_MATCH:
        return "PATTERN_MATCH";
    case OP_IMPORT:
        return "IMPORT";
    case OP_EXPAND_ARGS:
        return "EXPAND_ARGS";
    case OP_CAP_CHECK:
        return "CAP_CHECK";
    case OP_ERREXIT_PUSH:
        return "ERREXIT_PUSH";
    case OP_ERREXIT_POP:
        return "ERREXIT_POP";
    case OP_STATUS_ZERO:
        return "STATUS_ZERO";
    case OP_PUSH_STATUS:
        return "PUSH_STATUS";
    case OP_POP_STATUS:
        return "POP_STATUS";
    case OP_HALT:
        return "HALT";
    }
    return "UNKNOWN";
}

size_t disasm_instruction(const bytecode_image_t *img, size_t offset, FILE *out)
{
    if (offset >= img->code_size) {
        return offset;
    }

    uint8_t op = peek_u8(img, offset);
    fprintf(out, "%04zu  %-16s", offset, opcode_name(op));
    offset++;

    switch ((opcode_t)op) {
    case OP_PUSH_CONST: {
        uint16_t idx = peek_u16(img, offset);
        offset += 2;
        if (idx < img->const_count) {
            fprintf(out, "%u (\"%s\")", idx, img->const_pool[idx]);
        } else {
            fprintf(out, "%u (invalid)", idx);
        }
        break;
    }
    case OP_PUSH_INT:
        fprintf(out, "%d", peek_i32(img, offset));
        offset += 4;
        break;
    case OP_CONCAT:
    case OP_EXEC_BUILTIN:
    case OP_EXEC_FUNC:
    case OP_PIPELINE:
    case OP_IMPORT:
    case OP_INIT_ITER:
        fprintf(out, "%u", peek_u16(img, offset));
        offset += 2;
        break;
    case OP_GET_VAR:
    case OP_SET_VAR:
    case OP_GET_LOCAL:
    case OP_SET_LOCAL:
    case OP_GET_ARRAY:
    case OP_SET_ARRAY:
    case OP_EXPORT: {
        uint16_t idx = peek_u16(img, offset);
        offset += 2;
        if (idx < img->const_count) {
            fprintf(out, "%u (\"%s\")", idx, img->const_pool[idx]);
        } else {
            fprintf(out, "%u", idx);
        }
        break;
    }
    case OP_SET_ARRAY_BULK:
        fprintf(out, "%u, %u", peek_u16(img, offset), peek_u16(img, offset + 2));
        offset += 4;
        break;
    case OP_EXPAND_PARAM:
        fprintf(out, "%u, op=%u, flags=%u", peek_u16(img, offset), peek_u8(img, offset + 2),
                peek_u8(img, offset + 3));
        offset += 4;
        break;
    case OP_JMP:
    case OP_JMP_TRUE:
    case OP_JMP_FALSE:
    case OP_JMP_NONE: {
        int32_t off = peek_i32(img, offset);
        fprintf(out, "%d (-> %04zu)", off, (size_t)((int64_t)(offset + 4) + off));
        offset += 4;
        break;
    }
    case OP_PIPELINE_CMD:
    case OP_CMD_SUBST:
    case OP_SUBSHELL:
        fprintf(out, "%u", (uint32_t)peek_i32(img, offset));
        offset += 4;
        break;
    case OP_PUSH_SCOPE:
    case OP_GET_SPECIAL:
    case OP_EXEC_SIMPLE:
    case OP_BREAK:
    case OP_CONTINUE:
    case OP_PIPELINE_END:
    case OP_CAP_CHECK:
    case OP_REDIR_CLOSE:
    case OP_TEST_UNARY:
    case OP_TEST_BINARY:
        fprintf(out, "%u", peek_u8(img, offset));
        offset += 1;
        break;
    case OP_REDIR_OPEN:
    case OP_REDIR_DUP:
    case OP_REDIR_HERE:
        fprintf(out, "%u, %u", peek_u8(img, offset), peek_u8(img, offset + 1));
        offset += 2;
        break;
    default:
        /* No operands */
        break;
    }

    fprintf(out, "\n");
    return offset;
}

void disasm_image(const bytecode_image_t *img, FILE *out)
{
    uint16_t i;
    size_t offset;

    fprintf(out, "=== Constants ===\n");
    for (i = 0; i < img->const_count; i++) {
        fprintf(out, "  [%u] \"%s\"\n", i, img->const_pool[i]);
    }

    fprintf(out, "=== Bytecode (%zu bytes) ===\n", img->code_size);
    offset = 0;
    while (offset < img->code_size) {
        offset = disasm_instruction(img, offset, out);
    }
}
