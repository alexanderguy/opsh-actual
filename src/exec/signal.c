#include "exec/signal.h"

#include <string.h>

static volatile sig_atomic_t sig_int_pending = 0;
static volatile sig_atomic_t sig_term_pending = 0;
static volatile sig_atomic_t sig_chld_pending = 0;

static void handle_sigint(int sig)
{
    (void)sig;
    sig_int_pending = 1;
}

static void handle_sigterm(int sig)
{
    (void)sig;
    sig_term_pending = 1;
}

static void handle_sigchld(int sig)
{
    (void)sig;
    sig_chld_pending = 1;
}

void signal_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = 0; /* no SA_RESTART: let read/waitpid return EINTR */
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa, NULL);

    /* SIGCHLD: set pending flag so job_update() can reap.
     * No SA_NOCLDSTOP: we want SIGCHLD on stop and continue too,
     * so WUNTRACED and WCONTINUED in job_update work correctly. */
    sa.sa_handler = handle_sigchld;
    sigaction(SIGCHLD, &sa, NULL);

    /* Shell ignores SIGTSTP — children get SIG_DFL after fork */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGTSTP, &sa, NULL);

    /* Ignore SIGPIPE in the parent shell so broken pipes
     * produce EPIPE returns instead of killing the shell */
    sigaction(SIGPIPE, &sa, NULL);

    /* Ignore SIGTTIN/SIGTTOU so background shell doesn't stop
     * when trying to read/write the terminal */
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);
}

void signal_reset(void)
{
    /* Restore defaults for child processes */
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);

    sig_int_pending = 0;
    sig_term_pending = 0;
    sig_chld_pending = 0;
}

bool signal_pending(void)
{
    return sig_int_pending || sig_term_pending;
}

void signal_clear(void)
{
    sig_int_pending = 0;
    sig_term_pending = 0;
    sig_chld_pending = 0;
}

bool signal_check_int(void)
{
    if (sig_int_pending) {
        sig_int_pending = 0;
        return true;
    }
    return false;
}

bool signal_check_term(void)
{
    if (sig_term_pending) {
        sig_term_pending = 0;
        return true;
    }
    return false;
}

bool signal_check_chld(void)
{
    if (sig_chld_pending) {
        sig_chld_pending = 0;
        return true;
    }
    return false;
}

int signal_get_pending(void)
{
    if (sig_int_pending) {
        return SIGINT;
    }
    if (sig_term_pending) {
        return SIGTERM;
    }
    return 0;
}
