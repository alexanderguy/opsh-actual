#ifndef OPSH_PARSER_LEXER_H
#define OPSH_PARSER_LEXER_H

#include "foundation/arena.h"
#include "foundation/strbuf.h"
#include "parser/ast.h"

#include <stdbool.h>

/*
 * Token types
 */
typedef enum {
    /* End markers */
    TOK_EOF = 0,
    TOK_NEWLINE,

    /* Words and operators */
    TOK_WORD,       /* unquoted/quoted word */
    TOK_IO_NUMBER,  /* number immediately before < or > */
    TOK_ASSIGNMENT, /* word containing = in assignment position */

    /* Operators */
    TOK_PIPE,     /* | */
    TOK_AND_IF,   /* && */
    TOK_OR_IF,    /* || */
    TOK_SEMI,     /* ; */
    TOK_DSEMI,    /* ;; */
    TOK_SEMIAND,  /* ;& */
    TOK_SEMIPIPE, /* ;| */
    TOK_AMP,      /* & */
    TOK_LPAREN,   /* ( */
    TOK_RPAREN,   /* ) */
    TOK_LBRACE,   /* { */
    TOK_RBRACE,   /* } */

    /* Redirections */
    TOK_LESS,      /* < */
    TOK_GREAT,     /* > */
    TOK_DLESS,     /* << */
    TOK_DGREAT,    /* >> */
    TOK_LESSAND,   /* <& */
    TOK_GREATAND,  /* >& */
    TOK_LESSGREAT, /* <> */
    TOK_CLOBBER,   /* >| */
    TOK_TLESS,     /* <<< */
    TOK_DLESSDASH, /* <<- */

    /* Reserved words */
    TOK_IF,
    TOK_THEN,
    TOK_ELSE,
    TOK_ELIF,
    TOK_FI,
    TOK_DO,
    TOK_DONE,
    TOK_CASE,
    TOK_ESAC,
    TOK_WHILE,
    TOK_UNTIL,
    TOK_FOR,
    TOK_IN,
    TOK_FUNCTION,
    TOK_BANG,         /* ! */
    TOK_DBRACK_OPEN,  /* [[ */
    TOK_DBRACK_CLOSE, /* ]] */
} token_type_t;

/*
 * Token
 */
typedef struct {
    token_type_t type;
    char *value;       /* token text (owned; NULL for operators) */
    word_part_t *word; /* parsed word structure (for TOK_WORD/TOK_ASSIGNMENT) */
    unsigned int lineno;
} token_t;

/*
 * Lexer state
 */
typedef struct {
    const char *source;      /* source text */
    size_t pos;              /* current position */
    size_t length;           /* source length */
    unsigned int lineno;     /* current line number */
    const char *filename;    /* source filename (borrowed) */
    bool recognize_reserved; /* true when reserved words are valid */
    bool regex_mode;         /* true when parsing =~ RHS: ( ) | are word chars */
    token_t lookahead;       /* one-token lookahead */
    bool has_lookahead;
    int error_count;
    arena_t *arena;            /* bump allocator for word_part/param_exp nodes (borrowed) */
    comment_t *comments;       /* collected comments (linked list head) */
    comment_t **comments_tail; /* tail pointer for O(1) append */
} lexer_t;

/* Initialize lexer from a string */
void lexer_init(lexer_t *lex, const char *source, const char *filename);

/* Free lexer resources */
void lexer_destroy(lexer_t *lex);

/* Get the next token */
token_t lexer_next(lexer_t *lex);

/* Peek at the next token without consuming it (returned pointer is borrowed) */
const token_t *lexer_peek(lexer_t *lex);

/* Free a token's owned resources */
void token_free(token_t *tok);

/* Return true if token is a word-like token */
bool token_is_word(const token_t *tok);

/* Return the name of a token type (for error messages) */
const char *token_type_name(token_type_t type);

/* Parse a here-document body string for parameter/command expansion.
 * Returns a word chain with WP_LITERAL, WP_PARAM, WP_CMDSUB nodes.
 * Caller owns the result. */
word_part_t *lexer_parse_heredoc_body(const char *body, const char *filename, arena_t *arena);

#endif /* OPSH_PARSER_LEXER_H */
