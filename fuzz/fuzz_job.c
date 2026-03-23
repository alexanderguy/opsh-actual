/*
 * libfuzzer harness for job table operations.
 *
 * Exercises job_add, job_find_by_id, job_find_by_pid, job_remove,
 * job_parse_spec, job_reap_done, and job_check_complete with
 * fuzz-derived operation sequences.
 */
#include "exec/job.h"
#include "foundation/util.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Operations driven by fuzz input */
enum {
    OP_ADD = 0,
    OP_FIND_ID,
    OP_FIND_PID,
    OP_REMOVE,
    OP_REAP_DONE,
    OP_PARSE_SPEC,
    OP_MARK_DONE,
    OP_MARK_STOPPED,
    OP_UPDATE_CHECK,
    OP_COUNT
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;

    job_table_t jt;
    job_table_init(&jt);

    size_t i = 0;
    while (i + 2 <= size) {
        uint8_t op = data[i] % OP_COUNT;
        uint8_t arg = data[i + 1];
        i += 2;

        switch (op) {
        case OP_ADD: {
            if (jt.count >= JOB_TABLE_MAX)
                break;
            int npids = (arg % 4) + 1;
            pid_t *pids = xmalloc((size_t)npids * sizeof(pid_t));
            int p;
            for (p = 0; p < npids; p++) {
                pids[p] = (pid_t)(arg * 100 + p + 1);
            }
            pid_t pgid = pids[0];
            int id = job_add(&jt, pgid, pids, npids, "fuzz_cmd");
            if (id < 0) {
                free(pids);
            }
            break;
        }
        case OP_FIND_ID: {
            int id = (int)(arg % 128) + 1;
            (void)job_find_by_id(&jt, id);
            break;
        }
        case OP_FIND_PID: {
            pid_t pid = (pid_t)(arg * 100 + 1);
            (void)job_find_by_pid(&jt, pid);
            break;
        }
        case OP_REMOVE: {
            if (jt.count > 0) {
                int idx = arg % jt.count;
                job_remove(&jt, jt.jobs[idx].id);
            }
            break;
        }
        case OP_REAP_DONE:
            job_reap_done(&jt);
            break;
        case OP_PARSE_SPEC: {
            /* Build a spec string from fuzz data */
            char spec[16];
            if (arg < 64) {
                snprintf(spec, sizeof(spec), "%%%d", (arg % 10) + 1);
            } else if (arg < 128) {
                snprintf(spec, sizeof(spec), "%%%%");
            } else if (arg < 192) {
                snprintf(spec, sizeof(spec), "%%-");
            } else {
                snprintf(spec, sizeof(spec), "%%?fuzz");
            }
            (void)job_parse_spec(&jt, spec);
            break;
        }
        case OP_MARK_DONE: {
            if (jt.count > 0) {
                int idx = arg % jt.count;
                job_t *j = &jt.jobs[idx];
                /* Mark all PIDs as done */
                int p;
                for (p = 0; p < j->pid_count; p++) {
                    j->pid_done[p] = true;
                    j->pid_status[p] = (int)(arg % 128);
                }
                j->state = JOB_DONE;
                j->status = j->pid_status[j->pid_count - 1];
                j->notified = true;
            }
            break;
        }
        case OP_MARK_STOPPED: {
            if (jt.count > 0) {
                int idx = arg % jt.count;
                jt.jobs[idx].state = JOB_STOPPED;
                jt.jobs[idx].status = 128 + 19; /* SIGSTOP */
            }
            break;
        }
        case OP_UPDATE_CHECK: {
            /* Exercise current/previous tracking */
            (void)job_current(&jt);
            break;
        }
        }
    }

    job_table_destroy(&jt);
    return 0;
}
