#include "compiler/compiler.h"

#include "builtins/builtins.h"
#include "foundation/util.h"
#include "parser/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void compiler_init(compiler_t *cc, const char *filename)
{
    memset(cc, 0, sizeof(*cc));
    cc->image = image_new();
    cc->filename = filename;
    ht_init(&cc->imported);

    /* Extract script directory for module resolution */
    {
        const char *last_slash = strrchr(filename, '/');
        if (last_slash != NULL) {
            size_t dir_len = (size_t)(last_slash - filename);
            char *dir = xmalloc(dir_len + 1);
            memcpy(dir, filename, dir_len);
            dir[dir_len] = '\0';
            cc->script_dir = dir;
        } else {
            cc->script_dir = xmalloc(2);
            strcpy((char *)cc->script_dir, ".");
        }
    }
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

/* Forward declarations */
static void compile_word(compiler_t *cc, word_part_t *w, unsigned int lineno);
static void compile_command(compiler_t *cc, command_t *cmd);
static void compile_program(compiler_t *cc, sh_list_t *program);

/*
 * Emit a jump instruction with a placeholder offset.
 * Returns the offset of the i32 operand for later backpatching.
 */
static size_t emit_jump(compiler_t *cc, opcode_t op)
{
    size_t patch_pos;
    image_emit_u8(cc->image, (uint8_t)op);
    patch_pos = cc->image->code_size;
    image_emit_i32(cc->image, 0);
    return patch_pos;
}

/*
 * Backpatch a jump instruction to target the current code position.
 */
static void patch_jump(compiler_t *cc, size_t patch_pos)
{
    size_t target = cc->image->code_size;
    int32_t offset = (int32_t)((int64_t)target - (int64_t)(patch_pos + 4));
    cc->image->code[patch_pos] = (uint8_t)(offset & 0xFF);
    cc->image->code[patch_pos + 1] = (uint8_t)((offset >> 8) & 0xFF);
    cc->image->code[patch_pos + 2] = (uint8_t)((offset >> 16) & 0xFF);
    cc->image->code[patch_pos + 3] = (uint8_t)((offset >> 24) & 0xFF);
}

/*
 * Patch a jump to target a specific bytecode offset.
 */
static void patch_jump_to(compiler_t *cc, size_t patch_pos, size_t target)
{
    int32_t offset = (int32_t)((int64_t)target - (int64_t)(patch_pos + 4));
    cc->image->code[patch_pos] = (uint8_t)(offset & 0xFF);
    cc->image->code[patch_pos + 1] = (uint8_t)((offset >> 8) & 0xFF);
    cc->image->code[patch_pos + 2] = (uint8_t)((offset >> 16) & 0xFF);
    cc->image->code[patch_pos + 3] = (uint8_t)((offset >> 24) & 0xFF);
}

/* Loop stack management */
static void push_loop(compiler_t *cc, size_t continue_target)
{
    if (cc->loop_depth >= MAX_LOOP_DEPTH) {
        compiler_error(cc, 0, "loop nesting too deep");
        return;
    }
    loop_info_t *li = &cc->loop_stack[cc->loop_depth++];
    li->continue_target = continue_target;
    plist_init(&li->break_patches);
}

static void pop_loop(compiler_t *cc)
{
    if (cc->loop_depth <= 0) {
        return;
    }
    cc->loop_depth--;
    loop_info_t *li = &cc->loop_stack[cc->loop_depth];

    /* Backpatch all break jumps to current position */
    size_t i;
    for (i = 0; i < li->break_patches.length; i++) {
        size_t *patch_pos = plist_get(&li->break_patches, i);
        patch_jump(cc, *patch_pos);
        free(patch_pos);
    }
    plist_destroy(&li->break_patches);
}

/*
 * Compile a word unit chain into bytecode that leaves a single string
 * value on the stack.
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

            if (pe->flags & PE_STRLEN) {
                uint16_t name_idx = image_add_const(cc->image, pe->name);
                image_emit_u8(cc->image, OP_EXPAND_PARAM);
                image_emit_u16(cc->image, name_idx);
                image_emit_u8(cc->image, (uint8_t)PE_NONE);
                image_emit_u8(cc->image, (uint8_t)(pe->flags & 0xFF));
                part_count++;
                break;
            }

            /* Special parameters */
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
                image_emit_u8(cc->image, is_local_var(cc, pe->name) ? OP_GET_LOCAL : OP_GET_VAR);
                image_emit_u16(cc->image, name_idx);
                part_count++;
                break;
            }

            /* Parameter expansion with operators */
            uint16_t name_idx = image_add_const(cc->image, pe->name);

            if (pe->type == PE_REPLACE) {
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
                       pe->type == PE_ASSIGN || pe->type == PE_ERROR || pe->type == PE_TRIM) {
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

        case WP_CMDSUB: {
            /* Compile the command substitution body as a sub-segment.
             * Layout: JMP over body, body..., HALT, then CMD_SUBST offset */
            char *cmdsub_source = NULL;
            if (unit->part.cmdsub.is_preparsed) {
                compiler_error(cc, lineno, "preparsed command substitution not yet supported");
                uint16_t empty = image_add_const(cc->image, "");
                image_emit_u8(cc->image, OP_PUSH_CONST);
                image_emit_u16(cc->image, empty);
                part_count++;
                break;
            }
            cmdsub_source = unit->part.cmdsub.u.unparsed;

            /* Parse the command substitution content */
            parser_t sub_parser;
            parser_init(&sub_parser, cmdsub_source, cc->filename);
            sh_list_t *sub_ast = parser_parse(&sub_parser);
            if (parser_error_count(&sub_parser) > 0) {
                compiler_error(cc, lineno, "error in command substitution");
                sh_list_free(sub_ast);
                parser_destroy(&sub_parser);
                uint16_t empty = image_add_const(cc->image, "");
                image_emit_u8(cc->image, OP_PUSH_CONST);
                image_emit_u16(cc->image, empty);
                part_count++;
                break;
            }

            /* JMP over the sub-segment */
            size_t skip_jump = emit_jump(cc, OP_JMP);

            /* Record the sub-segment start offset */
            size_t sub_offset = cc->image->code_size;

            /* Compile the sub-program */
            compile_program(cc, sub_ast);
            image_emit_u8(cc->image, OP_HALT);

            sh_list_free(sub_ast);
            parser_destroy(&sub_parser);

            /* Patch the skip jump */
            patch_jump(cc, skip_jump);

            /* Emit CMD_SUBST with the sub-segment offset */
            image_emit_u8(cc->image, OP_CMD_SUBST);
            {
                uint32_t uoff = (uint32_t)sub_offset;
                image_emit_u8(cc->image, (uint8_t)(uoff & 0xFF));
                image_emit_u8(cc->image, (uint8_t)((uoff >> 8) & 0xFF));
                image_emit_u8(cc->image, (uint8_t)((uoff >> 16) & 0xFF));
                image_emit_u8(cc->image, (uint8_t)((uoff >> 24) & 0xFF));
            }
            part_count++;
            break;
        }
        }
    }

    if (part_count > 1) {
        image_emit_u8(cc->image, OP_CONCAT);
        image_emit_u16(cc->image, (uint16_t)part_count);
    } else if (part_count == 0) {
        uint16_t empty = image_add_const(cc->image, "");
        image_emit_u8(cc->image, OP_PUSH_CONST);
        image_emit_u16(cc->image, empty);
    }
}

/*
 * Check if a word needs field splitting at runtime.
 * A word needs splitting if it contains unquoted parameter expansions
 * or command substitutions.
 */
static bool word_needs_split(word_part_t *w)
{
    word_part_t *unit;
    for (unit = w; unit != NULL; unit = unit->next) {
        if ((unit->type == WP_PARAM || unit->type == WP_CMDSUB) && !unit->quoted) {
            return true;
        }
    }
    return false;
}

/*
 * Check if a literal word contains glob characters (* ? [).
 */
static bool word_has_glob(word_part_t *w)
{
    word_part_t *unit;
    for (unit = w; unit != NULL; unit = unit->next) {
        if (unit->type == WP_LITERAL && !unit->quoted) {
            const char *p = unit->part.string;
            while (*p) {
                if (*p == '*' || *p == '?' || *p == '[') {
                    return true;
                }
                p++;
            }
        }
    }
    return false;
}

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
/*
 * Resolve a module name to a file path.
 * Tries: script_dir/lib/name.opsh, then $OPSH_LOADPATH entries.
 * Returns a malloc'd string or NULL.
 */
static char *resolve_module(compiler_t *cc, const char *name)
{
    char path[4096];
    struct stat st;

    /* Try script_dir/lib/name.opsh */
    snprintf(path, sizeof(path), "%s/lib/%s.opsh", cc->script_dir, name);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        char *result = xmalloc(strlen(path) + 1);
        strcpy(result, path);
        return result;
    }

    /* Try $OPSH_LOADPATH */
    const char *loadpath = getenv("OPSH_LOADPATH");
    if (loadpath != NULL) {
        const char *p = loadpath;
        while (*p) {
            const char *sep = strchr(p, ':');
            size_t dir_len = sep ? (size_t)(sep - p) : strlen(p);
            if (dir_len > 0) {
                snprintf(path, sizeof(path), "%.*s/%s.opsh", (int)dir_len, p, name);
                if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                    char *result = xmalloc(strlen(path) + 1);
                    strcpy(result, path);
                    return result;
                }
            }
            if (sep) {
                p = sep + 1;
            } else {
                break;
            }
        }
    }

    return NULL;
}

/*
 * Read a file into a malloc'd string. Returns NULL on failure.
 */
static char *read_file_contents(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = xmalloc((size_t)len + 1);
    if (len > 0) {
        size_t nread = fread(buf, 1, (size_t)len, f);
        buf[nread] = '\0';
    } else {
        buf[0] = '\0';
    }
    fclose(f);
    return buf;
}

/*
 * Compile a module import. Resolves the module file, parses and compiles
 * it into the same bytecode image, and registers its functions.
 */
static void compile_import(compiler_t *cc, const char *module_name, unsigned int lineno);

static void compile_simple(compiler_t *cc, command_t *cmd)
{
    size_t i;
    plist_t *assigns = &cmd->u.simple.assigns;
    plist_t *words = &cmd->u.simple.words;

    /* Intercept lib::import as a compile-time directive */
    if (words->length >= 2) {
        word_part_t *cmd_word = plist_get(words, 0);
        if (is_literal_word(cmd_word) && strcmp(literal_value(cmd_word), "lib::import") == 0) {
            word_part_t *mod_word = plist_get(words, 1);
            if (is_literal_word(mod_word)) {
                compile_import(cc, literal_value(mod_word), cmd->lineno);
            } else {
                compiler_error(cc, cmd->lineno, "lib::import requires a literal module name");
            }
            return;
        }
    }

    /* Compile assignments */
    for (i = 0; i < assigns->length; i++) {
        word_part_t *w = plist_get(assigns, i);
        if (w == NULL) {
            continue;
        }
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

            if (w->next == NULL) {
                const char *val = eq + 1;
                uint16_t val_idx = image_add_const(cc->image, val);
                image_emit_u8(cc->image, OP_PUSH_CONST);
                image_emit_u16(cc->image, val_idx);
            } else {
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

            image_emit_u8(cc->image,
                          is_local_var(cc, cc->image->const_pool[name_idx]) ? OP_SET_LOCAL
                                                                            : OP_SET_VAR);
            image_emit_u16(cc->image, name_idx);
        } else {
            compiler_error(cc, cmd->lineno, "assignment with non-string prefix");
        }
    }

    if (words->length == 0) {
        return;
    }

    word_part_t *cmd_word = plist_get(words, 0);
    int builtin_idx = -1;
    if (is_literal_word(cmd_word)) {
        builtin_idx = builtin_lookup(literal_value(cmd_word));
    }

    image_emit_u8(cc->image, OP_REDIR_SAVE);

    /* Emit redirections */
    {
        io_redir_t *rd;
        for (rd = cmd->redirs; rd != NULL; rd = rd->next) {
            switch (rd->type) {
            case REDIR_IN:
            case REDIR_OUT:
            case REDIR_APPEND:
            case REDIR_CLOBBER:
            case REDIR_RDWR:
                /* Push filename onto stack, then REDIR_OPEN */
                compile_word(cc, rd->target, cmd->lineno);
                image_emit_u8(cc->image, OP_REDIR_OPEN);
                image_emit_u8(cc->image, (uint8_t)rd->fd);
                image_emit_u8(cc->image, (uint8_t)rd->type);
                break;
            case REDIR_DUPIN:
            case REDIR_DUPOUT:
                /* Push target FD string, then REDIR_DUP */
                compile_word(cc, rd->target, cmd->lineno);
                image_emit_u8(cc->image, OP_REDIR_DUP);
                image_emit_u8(cc->image, (uint8_t)rd->fd);
                image_emit_u8(cc->image, 0); /* direction flag */
                break;
            case REDIR_CLOSE:
                image_emit_u8(cc->image, OP_REDIR_CLOSE);
                image_emit_u8(cc->image, (uint8_t)rd->fd);
                break;
            case REDIR_HEREDOC:
            case REDIR_HEREDOC_STRIP:
            case REDIR_HERESTR:
                /* Push here-doc content, then REDIR_HERE */
                if (rd->target != NULL) {
                    compile_word(cc, rd->target, cmd->lineno);
                } else {
                    uint16_t empty = image_add_const(cc->image, "");
                    image_emit_u8(cc->image, OP_PUSH_CONST);
                    image_emit_u16(cc->image, empty);
                }
                image_emit_u8(cc->image, OP_REDIR_HERE);
                image_emit_u8(cc->image, (uint8_t)rd->fd);
                image_emit_u8(cc->image, rd->heredoc_expand ? 1 : 0);
                break;
            }
        }
    }

    /* Compile each word as a (values... count) group.
     * Unquoted words with expansions get SPLIT_FIELDS.
     * Unquoted literal words with glob chars get GLOB.
     * Everything else gets count=1. */
    for (i = 0; i < words->length; i++) {
        word_part_t *w = plist_get(words, i);
        compile_word(cc, w, cmd->lineno);

        if (word_needs_split(w)) {
            image_emit_u8(cc->image, OP_SPLIT_FIELDS);
        } else if (word_has_glob(w)) {
            image_emit_u8(cc->image, OP_GLOB);
        } else {
            image_emit_u8(cc->image, OP_PUSH_INT);
            image_emit_i32(cc->image, 1);
        }
    }

    /* Collect all word groups into flat argc/argv */
    image_emit_u8(cc->image, OP_PUSH_INT);
    image_emit_i32(cc->image, (int32_t)words->length);
    image_emit_u8(cc->image, OP_COLLECT_WORDS);

    if (builtin_idx >= 0) {
        image_emit_u8(cc->image, OP_EXEC_BUILTIN);
        image_emit_u16(cc->image, (uint16_t)builtin_idx);
    } else if (is_literal_word(cmd_word)) {
        /* Check if it's a user-defined function */
        const char *name = literal_value(cmd_word);
        int fi;
        bool found = false;
        for (fi = 0; fi < cc->func_count; fi++) {
            if (strcmp(cc->func_table[fi].name, name) == 0) {
                image_emit_u8(cc->image, OP_EXEC_FUNC);
                image_emit_u16(cc->image, (uint16_t)fi);
                found = true;
                break;
            }
        }
        if (!found) {
            image_emit_u8(cc->image, OP_EXEC_SIMPLE);
            image_emit_u8(cc->image, 0);
        }
    } else {
        image_emit_u8(cc->image, OP_EXEC_SIMPLE);
        image_emit_u8(cc->image, 0);
    }

    image_emit_u8(cc->image, OP_REDIR_RESTORE);
}

/*
 * Compile an if/elif/else command.
 */
static void compile_if(compiler_t *cc, command_t *cmd)
{
    if_clause_t *ic;
    plist_t end_patches;
    size_t i;
    plist_init(&end_patches);

    for (ic = cmd->u.if_clause.clauses; ic != NULL; ic = ic->next) {
        if (ic->condition != NULL) {
            /* Compile condition */
            compile_program(cc, ic->condition);
            /* Jump past body if condition fails */
            size_t skip_body = emit_jump(cc, OP_JMP_FALSE);
            /* Compile body */
            compile_program(cc, ic->body);
            /* Jump to end of if chain */
            if (ic->next != NULL) {
                size_t *end_patch = xmalloc(sizeof(size_t));
                *end_patch = emit_jump(cc, OP_JMP);
                plist_add(&end_patches, end_patch);
            }
            /* Patch the skip-body jump */
            patch_jump(cc, skip_body);
        } else {
            /* else clause (no condition) */
            compile_program(cc, ic->body);
        }
    }

    /* Patch all end-of-chain jumps */
    for (i = 0; i < end_patches.length; i++) {
        size_t *patch_pos = plist_get(&end_patches, i);
        patch_jump(cc, *patch_pos);
        free(patch_pos);
    }
    plist_destroy(&end_patches);
}

/*
 * Compile a for loop.
 */
static void compile_for(compiler_t *cc, command_t *cmd)
{
    size_t i;
    char *varname = cmd->u.for_clause.varname;
    plist_t *words = &cmd->u.for_clause.wordlist;
    uint16_t var_idx = image_add_const(cc->image, varname);

    /* Push all word values with splitting/globbing as groups */
    for (i = 0; i < words->length; i++) {
        word_part_t *w = plist_get(words, i);
        compile_word(cc, w, cmd->lineno);

        if (word_needs_split(w)) {
            image_emit_u8(cc->image, OP_SPLIT_FIELDS);
        } else if (word_has_glob(w)) {
            image_emit_u8(cc->image, OP_GLOB);
        } else {
            image_emit_u8(cc->image, OP_PUSH_INT);
            image_emit_i32(cc->image, 1);
        }
    }

    /* Collect all word groups and create iterator */
    image_emit_u8(cc->image, OP_PUSH_INT);
    image_emit_i32(cc->image, (int32_t)words->length);
    image_emit_u8(cc->image, OP_COLLECT_WORDS);
    /* Stack now has: values... total_count. Use as INIT_ITER group. */
    image_emit_u8(cc->image, OP_INIT_ITER);
    image_emit_u16(cc->image, 1); /* 1 group */

    /* Loop start */
    size_t loop_top = cc->image->code_size;

    image_emit_u8(cc->image, OP_GET_NEXT_ITER);
    /* DUP for the JMP_NONE check */
    image_emit_u8(cc->image, OP_DUP);
    size_t exit_jump = emit_jump(cc, OP_JMP_NONE);

    /* Set the loop variable */
    image_emit_u8(cc->image, OP_SET_VAR);
    image_emit_u16(cc->image, var_idx);

    /* Register loop for break/continue */
    push_loop(cc, loop_top);

    /* Compile body */
    compile_program(cc, cmd->u.for_clause.body);

    /* Jump back to loop top */
    size_t back_jump = emit_jump(cc, OP_JMP);
    patch_jump_to(cc, back_jump, loop_top);

    /* Backpatch breaks to the cleanup code below */
    pop_loop(cc);

    /* Patch exit jump to cleanup */
    patch_jump(cc, exit_jump);

    /* Pop the VT_NONE that DUP left */
    image_emit_u8(cc->image, OP_POP);
    /* Pop the iterator */
    image_emit_u8(cc->image, OP_POP);
}

/*
 * Compile a while or until loop.
 */
static void compile_while_until(compiler_t *cc, command_t *cmd, bool is_until)
{
    /* Loop top: evaluate condition */
    size_t loop_top = cc->image->code_size;

    compile_program(cc, cmd->u.while_clause.condition);

    /* Jump past body if condition fails (while) or succeeds (until) */
    size_t exit_jump;
    if (is_until) {
        exit_jump = emit_jump(cc, OP_JMP_TRUE);
    } else {
        exit_jump = emit_jump(cc, OP_JMP_FALSE);
    }

    /* Register loop for break/continue */
    push_loop(cc, loop_top);

    /* Compile body */
    compile_program(cc, cmd->u.while_clause.body);

    /* Jump back to condition */
    size_t back_jump = emit_jump(cc, OP_JMP);
    patch_jump_to(cc, back_jump, loop_top);

    /* Backpatch breaks to after the loop */
    pop_loop(cc);

    /* Patch exit jump */
    patch_jump(cc, exit_jump);
}

/*
 * Compile a case command.
 * For each case item, compile the case word, compare with each pattern,
 * and conditionally execute the body.
 */
static void compile_case(compiler_t *cc, command_t *cmd)
{
    case_item_t *ci;
    plist_t end_patches;
    size_t i;
    plist_init(&end_patches);

    for (ci = cmd->u.case_clause.items; ci != NULL; ci = ci->next) {
        plist_t pattern_jumps;
        plist_init(&pattern_jumps);
        size_t skip_body = 0;
        bool has_skip = false;

        /* For each pattern, compile: push case word, push pattern, PATTERN_MATCH */
        for (i = 0; i < ci->patterns.length; i++) {
            word_part_t *pat = plist_get(&ci->patterns, i);
            /* Push the case word */
            compile_word(cc, cmd->u.case_clause.subject, cmd->lineno);
            /* Push the pattern */
            compile_word(cc, pat, cmd->lineno);
            /* PATTERN_MATCH sets laststatus */
            image_emit_u8(cc->image, OP_PATTERN_MATCH);
            /* If match, jump to the body */
            size_t *match_jump = xmalloc(sizeof(size_t));
            *match_jump = emit_jump(cc, OP_JMP_TRUE);
            plist_add(&pattern_jumps, match_jump);
        }

        /* None matched -- skip body */
        if (ci->patterns.length > 0) {
            skip_body = emit_jump(cc, OP_JMP);
            has_skip = true;
        }

        /* Patch pattern match jumps to body start */
        for (i = 0; i < pattern_jumps.length; i++) {
            size_t *pj = plist_get(&pattern_jumps, i);
            patch_jump(cc, *pj);
            free(pj);
        }
        plist_destroy(&pattern_jumps);

        /* Compile body */
        compile_program(cc, ci->body);

        /* ;; -> jump to end of case */
        if (ci->terminator == CASE_BREAK) {
            size_t *ep = xmalloc(sizeof(size_t));
            *ep = emit_jump(cc, OP_JMP);
            plist_add(&end_patches, ep);
        }
        /* ;& (fall-through) and ;| (continue matching) are handled by
         * not emitting a jump -- execution falls through to next item */

        /* Patch skip-body jump */
        if (has_skip) {
            patch_jump(cc, skip_body);
        }
    }

    /* Patch all end jumps */
    for (i = 0; i < end_patches.length; i++) {
        size_t *ep = plist_get(&end_patches, i);
        patch_jump(cc, *ep);
        free(ep);
    }
    plist_destroy(&end_patches);
}

/*
 * Compile a brace group { list; }
 */
static void compile_group(compiler_t *cc, command_t *cmd)
{
    compile_program(cc, cmd->u.group.body);
}

/*
 * Pre-scan a command tree for `local` declarations and record variable names.
 */
static void collect_locals_cmd(command_t *cmd, hashtable_t *locals);

static void collect_locals_program(sh_list_t *prog, hashtable_t *locals)
{
    sh_list_t *sl;
    for (sl = prog; sl != NULL; sl = sl->next) {
        and_or_t *ao;
        for (ao = sl->pipelines; ao != NULL; ao = ao->next) {
            command_t *c;
            for (c = ao->commands; c != NULL; c = c->next) {
                collect_locals_cmd(c, locals);
            }
        }
    }
}

static void collect_locals_cmd(command_t *cmd, hashtable_t *locals)
{
    if (cmd == NULL) {
        return;
    }
    switch (cmd->type) {
    case CT_SIMPLE: {
        plist_t *words = &cmd->u.simple.words;
        if (words->length == 0) {
            break;
        }
        word_part_t *first = plist_get(words, 0);
        if (first == NULL || first->type != WP_LITERAL || first->next != NULL) {
            break;
        }
        if (strcmp(first->part.string, "local") != 0) {
            break;
        }
        size_t wi;
        for (wi = 1; wi < words->length; wi++) {
            word_part_t *w = plist_get(words, wi);
            if (w == NULL || w->type != WP_LITERAL) {
                continue;
            }
            const char *s = w->part.string;
            const char *eq = strchr(s, '=');
            if (eq != NULL) {
                size_t nlen = (size_t)(eq - s);
                char *n = xmalloc(nlen + 1);
                memcpy(n, s, nlen);
                n[nlen] = '\0';
                if (ht_get(locals, n) == NULL) {
                    ht_set(locals, n, (void *)1);
                } else {
                    free(n);
                }
            } else {
                if (ht_get(locals, s) == NULL) {
                    size_t slen = strlen(s);
                    char *n = xmalloc(slen + 1);
                    memcpy(n, s, slen + 1);
                    ht_set(locals, n, (void *)1);
                }
            }
        }
        break;
    }
    case CT_IF: {
        if_clause_t *ic;
        for (ic = cmd->u.if_clause.clauses; ic != NULL; ic = ic->next) {
            if (ic->condition) {
                collect_locals_program(ic->condition, locals);
            }
            if (ic->body) {
                collect_locals_program(ic->body, locals);
            }
        }
        break;
    }
    case CT_FOR:
        if (cmd->u.for_clause.body) {
            collect_locals_program(cmd->u.for_clause.body, locals);
        }
        break;
    case CT_WHILE:
    case CT_UNTIL:
        if (cmd->u.while_clause.condition) {
            collect_locals_program(cmd->u.while_clause.condition, locals);
        }
        if (cmd->u.while_clause.body) {
            collect_locals_program(cmd->u.while_clause.body, locals);
        }
        break;
    case CT_CASE: {
        case_item_t *ci;
        for (ci = cmd->u.case_clause.items; ci != NULL; ci = ci->next) {
            if (ci->body) {
                collect_locals_program(ci->body, locals);
            }
        }
        break;
    }
    case CT_GROUP:
        if (cmd->u.group.body) {
            collect_locals_program(cmd->u.group.body, locals);
        }
        break;
    case CT_SUBSHELL:
        break;
    case CT_FUNCDEF:
        /* Don't recurse into nested functions */
        break;
    case CT_BRACKET:
        break;
    }
}

static int free_local_name(const char *key, void *value, void *ctx)
{
    (void)value;
    (void)ctx;
    free((void *)key);
    return 0;
}

/*
 * Compile a function definition.
 * The function body is compiled inline with a JMP over it.
 * The function's bytecode offset is recorded in the func table.
 */
static void compile_funcdef(compiler_t *cc, command_t *cmd)
{
    char *name = cmd->u.func_def.name;

    /* Jump over the function body */
    size_t skip_jump = emit_jump(cc, OP_JMP);

    /* Record function entry point */
    size_t func_offset = cc->image->code_size;

    if (cc->func_count >= MAX_FUNC_COUNT) {
        compiler_error(cc, cmd->lineno, "too many function definitions");
        patch_jump(cc, skip_jump);
        return;
    }

    cc->func_table[cc->func_count].name = xmalloc(strlen(name) + 1);
    strcpy(cc->func_table[cc->func_count].name, name);
    cc->func_table[cc->func_count].bytecode_offset = func_offset;
    cc->func_count++;

    /* Save and set up local variable tracking */
    hashtable_t saved_locals = cc->locals;
    bool saved_active = cc->locals_active;
    ht_init(&cc->locals);
    cc->locals_active = true;
    cc->func_depth++;
    collect_locals_cmd(cmd->u.func_def.body, &cc->locals);

    /* Compile function body */
    compile_command(cc, cmd->u.func_def.body);

    /* Return from function */
    image_emit_u8(cc->image, OP_RET);

    /* Restore local variable tracking */
    cc->func_depth--;
    ht_foreach(&cc->locals, free_local_name, NULL);
    ht_destroy(&cc->locals);
    cc->locals = saved_locals;
    cc->locals_active = saved_active;

    /* Patch the skip jump */
    patch_jump(cc, skip_jump);
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
    case CT_IF:
        compile_if(cc, cmd);
        break;
    case CT_FOR:
        compile_for(cc, cmd);
        break;
    case CT_WHILE:
        compile_while_until(cc, cmd, false);
        break;
    case CT_UNTIL:
        compile_while_until(cc, cmd, true);
        break;
    case CT_CASE:
        compile_case(cc, cmd);
        break;
    case CT_GROUP:
        compile_group(cc, cmd);
        break;
    case CT_FUNCDEF:
        compile_funcdef(cc, cmd);
        break;
    case CT_SUBSHELL:
        compiler_error(cc, cmd->lineno, "subshell not yet supported by compiler");
        break;
    case CT_BRACKET:
        compiler_error(cc, cmd->lineno, "[[ ]] not yet supported by compiler");
        break;
    }
}

static void compile_and_or(compiler_t *cc, and_or_t *pl)
{
    command_t *cmd;
    int cmd_count;

    if (pl->commands == NULL) {
        return;
    }

    /* Single-command pipeline: no fork needed */
    if (pl->commands->next == NULL) {
        compile_command(cc, pl->commands);
        if (pl->negated) {
            image_emit_u8(cc->image, OP_NEGATE_STATUS);
        }
        return;
    }

    /* Multi-command pipeline: compile each member as a sub-segment */
    cmd_count = 0;
    for (cmd = pl->commands; cmd != NULL; cmd = cmd->next) {
        cmd_count++;
    }

    /* First pass: compile all sub-segments, collecting their offsets.
     * This ensures the PIPELINE/PIPELINE_CMD/PIPELINE_END sequence
     * is contiguous in the bytecode stream. */
    {
        size_t *offsets = xmalloc((size_t)cmd_count * sizeof(size_t));
        int ci = 0;

        for (cmd = pl->commands; cmd != NULL; cmd = cmd->next) {
            size_t skip_jump = emit_jump(cc, OP_JMP);
            offsets[ci] = cc->image->code_size;
            compile_command(cc, cmd);
            image_emit_u8(cc->image, OP_HALT);
            patch_jump(cc, skip_jump);
            ci++;
        }

        /* Second pass: emit the contiguous PIPELINE instruction block */
        image_emit_u8(cc->image, OP_PIPELINE);
        image_emit_u16(cc->image, (uint16_t)cmd_count);

        for (ci = 0; ci < cmd_count; ci++) {
            uint32_t uoff = (uint32_t)offsets[ci];
            image_emit_u8(cc->image, OP_PIPELINE_CMD);
            image_emit_u8(cc->image, (uint8_t)(uoff & 0xFF));
            image_emit_u8(cc->image, (uint8_t)((uoff >> 8) & 0xFF));
            image_emit_u8(cc->image, (uint8_t)((uoff >> 16) & 0xFF));
            image_emit_u8(cc->image, (uint8_t)((uoff >> 24) & 0xFF));
        }

        image_emit_u8(cc->image, OP_PIPELINE_END);
        image_emit_u8(cc->image, pl->negated ? 1 : 0);

        free(offsets);
    }
}

static void compile_sh_list(compiler_t *cc, sh_list_t *ao)
{
    and_or_t *pl;

    if (ao->pipelines == NULL) {
        return;
    }

    compile_and_or(cc, ao->pipelines);

    for (pl = ao->pipelines; pl->next != NULL; pl = pl->next) {
        size_t patch;
        if (pl->connector) {
            patch = emit_jump(cc, OP_JMP_FALSE);
        } else {
            patch = emit_jump(cc, OP_JMP_TRUE);
        }
        compile_and_or(cc, pl->next);
        patch_jump(cc, patch);
    }
}

static void compile_program(compiler_t *cc, sh_list_t *program)
{
    sh_list_t *ao;
    for (ao = program; ao != NULL; ao = ao->next) {
        compile_sh_list(cc, ao);
    }
}

/*
 * Compile a module import.
 */
static void compile_import(compiler_t *cc, const char *module_name, unsigned int lineno)
{
    /* Check if already imported */
    if (ht_get(&cc->imported, module_name) != NULL) {
        return; /* already compiled */
    }

    /* Circular dependency check */
    {
        int di;
        for (di = 0; di < cc->import_depth; di++) {
            if (strcmp(cc->import_stack[di], module_name) == 0) {
                char buf[256];
                snprintf(buf, sizeof(buf), "circular dependency: module '%s'", module_name);
                compiler_error(cc, lineno, buf);
                return;
            }
        }
    }

    if (cc->import_depth >= MAX_IMPORT_DEPTH) {
        compiler_error(cc, lineno, "import nesting too deep");
        return;
    }

    /* Resolve module file */
    char *path = resolve_module(cc, module_name);
    if (path == NULL) {
        char buf[256];
        snprintf(buf, sizeof(buf), "module '%s' not found", module_name);
        compiler_error(cc, lineno, buf);
        return;
    }

    /* Read module source */
    char *source = read_file_contents(path);
    if (source == NULL) {
        char buf[256];
        snprintf(buf, sizeof(buf), "cannot read module '%s'", module_name);
        compiler_error(cc, lineno, buf);
        free(path);
        return;
    }

    /* Push onto import stack (for circular dep detection during parse) */
    cc->import_stack[cc->import_depth] = xmalloc(strlen(module_name) + 1);
    strcpy(cc->import_stack[cc->import_depth], module_name);
    cc->import_depth++;

    /* Parse the module */
    parser_t p;
    parser_init(&p, source, path);
    sh_list_t *ast = parser_parse(&p);

    if (parser_error_count(&p) > 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "errors in module '%s'", module_name);
        compiler_error(cc, lineno, buf);
        sh_list_free(ast);
        parser_destroy(&p);
        free(source);
        free(path);
        cc->import_depth--;
        free(cc->import_stack[cc->import_depth]);
        return;
    }

    /* Mark as imported now (after successful parse) */
    {
        char *key = xmalloc(strlen(module_name) + 1);
        strcpy(key, module_name);
        ht_set(&cc->imported, key, (void *)1);
    }

    /* Compile the module's code as a sub-segment.
     * JMP over the init code, compile the module body, HALT.
     * The init code runs once when OP_IMPORT executes. */
    size_t skip_jump = emit_jump(cc, OP_JMP);
    size_t init_offset = cc->image->code_size;

    /* Save/restore the filename for error reporting */
    const char *saved_filename = cc->filename;
    cc->filename = path;

    compile_program(cc, ast);
    image_emit_u8(cc->image, OP_HALT);

    cc->filename = saved_filename;

    patch_jump(cc, skip_jump);

    /* Record the module */
    if (cc->module_count < MAX_IMPORT_DEPTH * 4) {
        cc->modules[cc->module_count].name = xmalloc(strlen(module_name) + 1);
        strcpy(cc->modules[cc->module_count].name, module_name);
        cc->modules[cc->module_count].init_offset = init_offset;
        cc->module_count++;
    }

    /* Emit OP_IMPORT for runtime initialization */
    {
        uint16_t name_idx = image_add_const(cc->image, module_name);
        image_emit_u8(cc->image, OP_IMPORT);
        image_emit_u16(cc->image, name_idx);
    }

    /* Pop import stack */
    cc->import_depth--;
    free(cc->import_stack[cc->import_depth]);

    sh_list_free(ast);
    parser_destroy(&p);
    free(source);
    free(path);
}

static int free_ht_key_cb(const char *key, void *value, void *ctx)
{
    (void)value;
    (void)ctx;
    free((void *)key);
    return 0;
}

bytecode_image_t *compile(sh_list_t *program, const char *filename)
{
    compiler_t cc;
    bytecode_image_t *image;
    int i;

    compiler_init(&cc, filename);
    compile_program(&cc, program);
    image_emit_u8(cc.image, OP_HALT);

    if (cc.error_count > 0) {
        image_free(cc.image);
        for (i = 0; i < cc.func_count; i++) {
            free(cc.func_table[i].name);
        }
        for (i = 0; i < cc.module_count; i++) {
            free(cc.modules[i].name);
        }
        ht_foreach(&cc.imported, free_ht_key_cb, NULL);
        ht_destroy(&cc.imported);
        free((void *)cc.script_dir);
        return NULL;
    }

    image = cc.image;

    /* Transfer function table to the image */
    if (cc.func_count > 0) {
        image->funcs = xmalloc((size_t)cc.func_count * sizeof(vm_func_t));
        image->func_count = cc.func_count;
        for (i = 0; i < cc.func_count; i++) {
            image->funcs[i].name = cc.func_table[i].name;
            image->funcs[i].bytecode_offset = cc.func_table[i].bytecode_offset;
        }
    }

    /* Transfer module table to the image */
    if (cc.module_count > 0) {
        image->modules = xmalloc((size_t)cc.module_count * sizeof(vm_module_t));
        image->module_count = cc.module_count;
        for (i = 0; i < cc.module_count; i++) {
            image->modules[i].name = cc.modules[i].name;
            image->modules[i].init_offset = cc.modules[i].init_offset;
        }
    }

    ht_foreach(&cc.imported, free_ht_key_cb, NULL);
    ht_destroy(&cc.imported);
    free((void *)cc.script_dir);

    return image;
}
