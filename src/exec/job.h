#ifndef OPSH_EXEC_JOB_H
#define OPSH_EXEC_JOB_H

#include <stdbool.h>
#include <sys/types.h>

/*
 * Job control for opsh.
 *
 * waitpid ownership:
 * - job_update() does non-blocking reaps (WNOHANG) for all children.
 * - job_wait_fg() does blocking waits for a specific job's process group.
 * - builtin_wait falls back to direct waitpid(pid) only for PIDs NOT in
 *   the job table.
 * - OP_EXEC_SIMPLE, OP_CMD_SUBST, OP_SUBSHELL wait for specific PIDs that
 *   are never registered in the job table.
 *
 * This avoids double-reap: job-table PIDs are only reaped by job_update
 * or job_wait_fg, and non-table PIDs are only waited by their caller.
 */

#define JOB_TABLE_MAX 64

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } job_state_t;

typedef struct {
    int id;          /* job number (1, 2, 3...) */
    pid_t pgid;      /* process group ID (== first PID in the job) */
    pid_t *pids;     /* PIDs of processes in this job (owned) */
    int pid_count;   /* number of PIDs */
    int *pid_status; /* per-PID exit status (owned, parallel to pids) */
    bool *pid_done;  /* per-PID done flag (owned, parallel to pids) */
    job_state_t state;
    int status;    /* exit status of last process (when DONE) */
    char *command; /* command string for display (owned) */
    bool notified; /* has terminal state change been reported? */
} job_t;

typedef struct {
    job_t jobs[JOB_TABLE_MAX];
    int count;    /* number of active jobs */
    int next_id;  /* next job ID to assign */
    int current;  /* job ID of current job (%+), 0 if none */
    int previous; /* job ID of previous job (%-), 0 if none */
} job_table_t;

/* Initialize a job table (zeroes everything) */
void job_table_init(job_table_t *jt);

/* Destroy a job table, freeing all job resources */
void job_table_destroy(job_table_t *jt);

/* Reset the job table (called in child after fork) */
void job_table_reset(job_table_t *jt);

/* Add a new job. Takes ownership of pids array and command string.
 * Returns the job ID, or -1 if the table is full. */
int job_add(job_table_t *jt, pid_t pgid, pid_t *pids, int pid_count, const char *command);

/* Find a job by ID. Returns NULL if not found. */
job_t *job_find_by_id(job_table_t *jt, int id);

/* Find a job by any PID in the job. Returns NULL if not found. */
job_t *job_find_by_pid(job_table_t *jt, pid_t pid);

/* Remove a completed job by ID, freeing its resources. */
void job_remove(job_table_t *jt, int id);

/* Remove all jobs marked DONE and notified. */
void job_reap_done(job_table_t *jt);

/* Non-blocking reap: calls waitpid(-1, WNOHANG|WUNTRACED) in a loop
 * to update job states. This is the ONLY function that calls waitpid
 * with WNOHANG. Returns the number of state changes. */
int job_update(job_table_t *jt);

/* Blocking wait for a specific job to finish or stop.
 * Calls waitpid in a loop (with WUNTRACED) for the job's processes.
 * Forwards SIGINT to the job's process group if received.
 * Returns the job's exit status. */
int job_wait_fg(job_table_t *jt, int job_id);

/* Parse a job spec string (%N, %%, %+, %-, %?string, %string).
 * Returns the job_t*, or NULL if not found. */
job_t *job_parse_spec(job_table_t *jt, const char *spec);

/* Get the current job (%+). Returns NULL if none. */
job_t *job_current(job_table_t *jt);

#endif /* OPSH_EXEC_JOB_H */
