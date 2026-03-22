#ifndef OPSH_PARSER_AST_H
#define OPSH_PARSER_AST_H

#include "foundation/plist.h"
#include "foundation/util.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Forward declarations
 */
typedef struct sh_list sh_list_t;
typedef struct and_or and_or_t;
typedef struct command command_t;
typedef struct word_part word_part_t;
typedef struct param_exp param_exp_t;
typedef struct io_redir io_redir_t;
typedef struct cond_expr cond_expr_t;
typedef struct case_item case_item_t;

/*
 * Embedded command: either pre-parsed or unparsed backtick form.
 */
typedef struct {
    bool is_preparsed;
    union {
        sh_list_t *preparsed;
        char *unparsed;
    } u;
} cmd_subst_t;

/*
 * Word unit types
 */
typedef enum {
    WP_LITERAL, /* literal string */
    WP_PARAM,  /* parameter expansion */
    WP_CMDSUB, /* command substitution */
    WP_ARITH,  /* arithmetic expansion */
} word_part_type_t;

/*
 * Parameter expansion types
 */
typedef enum {
    PE_NONE = 0,
    PE_DEFAULT = 1,  /* ${n-s} or ${n:-s} */
    PE_ALTERNATE = 2,   /* ${n+s} or ${n:+s} */
    PE_ASSIGN = 3, /* ${n=s} or ${n:=s} */
    PE_ERROR = 4,  /* ${n?s} or ${n:?s} */
    PE_TRIM = 5,  /* ${n#m}, ${n##m}, ${n%m}, ${n%%m} */
    PE_REPLACE = 6,  /* ${n/m/s} or ${n//m/s} */
} param_exp_type_t;

/* Parameter expansion modifier flags */
#define PE_COLON (1 << 0)        /* colon variant (:) */
#define PE_PREFIX (1 << 1)    /* # variants (match at head) */
#define PE_SUFFIX (1 << 2)    /* % variants (match at tail) */
#define PE_LONGEST (1 << 3) /* ## or %% (longest match) */
#define PE_GLOBAL (1 << 4)     /* // (replace all) */
#define PE_STRLEN (1 << 5)       /* ${#n} (length) */

/*
 * Parameter expansion
 */
struct param_exp {
    char *name;
    param_exp_type_t type;
    unsigned flags;
    word_part_t *pattern;
    word_part_t *replacement;
};

/*
 * Word unit (linked list)
 */
struct word_part {
    word_part_type_t type;
    bool quoted; /* true if this unit appeared inside double quotes */
    word_part_t *next;
    union {
        char *string;      /* WP_LITERAL */
        param_exp_t *param; /* WP_PARAM */
        cmd_subst_t cmdsub; /* WP_CMDSUB */
        word_part_t *arith; /* WP_ARITH */
    } part;
};

/*
 * Redirection types
 */
typedef enum {
    REDIR_IN,   /* < */
    REDIR_OUT,  /* > */
    REDIR_APPEND,  /* >> */
    REDIR_CLOBBER, /* >| */
    REDIR_RDWR,   /* <> */
    REDIR_DUPIN,   /* <& */
    REDIR_DUPOUT,  /* >& */
    REDIR_CLOSE,   /* <&- or >&- */
    REDIR_HEREDOC,    /* << */
    REDIR_HEREDOC_STRIP,  /* <<- (strip tabs) */
    REDIR_HERESTR, /* <<< */
} io_redir_type_t;

/*
 * Redirection (linked list)
 */
struct io_redir {
    io_redir_type_t type;
    int fd;
    io_redir_t *next;
    word_part_t *target; /* target filename or here-doc content */
    bool heredoc_strip_tabs;         /* true if here-doc strips tabs */
    bool heredoc_expand;      /* true if here-doc expands parameters */
};

/*
 * Command types
 */
typedef enum {
    CT_SIMPLE,   /* simple command */
    CT_GROUP,    /* { ... } */
    CT_SUBSHELL, /* ( ... ) */
    CT_IF,       /* if/elif/else */
    CT_FOR,      /* for loop */
    CT_WHILE,    /* while loop */
    CT_UNTIL,    /* until loop */
    CT_CASE,     /* case */
    CT_FUNCDEF,  /* function definition */
    CT_BRACKET,  /* [[ ]] */
} command_type_t;

/*
 * If-clause condition+body pair (linked list)
 */
typedef struct if_clause {
    sh_list_t *condition; /* NULL for final else */
    sh_list_t *body;
    struct if_clause *next;
} if_clause_t;

/*
 * Case item: pattern list + body (linked list)
 */
typedef enum {
    CASE_BREAK,    /* ;; */
    CASE_FALLTHROUGH,     /* ;& */
    CASE_CONTINUE, /* ;| */
} caseend_t;

struct case_item {
    plist_t patterns; /* plist of word_part_t* */
    sh_list_t *body;
    caseend_t terminator;
    case_item_t *next;
};

/*
 * Double-bracket expression types
 */
typedef enum {
    COND_AND,    /* && */
    COND_OR,     /* || */
    COND_NOT,    /* ! */
    COND_UNARY,  /* -f, -d, etc. */
    COND_BINARY, /* ==, !=, -eq, etc. */
    COND_STRING, /* bare string (equivalent to -n) */
} cond_expr_type_t;

struct cond_expr {
    cond_expr_type_t type;
    union {
        struct {
            cond_expr_t *left;
            cond_expr_t *right;
        } andor; /* COND_AND, COND_OR */
        struct {
            cond_expr_t *child;
        } not; /* COND_NOT */
        struct {
            char *op; /* operator string, e.g. "-f" */
            word_part_t *arg;
        } unary; /* COND_UNARY */
        struct {
            char *op; /* operator string, e.g. "==" */
            word_part_t *left;
            word_part_t *right;
        } binary; /* COND_BINARY */
        struct {
            word_part_t *word;
        } string; /* COND_STRING */
    } u;
};

/*
 * Command node (discriminated union)
 */
struct command {
    command_type_t type;
    unsigned int lineno;
    io_redir_t *redirs;
    command_t *next;
    refcount_t refcount;
    union {
        struct {
            plist_t assigns; /* plist of word_part_t* (assignments) */
            plist_t words;   /* plist of word_part_t* (command words) */
        } simple;              /* CT_SIMPLE */
        struct {
            sh_list_t *body;
        } group; /* CT_GROUP, CT_SUBSHELL */
        struct {
            if_clause_t *clauses;
        } if_clause; /* CT_IF */
        struct {
            char *varname;
            plist_t wordlist; /* plist of word_part_t* */
            sh_list_t *body;
        } for_clause; /* CT_FOR */
        struct {
            sh_list_t *condition;
            sh_list_t *body;
        } while_clause; /* CT_WHILE, CT_UNTIL */
        struct {
            word_part_t *subject;
            case_item_t *items;
        } case_clause; /* CT_CASE */
        struct {
            char *name;
            command_t *body;
        } func_def; /* CT_FUNCDEF */
        struct {
            cond_expr_t *expr;
        } cond; /* CT_BRACKET */
    } u;
};

/*
 * Pipeline (linked list)
 */
struct and_or {
    command_t *commands; /* linked list of commands in the pipeline */
    bool negated;            /* ! prefix */
    bool connector;           /* true = &&, false = || (for linking to next) */
    and_or_t *next;
};

/*
 * And-or list
 */
struct sh_list {
    and_or_t *pipelines;
    bool background; /* & suffix */
    sh_list_t *next;
};

/*
 * AST node memory management
 */
void word_part_free(word_part_t *w);
void io_redir_free(io_redir_t *r);
void command_free(command_t *c);
void and_or_free(and_or_t *p);
void sh_list_free(sh_list_t *a);
void cond_expr_free(cond_expr_t *d);
void param_exp_free(param_exp_t *p);
void case_item_free(case_item_t *ci);
void if_clause_free(if_clause_t *ic);
void cmd_subst_free(cmd_subst_t *ec);

#endif /* OPSH_PARSER_AST_H */
