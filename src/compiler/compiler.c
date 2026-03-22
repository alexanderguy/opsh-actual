#include "compiler/compiler.h"

#include "builtins/builtins.h"
#include "foundation/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void compiler_init(compiler_t *cc, const char *filename)
{
    memset(cc, 0, sizeof(*cc));
    cc->image = image_new();
    cc->error_count = 0;
    cc->filename = filename;
}

static bool is_local_var(compiler_t *cc, const char *name)
{
    return cc->locals_active && ht_get(&cc->locals, name) != NULL;
}

static void compiler_error(compiler_t *cc, unsigned int lineno, const char *msg)
{
    fprintf(stderr, "opsh: %s:%u: %s\n", cc->filename, lineno, msg);
    cc->error_count++;
}

/*
 * Compile a word unit chain into bytecode that leaves a single string
 * value on the stack. Handles WP_LITERAL, WP_PARAM, WP_ARITH, WP_CMDSUB.
 *
 * For multi-part words (e.g. "hello${var}world"), emits each part and
 * then a CONCAT to join them.
 */
static void compile_word(compiler_t *cc, word_part_t *w, unsigned int lineno)
{
    int part_count = 0;
    word_part_t *unit;

    for (unit = w; unit != NULL; unit = unit->next) {
        switch (unit->type) {
        case WP_LITERAL: {
            uint16_t idx = image_add_const(cc->image, unit->part.string);
            image_emit_u8(cc->image, OP_PUSH_CONST);
            image_emit_u16(cc->image, idx);
            part_count++;
            break;
        }

        case WP_PARAM: {
            param_exp_t *pe = unit->part.param;

            /* ${#var} -- string length */
            if (pe->flags & PE_STRLEN) {
                uint16_t name_idx = image_add_const(cc->image, pe->name);
                image_emit_u8(cc->image, OP_EXPAND_PARAM);
                image_emit_u16(cc->image, name_idx);
                image_emit_u8(cc->image, (uint8_t)PE_NONE);
                image_emit_u8(cc->image, (uint8_t)(pe->flags & 0xFF));
                part_count++;
                break;
            }

            /* Check for special parameters ($?, $$, etc.) */
            if (pe->type == PE_NONE && pe->name[0] != '\0' && pe->name[1] == '\0') {
                char ch = pe->name[0];
                special_var_t sp = (special_var_t)-1;
                switch (ch) {
                case '?':
                    sp = SPECIAL_QUESTION;
                    break;
                case '#':
                    sp = SPECIAL_HASH;
                    break;
                case '@':
                    sp = SPECIAL_AT;
                    break;
                case '*':
                    sp = SPECIAL_STAR;
                    break;
                case '$':
                    sp = SPECIAL_DOLLAR;
                    break;
                case '!':
                    sp = SPECIAL_BANG;
                    break;
                case '-':
                    sp = SPECIAL_DASH;
                    break;
                case '0':
                    sp = SPECIAL_ZERO;
                    break;
                default:
                    break;
                }
                if ((int)sp != -1) {
                    image_emit_u8(cc->image, OP_GET_SPECIAL);
                    image_emit_u8(cc->image, (uint8_t)sp);
                    part_count++;
                    break;
                }
            }

            /* Simple $var or ${var} */
            if (pe->type == PE_NONE) {
                uint16_t name_idx = image_add_const(cc->image, pe->name);
                image_emit_u8(cc->image,
                              is_local_var(cc, pe->name) ? OP_GET_LOCAL : OP_GET_VAR);
                image_emit_u16(cc->image, name_idx);
                part_count++;
                break;
            }

            /* Parameter expansion with operators */
            uint16_t name_idx = image_add_const(cc->image, pe->name);

            /* Push sub-words onto stack before EXPAND_PARAM */
            if (pe->type == PE_REPLACE) {
                /* Pattern first, then replacement */
                if (pe->pattern != NULL) {
                    compile_word(cc, pe->pattern, lineno);
                } else {
                    uint16_t empty = image_add_const(cc->image, "");
                    image_emit_u8(cc->image, OP_PUSH_CONST);
                    image_emit_u16(cc->image, empty);
                }
                if (pe->replacement != NULL) {
                    compile_word(cc, pe->replacement, lineno);
                } else {
                    uint16_t empty = image_add_const(cc->image, "");
                    image_emit_u8(cc->image, OP_PUSH_CONST);
                    image_emit_u16(cc->image, empty);
                }
            } else if (pe->type == PE_DEFAULT || pe->type == PE_ALTERNATE ||
                       pe->type == PE_ASSIGN || pe->type == PE_ERROR ||
                       pe->type == PE_TRIM) {
                /* One sub-word argument */
                if (pe->pattern != NULL) {
                    compile_word(cc, pe->pattern, lineno);
                } else {
                    uint16_t empty = image_add_const(cc->image, "");
                    image_emit_u8(cc->image, OP_PUSH_CONST);
                    image_emit_u16(cc->image, empty);
                }
            }

            image_emit_u8(cc->image, OP_EXPAND_PARAM);
            image_emit_u16(cc->image, name_idx);
            image_emit_u8(cc->image, (uint8_t)pe->type);
            image_emit_u8(cc->image, (uint8_t)(pe->flags & 0xFF));
            part_count++;
            break;
        }

        case WP_ARITH: {
            /* Compile the expression content, then EXPAND_ARITH */
            if (unit->part.arith != NULL) {
                compile_word(cc, unit->part.arith, lineno);
            } else {
                uint16_t zero = image_add_const(cc->image, "0");
                image_emit_u8(cc->image, OP_PUSH_CONST);
                image_emit_u16(cc->image, zero);
            }
            image_emit_u8(cc->image, OP_EXPAND_ARITH);
            part_count++;
            break;
        }

        case WP_CMDSUB:
            compiler_error(cc, lineno, "command substitution not yet supported by compiler");
            /* Push empty string as placeholder */
            {
                uint16_t empty = image_add_const(cc->image, "");
                image_emit_u8(cc->image, OP_PUSH_CONST);
                image_emit_u16(cc->image, empty);
            }
            part_count++;
            break;
        }
    }

    /* If multiple parts, concatenate */
    if (part_count > 1) {
        image_emit_u8(cc->image, OP_CONCAT);
        image_emit_u16(cc->image, (uint16_t)part_count);
    } else if (part_count == 0) {
        /* Empty word -- push empty string */
        uint16_t empty = image_add_const(cc->image, "");
        image_emit_u8(cc->image, OP_PUSH_CONST);
        image_emit_u16(cc->image, empty);
    }
}

/*
 * Check if a wordunit is a single literal string.
 */
static bool is_literal_word(word_part_t *w)
{
    return w != NULL && w->type == WP_LITERAL && w->next == NULL;
}

static const char *literal_value(word_part_t *w)
{
    return w->part.string;
}

/*
 * Compile a simple command.
 */
static void compile_simple(compiler_t *cc, command_t *cmd)
{
    size_t i;
    plist_t *assigns = &cmd->u.simple.assigns;
    plist_t *words = &cmd->u.simple.words;

    /* Compile assignments */
    for (i = 0; i < assigns->length; i++) {
        word_part_t *w = plist_get(assigns, i);

        /* Assignments are a single WP_LITERAL like "VAR=value" or contain
         * expansions in the value part. For now, split the first WP_LITERAL
         * on '=' to get the name, then compile the value portion. */
        if (w == NULL) {
            continue;
        }

        /* The assignment word is either a pure literal "NAME=value" or has
         * the name as a literal prefix before the '='. The parser guarantees
         * the raw token matched the assignment pattern. For the literal case,
         * split on '='. For words with expansions, the first WP_LITERAL
         * contains "NAME=" and subsequent units are the value. */
        if (w->type == WP_LITERAL) {
            const char *eq = strchr(w->part.string, '=');
            if (eq == NULL) {
                compiler_error(cc, cmd->lineno, "invalid assignment");
                continue;
            }

            size_t name_len = (size_t)(eq - w->part.string);
            char *name = xmalloc(name_len + 1);
            memcpy(name, w->part.string, name_len);
            name[name_len] = '\0';
            uint16_t name_idx = image_add_const(cc->image, name);
            free(name);

            /* If this is the only unit and there's a value after '=' */
            if (w->next == NULL) {
                const char *val = eq + 1;
                uint16_t val_idx = image_add_const(cc->image, val);
                image_emit_u8(cc->image, OP_PUSH_CONST);
                image_emit_u16(cc->image, val_idx);
            } else {
                /* The value part starts after '=' in this string plus
                 * the remaining word units. Build a temporary word chain. */
                const char *val_prefix = eq + 1;
                if (val_prefix[0] != '\0') {
                    uint16_t pfx = image_add_const(cc->image, val_prefix);
                    image_emit_u8(cc->image, OP_PUSH_CONST);
                    image_emit_u16(cc->image, pfx);
                    compile_word(cc, w->next, cmd->lineno);
                    image_emit_u8(cc->image, OP_CONCAT);
                    image_emit_u16(cc->image, 2);
                } else {
                    compile_word(cc, w->next, cmd->lineno);
                }
            }

            image_emit_u8(cc->image, is_local_var(cc, name) ? OP_SET_LOCAL : OP_SET_VAR);
            image_emit_u16(cc->image, name_idx);
        } else {
            compiler_error(cc, cmd->lineno, "assignment with non-string prefix");
        }
    }

    /* If no command words, done */
    if (words->length == 0) {
        return;
    }

    /* Determine if the command name is a literal (for builtin resolution) */
    word_part_t *cmd_word = plist_get(words, 0);
    int builtin_idx = -1;
    if (is_literal_word(cmd_word)) {
        builtin_idx = builtin_lookup(literal_value(cmd_word));
    }

    /* Emit REDIR_SAVE */
    image_emit_u8(cc->image, OP_REDIR_SAVE);

    /* Compile each word */
    for (i = 0; i < words->length; i++) {
        word_part_t *w = plist_get(words, i);
        compile_word(cc, w, cmd->lineno);
    }

    /* Push argc */
    image_emit_u8(cc->image, OP_PUSH_INT);
    image_emit_i32(cc->image, (int32_t)words->length);

    if (builtin_idx >= 0) {
        image_emit_u8(cc->image, OP_EXEC_BUILTIN);
        image_emit_u16(cc->image, (uint16_t)builtin_idx);
    } else {
        image_emit_u8(cc->image, OP_EXEC_SIMPLE);
        image_emit_u8(cc->image, 0);
    }

    /* Emit REDIR_RESTORE */
    image_emit_u8(cc->image, OP_REDIR_RESTORE);
}

static void compile_command(compiler_t *cc, command_t *cmd)
{
    if (cmd == NULL) {
        return;
    }

    switch (cmd->type) {
    case CT_SIMPLE:
        compile_simple(cc, cmd);
        break;
    default:
        compiler_error(cc, cmd->lineno, "compound commands not yet supported by compiler");
        break;
    }
}

static void compile_and_or(compiler_t *cc, and_or_t *pl)
{
    if (pl->commands == NULL) {
        return;
    }
    if (pl->commands->next != NULL) {
        compiler_error(cc, pl->commands->lineno,
                       "multi-command pipelines not yet supported by compiler");
        return;
    }
    compile_command(cc, pl->commands);
}

static void compile_sh_list(compiler_t *cc, sh_list_t *ao)
{
    if (ao->pipelines == NULL) {
        return;
    }
    if (ao->pipelines->next != NULL) {
        compiler_error(cc, ao->pipelines->commands->lineno,
                       "&& / || not yet supported by compiler");
        return;
    }
    compile_and_or(cc, ao->pipelines);
}

static void compile_program(compiler_t *cc, sh_list_t *program)
{
    sh_list_t *ao;
    for (ao = program; ao != NULL; ao = ao->next) {
        compile_sh_list(cc, ao);
    }
    image_emit_u8(cc->image, OP_HALT);
}

bytecode_image_t *compile(sh_list_t *program, const char *filename)
{
    compiler_t cc;
    bytecode_image_t *image;

    compiler_init(&cc, filename);
    compile_program(&cc, program);

    if (cc.error_count > 0) {
        image_free(cc.image);
        return NULL;
    }

    image = cc.image;
    return image;
}
