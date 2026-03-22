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
 * Check if a wordunit is a single literal string (no expansions).
 */
static bool is_literal_word(word_part_t *w)
{
    return w != NULL && w->type == WP_LITERAL && w->next == NULL;
}

/*
 * Get the literal string value from a single-literal word.
 */
static const char *literal_value(word_part_t *w)
{
    return w->part.string;
}

/*
 * Compile a simple command with literal words only.
 */
static void compile_simple(compiler_t *cc, command_t *cmd)
{
    size_t i;
    plist_t *assigns = &cmd->u.simple.assigns;
    plist_t *words = &cmd->u.simple.words;

    /* Compile assignments */
    for (i = 0; i < assigns->length; i++) {
        word_part_t *w = plist_get(assigns, i);
        if (!is_literal_word(w)) {
            compiler_error(cc, cmd->lineno, "non-literal assignment not yet supported");
            continue;
        }
        const char *assign_str = literal_value(w);
        const char *eq = strchr(assign_str, '=');
        if (eq == NULL) {
            compiler_error(cc, cmd->lineno, "invalid assignment");
            continue;
        }

        /* Extract variable name */
        size_t name_len = (size_t)(eq - assign_str);
        char *name = xmalloc(name_len + 1);
        memcpy(name, assign_str, name_len);
        name[name_len] = '\0';

        /* Extract value */
        const char *val = eq + 1;

        uint16_t name_idx = image_add_const(cc->image, name);
        uint16_t val_idx = image_add_const(cc->image, val);
        bool local = is_local_var(cc, name);
        free(name);

        image_emit_u8(cc->image, OP_PUSH_CONST);
        image_emit_u16(cc->image, val_idx);
        image_emit_u8(cc->image, local ? OP_SET_LOCAL : OP_SET_VAR);
        image_emit_u16(cc->image, name_idx);
    }

    /* If no command words, we're done (assignment-only command) */
    if (words->length == 0) {
        return;
    }

    /* Check that all words are literals */
    for (i = 0; i < words->length; i++) {
        word_part_t *w = plist_get(words, i);
        if (!is_literal_word(w)) {
            compiler_error(cc, cmd->lineno, "non-literal command word not yet supported");
            return;
        }
    }

    /* Get the command name */
    word_part_t *cmd_word = plist_get(words, 0);
    const char *cmd_name = literal_value(cmd_word);

    /* Check if it's a builtin */
    int builtin_idx = builtin_lookup(cmd_name);

    /* Emit REDIR_SAVE */
    image_emit_u8(cc->image, OP_REDIR_SAVE);

    /* Push all words as constants */
    for (i = 0; i < words->length; i++) {
        word_part_t *w = plist_get(words, i);
        uint16_t idx = image_add_const(cc->image, literal_value(w));
        image_emit_u8(cc->image, OP_PUSH_CONST);
        image_emit_u16(cc->image, idx);
    }

    /* Push argc */
    image_emit_u8(cc->image, OP_PUSH_INT);
    image_emit_i32(cc->image, (int32_t)words->length);

    if (builtin_idx >= 0) {
        image_emit_u8(cc->image, OP_EXEC_BUILTIN);
        image_emit_u16(cc->image, (uint16_t)builtin_idx);
    } else {
        /* Unknown command -- emit EXEC_SIMPLE stub */
        image_emit_u8(cc->image, OP_EXEC_SIMPLE);
        image_emit_u8(cc->image, 0);
    }

    /* Emit REDIR_RESTORE */
    image_emit_u8(cc->image, OP_REDIR_RESTORE);
}

/*
 * Compile a command node.
 */
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

/*
 * Compile a pipeline (single command only for Phase 3c).
 */
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

/*
 * Compile an sh-list (single pipeline only for Phase 3c).
 */
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

/*
 * Compile a program (list of and-or commands).
 */
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
