#ifndef OPSH_PARSER_PARSER_H
#define OPSH_PARSER_PARSER_H

#include "foundation/arena.h"
#include "parser/ast.h"
#include "parser/lexer.h"

#define MAX_PARSE_ERRORS 64

/*
 * A collected parse error.
 */
typedef struct {
    unsigned int lineno;
    char *message;
} parse_error_t;

/*
 * Parser state
 */
typedef struct {
    lexer_t lexer;
    arena_t arena;
    int error_count;
    const char *filename;
    parse_error_t errors[MAX_PARSE_ERRORS];
} parser_t;

/* Initialize a parser for the given source string */
void parser_init(parser_t *p, const char *source, const char *filename);

/* Free parser resources */
void parser_destroy(parser_t *p);

/* Parse a complete program (list of and-or lists). Returns NULL on empty input. */
sh_list_t *parser_parse(parser_t *p);

/* Return total error count (parser + lexer) */
int parser_error_count(const parser_t *p);

#endif /* OPSH_PARSER_PARSER_H */
