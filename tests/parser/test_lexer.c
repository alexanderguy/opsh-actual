#include "../tap.h"
#include "foundation/arena.h"
#include "parser/lexer.h"

#include <stdlib.h>
#include <string.h>

static arena_t test_arena;

static void init_lexer(lexer_t *lex, const char *source, const char *filename)
{
    lexer_init(lex, source, filename);
    lex->arena = &test_arena;
}

static void test_basic_tokens(void)
{
    lexer_t lex;
    token_t tok;

    init_lexer(&lex, "echo hello world", "test");

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WORD, "word: echo");
    tap_is_str(tok.value, "echo", "word value: echo");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WORD, "word: hello");
    tap_is_str(tok.value, "hello", "word value: hello");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WORD, "word: world");
    tap_is_str(tok.value, "world", "word value: world");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_EOF, "EOF after words");
    token_free(&tok);

    lexer_destroy(&lex);
}

static void test_operators(void)
{
    lexer_t lex;
    token_t tok;

    init_lexer(&lex, "| || && ; ;; ( ) < > >> << <& >& <> >| <<<", "test");

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_PIPE, "operator: |");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_OR_IF, "operator: ||");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_AND_IF, "operator: &&");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_SEMI, "operator: ;");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_DSEMI, "operator: ;;");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_LPAREN, "operator: (");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_RPAREN, "operator: )");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_LESS, "operator: <");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_GREAT, "operator: >");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_DGREAT, "operator: >>");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_DLESS, "operator: <<");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_LESSAND, "operator: <&");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_GREATAND, "operator: >&");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_LESSGREAT, "operator: <>");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_CLOBBER, "operator: >|");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_TLESS, "operator: <<<");
    token_free(&tok);

    lexer_destroy(&lex);
}

static void test_reserved_words(void)
{
    lexer_t lex;
    token_t tok;

    init_lexer(&lex, "if then else elif fi do done case esac while until for in", "test");

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_IF, "reserved: if");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_THEN, "reserved: then");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_ELSE, "reserved: else");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_ELIF, "reserved: elif");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_FI, "reserved: fi");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_DO, "reserved: do");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_DONE, "reserved: done");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_CASE, "reserved: case");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_ESAC, "reserved: esac");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WHILE, "reserved: while");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_UNTIL, "reserved: until");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_FOR, "reserved: for");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_IN, "reserved: in");
    token_free(&tok);

    lexer_destroy(&lex);
}

static void test_param_expansion(void)
{
    lexer_t lex;
    token_t tok;

    init_lexer(&lex, "$var ${var} ${var:-default} ${#var}", "test");

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WORD, "$var is a word");
    tap_ok(tok.word != NULL, "$var has word structure");
    tap_is_int(tok.word->type, WP_PARAM, "$var is param expansion");
    tap_is_str(tok.word->part.param->name, "var", "$var name is var");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WORD, "${var} is a word");
    tap_ok(tok.word != NULL && tok.word->type == WP_PARAM, "${var} is param");
    tap_is_str(tok.word->part.param->name, "var", "${var} name is var");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WORD, "${var:-default} is a word");
    tap_ok(tok.word != NULL && tok.word->type == WP_PARAM, "${var:-default} is param");
    tap_is_int(tok.word->part.param->type, PE_DEFAULT, "${var:-default} is PE_DEFAULT");
    tap_ok(tok.word->part.param->flags & PE_COLON, "${var:-default} has PE_COLON");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WORD, "${#var} is a word");
    tap_ok(tok.word != NULL && tok.word->type == WP_PARAM, "${#var} is param");
    tap_ok(tok.word->part.param->flags & PE_STRLEN, "${#var} has PE_STRLEN");
    tap_is_str(tok.word->part.param->name, "var", "${#var} name is var");
    token_free(&tok);

    lexer_destroy(&lex);
}

static void test_command_substitution(void)
{
    lexer_t lex;
    token_t tok;

    init_lexer(&lex, "$(echo hello)", "test");

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WORD, "$(echo hello) is a word");
    tap_ok(tok.word != NULL, "has word structure");
    tap_is_int(tok.word->type, WP_CMDSUB, "is command substitution");
    token_free(&tok);

    lexer_destroy(&lex);
}

static void test_assignment(void)
{
    lexer_t lex;
    token_t tok;

    init_lexer(&lex, "VAR=value", "test");

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_ASSIGNMENT, "VAR=value is assignment");
    tap_is_str(tok.value, "VAR=value", "assignment raw value");
    token_free(&tok);

    lexer_destroy(&lex);
}

static void test_quoting(void)
{
    lexer_t lex;
    token_t tok;

    /* Single quotes */
    init_lexer(&lex, "'hello world'", "test");
    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WORD, "single-quoted is a word");
    tap_ok(tok.word != NULL && tok.word->type == WP_LITERAL, "single-quoted is string");
    tap_is_str(tok.word->part.string, "hello world", "single-quoted value preserved");
    token_free(&tok);
    lexer_destroy(&lex);

    /* Double quotes */
    init_lexer(&lex, "\"hello world\"", "test");
    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_WORD, "double-quoted is a word");
    tap_ok(tok.word != NULL && tok.word->type == WP_LITERAL, "double-quoted is string");
    tap_is_str(tok.word->part.string, "hello world", "double-quoted value preserved");
    token_free(&tok);
    lexer_destroy(&lex);
}

static void test_comments_and_newlines(void)
{
    lexer_t lex;
    token_t tok;

    init_lexer(&lex, "echo hello # comment\necho world", "test");

    tok = lexer_next(&lex);
    tap_is_str(tok.value, "echo", "word before comment");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_str(tok.value, "hello", "word before comment end");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_NEWLINE, "newline after comment");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_str(tok.value, "echo", "word after newline");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_str(tok.value, "world", "second word on next line");
    token_free(&tok);

    lexer_destroy(&lex);
}

static void test_io_number(void)
{
    lexer_t lex;
    token_t tok;

    init_lexer(&lex, "2>file", "test");

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_IO_NUMBER, "2> gives IO_NUMBER");
    tap_is_str(tok.value, "2", "IO_NUMBER value is 2");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_int(tok.type, TOK_GREAT, "redirection after IO_NUMBER");
    token_free(&tok);

    tok = lexer_next(&lex);
    tap_is_str(tok.value, "file", "filename after redirection");
    token_free(&tok);

    lexer_destroy(&lex);
}

int main(void)
{
    tap_plan(71);

    arena_init(&test_arena);

    test_basic_tokens();
    test_operators();
    test_reserved_words();
    test_param_expansion();
    test_command_substitution();
    test_assignment();
    test_quoting();
    test_comments_and_newlines();
    test_io_number();

    arena_destroy(&test_arena);

    return tap_done();
}
