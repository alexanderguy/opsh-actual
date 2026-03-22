#ifndef OPSH_VALUE_H
#define OPSH_VALUE_H

#include <stdint.h>

/*
 * Value type tags for the VM's tagged union.
 */
typedef enum {
    VT_NONE = 0,     /* no value (unset variable, exhausted iterator) */
    VT_STRING = 1,   /* char * (UTF-8) */
    VT_INTEGER = 2,  /* int64_t */
    VT_ARRAY = 3,    /* array of char * */
    VT_ITERATOR = 4, /* iteration state for for-loops */
} valuetype_t;

/*
 * Iterator state (for VT_ITERATOR values on the operand stack).
 */
typedef struct {
    char **elements; /* flattened array of strings (owned) */
    int count;       /* total element count */
    int position;    /* current position (next to yield) */
} iterator_t;

/*
 * Tagged union value type used on the operand stack and in variable storage.
 */
typedef struct {
    valuetype_t type;
    union {
        char *string;    /* VT_STRING (owned) */
        int64_t integer; /* VT_INTEGER */
        struct {
            char **elements; /* array of char * (each owned) */
            int count;
        } array;             /* VT_ARRAY */
        iterator_t iterator; /* VT_ITERATOR */
    } data;
} value_t;

/* Convenience constructors */
static inline value_t value_none(void)
{
    value_t v;
    v.type = VT_NONE;
    return v;
}

static inline value_t value_string(char *s)
{
    value_t v;
    v.type = VT_STRING;
    v.data.string = s;
    return v;
}

static inline value_t value_integer(int64_t n)
{
    value_t v;
    v.type = VT_INTEGER;
    v.data.integer = n;
    return v;
}

/* Free a value's owned resources (does not free the value_t itself) */
void value_destroy(value_t *v);

/* Deep copy a value */
value_t value_clone(const value_t *v);

/* Convert a value to a string (caller owns result). Returns "" for VT_NONE. */
char *value_to_string(const value_t *v);

/* Convert a value to an integer. Returns 0 for non-numeric/empty. */
int64_t value_to_integer(const value_t *v);

#endif /* OPSH_VALUE_H */
