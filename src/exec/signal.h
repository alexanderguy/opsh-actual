#ifndef OPSH_EXEC_SIGNAL_H
#define OPSH_EXEC_SIGNAL_H

#include <signal.h>
#include <stdbool.h>

/* Initialize signal handlers for the shell process */
void signal_init(void);

/* Reset signal handlers to defaults (called in child after fork) */
void signal_reset(void);

/* Check if any signal is pending */
bool signal_pending(void);

/* Clear all pending signal flags */
void signal_clear(void);

/* Check and clear specific signals */
bool signal_check_int(void);
bool signal_check_term(void);
bool signal_check_chld(void);

/* Get the signal number that is pending (0 if none) */
int signal_get_pending(void);

#endif /* OPSH_EXEC_SIGNAL_H */
