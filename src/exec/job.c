#include "exec/job.h"

#include "exec/signal.h"
#include "foundation/util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

void job_table_init(job_table_t *jt)
{
    memset(jt, 0, sizeof(*jt));
    jt->next_id = 1;
}

void job_table_destroy(job_table_t *jt)
{
    int i;
    for (i = 0; i < jt->count; i++) {
        free(jt->jobs[i].pids);
        free(jt->jobs[i].pid_status);
        free(jt->jobs[i].pid_done);
        free(jt->jobs[i].command);
    }
    memset(jt, 0, sizeof(*jt));
}

void job_table_reset(job_table_t *jt)
{
    /* Called in child after fork. Don't free — parent owns the memory.
     * Just zero out so the child doesn't try to manage parent's jobs. */
    memset(jt, 0, sizeof(*jt));
    jt->next_id = 1;
}

int job_add(job_table_t *jt, pid_t pgid, pid_t *pids, int pid_count, const char *command)
{
    if (jt->count >= JOB_TABLE_MAX) {
        fprintf(stderr, "opsh: job table full\n");
        return -1;
    }

    int id = jt->next_id++;
    job_t *j = &jt->jobs[jt->count];
    j->id = id;
    j->pgid = pgid;
    j->pids = pids;
    j->pid_count = pid_count;
    j->pid_status = xcalloc((size_t)pid_count, sizeof(int));
    j->pid_done = xcalloc((size_t)pid_count, sizeof(bool));
    j->state = JOB_RUNNING;
    j->status = 0;
    j->command = xstrdup(command);
    j->notified = false;
    jt->count++;

    /* Update current/previous */
    jt->previous = jt->current;
    jt->current = id;

    return id;
}

job_t *job_find_by_id(job_table_t *jt, int id)
{
    int i;
    for (i = 0; i < jt->count; i++) {
        if (jt->jobs[i].id == id) {
            return &jt->jobs[i];
        }
    }
    return NULL;
}

job_t *job_find_by_pid(job_table_t *jt, pid_t pid)
{
    int i, p;
    for (i = 0; i < jt->count; i++) {
        for (p = 0; p < jt->jobs[i].pid_count; p++) {
            if (jt->jobs[i].pids[p] == pid) {
                return &jt->jobs[i];
            }
        }
    }
    return NULL;
}

static void job_free_entry(job_t *j)
{
    free(j->pids);
    free(j->pid_status);
    free(j->pid_done);
    free(j->command);
    memset(j, 0, sizeof(*j));
}

void job_remove(job_table_t *jt, int id)
{
    int i;
    for (i = 0; i < jt->count; i++) {
        if (jt->jobs[i].id == id) {
            /* Update current/previous if needed */
            if (jt->current == id) {
                jt->current = jt->previous;
                jt->previous = 0;
            } else if (jt->previous == id) {
                jt->previous = 0;
            }
            job_free_entry(&jt->jobs[i]);
            /* Shift remaining jobs down */
            if (i < jt->count - 1) {
                memmove(&jt->jobs[i], &jt->jobs[i + 1],
                        (size_t)(jt->count - 1 - i) * sizeof(job_t));
            }
            jt->count--;
            return;
        }
    }
}

void job_reap_done(job_table_t *jt)
{
    int i = 0;
    while (i < jt->count) {
        if (jt->jobs[i].state == JOB_DONE && jt->jobs[i].notified) {
            job_remove(jt, jt->jobs[i].id);
            /* Don't increment i — removal shifted elements down */
        } else {
            i++;
        }
    }
}

/*
 * Check if all PIDs in a job are done. If so, mark the job as DONE
 * with the exit status of the last process.
 */
static void job_check_complete(job_t *j)
{
    int p;
    for (p = 0; p < j->pid_count; p++) {
        if (!j->pid_done[p]) {
            return; /* Still has running/stopped processes */
        }
    }
    j->state = JOB_DONE;
    /* Status is that of the last process in the pipeline */
    j->status = j->pid_status[j->pid_count - 1];
}

int job_update(job_table_t *jt)
{
    int changes = 0;
    int wstatus;
    pid_t pid;

    for (;;) {
        pid = waitpid(-1, &wstatus, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0) {
            break;
        }

        job_t *j = job_find_by_pid(jt, pid);
        if (j == NULL) {
            /* Unknown child — not in job table. Ignore. */
            continue;
        }

        /* Find which process in the job this is */
        int p;
        for (p = 0; p < j->pid_count; p++) {
            if (j->pids[p] == pid) {
                break;
            }
        }
        if (p >= j->pid_count) {
            continue;
        }

        if (WIFEXITED(wstatus)) {
            j->pid_done[p] = true;
            j->pid_status[p] = WEXITSTATUS(wstatus);
            changes++;
            job_check_complete(j);
        } else if (WIFSIGNALED(wstatus)) {
            j->pid_done[p] = true;
            j->pid_status[p] = 128 + WTERMSIG(wstatus);
            changes++;
            job_check_complete(j);
        } else if (WIFSTOPPED(wstatus)) {
            j->state = JOB_STOPPED;
            j->status = 128 + WSTOPSIG(wstatus);
            j->notified = false;
            changes++;
        } else if (WIFCONTINUED(wstatus)) {
            j->state = JOB_RUNNING;
            j->notified = false;
            changes++;
        }
    }

    return changes;
}

int job_wait_fg(job_table_t *jt, int job_id)
{
    job_t *j = job_find_by_id(jt, job_id);
    if (j == NULL) {
        return 127;
    }

    /* Block until the job finishes or stops */
    while (j->state == JOB_RUNNING) {
        int wstatus;
        pid_t pid = waitpid(-j->pgid, &wstatus, WUNTRACED | WCONTINUED);

        if (pid < 0) {
            if (errno == EINTR) {
                /* Check for signals — forward SIGINT to the job's group */
                if (signal_check_int()) {
                    kill(-j->pgid, SIGINT);
                }
                if (signal_check_term()) {
                    kill(-j->pgid, SIGTERM);
                }
                continue;
            }
            /* ECHILD: all children gone */
            break;
        }

        /* Update the specific PID in the job */
        int p;
        for (p = 0; p < j->pid_count; p++) {
            if (j->pids[p] == pid) {
                break;
            }
        }
        if (p >= j->pid_count) {
            continue;
        }

        if (WIFEXITED(wstatus)) {
            j->pid_done[p] = true;
            j->pid_status[p] = WEXITSTATUS(wstatus);
            job_check_complete(j);
        } else if (WIFSIGNALED(wstatus)) {
            j->pid_done[p] = true;
            j->pid_status[p] = 128 + WTERMSIG(wstatus);
            job_check_complete(j);
        } else if (WIFSTOPPED(wstatus)) {
            j->state = JOB_STOPPED;
            j->status = 128 + WSTOPSIG(wstatus);
            j->notified = false;
            break; /* Return control to the shell */
        } else if (WIFCONTINUED(wstatus)) {
            j->state = JOB_RUNNING;
            /* Keep waiting */
        }
    }

    return j->status;
}

job_t *job_parse_spec(job_table_t *jt, const char *spec)
{
    if (spec == NULL || spec[0] != '%') {
        return NULL;
    }

    /* %% or %+ — current job */
    if (spec[1] == '%' || spec[1] == '+' || spec[1] == '\0') {
        return job_current(jt);
    }

    /* %- — previous job */
    if (spec[1] == '-') {
        if (jt->previous > 0) {
            return job_find_by_id(jt, jt->previous);
        }
        return NULL;
    }

    /* %N — job by number */
    if (spec[1] >= '1' && spec[1] <= '9') {
        char *endp;
        long n = strtol(spec + 1, &endp, 10);
        if (*endp == '\0') {
            return job_find_by_id(jt, (int)n);
        }
        return NULL;
    }

    /* %?string — job whose command contains string */
    if (spec[1] == '?') {
        const char *needle = spec + 2;
        int i;
        for (i = 0; i < jt->count; i++) {
            if (jt->jobs[i].command && strstr(jt->jobs[i].command, needle)) {
                return &jt->jobs[i];
            }
        }
        return NULL;
    }

    /* %string — job whose command starts with string */
    {
        const char *prefix = spec + 1;
        size_t len = strlen(prefix);
        int i;
        for (i = 0; i < jt->count; i++) {
            if (jt->jobs[i].command && strncmp(jt->jobs[i].command, prefix, len) == 0) {
                return &jt->jobs[i];
            }
        }
    }

    return NULL;
}

job_t *job_current(job_table_t *jt)
{
    if (jt->current > 0) {
        return job_find_by_id(jt, jt->current);
    }
    /* Fall back to most recent job if current was removed */
    if (jt->count > 0) {
        return &jt->jobs[jt->count - 1];
    }
    return NULL;
}
