#ifndef OPSH_VM_ARITH_H
#define OPSH_VM_ARITH_H

#include "exec/variable.h"

#include <stdint.h>

typedef enum {
    ARITH_OK = 0,
    ARITH_ERR_SYNTAX,
    ARITH_ERR_DIV_ZERO,
    ARITH_ERR_DEPTH,
} arith_error_t;

/* Evaluate a POSIX arithmetic expression.
 * Variable references are resolved via env. Assignment operators modify env.
 * Returns the result; sets *err on failure. */
int64_t arith_eval(const char *expr, environ_t *env, arith_error_t *err);

#endif /* OPSH_VM_ARITH_H */
