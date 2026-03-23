#include "../tap.h"
#include "exec/job.h"
#include "foundation/util.h"

#include <stdlib.h>
#include <string.h>

static void test_job_table_init(void)
{
    job_table_t jt;
    job_table_init(&jt);
    tap_is_int(jt.count, 0, "init: count is 0");
    tap_is_int(jt.next_id, 1, "init: next_id is 1");
    tap_is_int(jt.current, 0, "init: current is 0");
    tap_is_int(jt.previous, 0, "init: previous is 0");
    job_table_destroy(&jt);
}

static void test_job_add(void)
{
    job_table_t jt;
    job_table_init(&jt);

    pid_t *pids = xmalloc(sizeof(pid_t));
    pids[0] = 1234;
    int id = job_add(&jt, 1234, pids, 1, "sleep 10");
    tap_is_int(id, 1, "add: first job gets id 1");
    tap_is_int(jt.count, 1, "add: count is 1");
    tap_is_int(jt.current, 1, "add: current is 1");

    pid_t *pids2 = xmalloc(sizeof(pid_t));
    pids2[0] = 5678;
    int id2 = job_add(&jt, 5678, pids2, 1, "echo hello");
    tap_is_int(id2, 2, "add: second job gets id 2");
    tap_is_int(jt.count, 2, "add: count is 2");
    tap_is_int(jt.current, 2, "add: current updated to 2");
    tap_is_int(jt.previous, 1, "add: previous is 1");

    job_table_destroy(&jt);
}

static void test_job_find_by_id(void)
{
    job_table_t jt;
    job_table_init(&jt);

    pid_t *pids = xmalloc(sizeof(pid_t));
    pids[0] = 1234;
    job_add(&jt, 1234, pids, 1, "test");

    job_t *j = job_find_by_id(&jt, 1);
    tap_ok(j != NULL, "find_by_id: found job 1");
    tap_is_int(j->pgid, 1234, "find_by_id: pgid matches");
    tap_is_str(j->command, "test", "find_by_id: command matches");

    job_t *missing = job_find_by_id(&jt, 99);
    tap_ok(missing == NULL, "find_by_id: returns NULL for missing");

    job_table_destroy(&jt);
}

static void test_job_find_by_pid(void)
{
    job_table_t jt;
    job_table_init(&jt);

    /* Pipeline with 3 PIDs */
    pid_t *pids = xmalloc(3 * sizeof(pid_t));
    pids[0] = 100;
    pids[1] = 200;
    pids[2] = 300;
    job_add(&jt, 100, pids, 3, "a | b | c");

    tap_ok(job_find_by_pid(&jt, 100) != NULL, "find_by_pid: finds first pid");
    tap_ok(job_find_by_pid(&jt, 200) != NULL, "find_by_pid: finds middle pid");
    tap_ok(job_find_by_pid(&jt, 300) != NULL, "find_by_pid: finds last pid");
    tap_ok(job_find_by_pid(&jt, 999) == NULL, "find_by_pid: NULL for unknown");

    job_table_destroy(&jt);
}

static void test_job_remove(void)
{
    job_table_t jt;
    job_table_init(&jt);

    pid_t *pids1 = xmalloc(sizeof(pid_t));
    pids1[0] = 100;
    job_add(&jt, 100, pids1, 1, "job1");

    pid_t *pids2 = xmalloc(sizeof(pid_t));
    pids2[0] = 200;
    job_add(&jt, 200, pids2, 1, "job2");

    tap_is_int(jt.count, 2, "remove: count starts at 2");
    job_remove(&jt, 1);
    tap_is_int(jt.count, 1, "remove: count is 1 after removal");
    tap_ok(job_find_by_id(&jt, 1) == NULL, "remove: job 1 gone");
    tap_ok(job_find_by_id(&jt, 2) != NULL, "remove: job 2 still exists");

    job_table_destroy(&jt);
}

static void test_job_parse_spec(void)
{
    job_table_t jt;
    job_table_init(&jt);

    pid_t *pids1 = xmalloc(sizeof(pid_t));
    pids1[0] = 100;
    job_add(&jt, 100, pids1, 1, "sleep 10");

    pid_t *pids2 = xmalloc(sizeof(pid_t));
    pids2[0] = 200;
    job_add(&jt, 200, pids2, 1, "echo hello");

    /* %N — by number */
    job_t *j = job_parse_spec(&jt, "%1");
    tap_ok(j != NULL, "parse_spec %%1: found");
    tap_is_int(j->id, 1, "parse_spec %%1: correct id");

    j = job_parse_spec(&jt, "%2");
    tap_ok(j != NULL, "parse_spec %%2: found");
    tap_is_int(j->id, 2, "parse_spec %%2: correct id");

    /* %% and %+ — current job */
    j = job_parse_spec(&jt, "%%");
    tap_ok(j != NULL, "parse_spec %%%%: found current");
    tap_is_int(j->id, 2, "parse_spec %%%%: is most recent");

    j = job_parse_spec(&jt, "%+");
    tap_ok(j != NULL, "parse_spec %%+: found current");

    /* %- — previous job */
    j = job_parse_spec(&jt, "%-");
    tap_ok(j != NULL, "parse_spec %%-: found previous");
    tap_is_int(j->id, 1, "parse_spec %%-: correct id");

    /* %?string — search in command */
    j = job_parse_spec(&jt, "%?sleep");
    tap_ok(j != NULL, "parse_spec %%?sleep: found");
    tap_is_int(j->id, 1, "parse_spec %%?sleep: correct id");

    /* %string — prefix match */
    j = job_parse_spec(&jt, "%echo");
    tap_ok(j != NULL, "parse_spec %%echo: found");
    tap_is_int(j->id, 2, "parse_spec %%echo: correct id");

    /* Missing */
    tap_ok(job_parse_spec(&jt, "%99") == NULL, "parse_spec %%99: not found");
    tap_ok(job_parse_spec(&jt, "%?zzz") == NULL, "parse_spec %%?zzz: not found");

    job_table_destroy(&jt);
}

static void test_job_reap_done(void)
{
    job_table_t jt;
    job_table_init(&jt);

    pid_t *pids1 = xmalloc(sizeof(pid_t));
    pids1[0] = 100;
    job_add(&jt, 100, pids1, 1, "done_job");
    jt.jobs[0].state = JOB_DONE;
    jt.jobs[0].notified = true;

    pid_t *pids2 = xmalloc(sizeof(pid_t));
    pids2[0] = 200;
    job_add(&jt, 200, pids2, 1, "running_job");

    tap_is_int(jt.count, 2, "reap_done: starts with 2 jobs");
    job_reap_done(&jt);
    tap_is_int(jt.count, 1, "reap_done: done+notified job removed");
    tap_ok(job_find_by_id(&jt, 2) != NULL, "reap_done: running job still exists");

    job_table_destroy(&jt);
}

static void test_job_reset(void)
{
    job_table_t jt;
    job_table_init(&jt);

    pid_t *pids = xmalloc(sizeof(pid_t));
    pids[0] = 100;
    job_add(&jt, 100, pids, 1, "test");
    tap_is_int(jt.count, 1, "reset: has 1 job");

    /* Reset simulates what happens in child after fork — doesn't free,
     * just zeroes out the table so child doesn't manage parent's jobs */
    job_table_reset(&jt);
    tap_is_int(jt.count, 0, "reset: count is 0");
    tap_is_int(jt.next_id, 1, "reset: next_id reset to 1");

    /* Note: we leaked the pids/command from the original job_add.
     * This is intentional — in the real child path, the parent owns that memory. */
}

static void test_job_current_previous(void)
{
    job_table_t jt;
    job_table_init(&jt);

    tap_ok(job_current(&jt) == NULL, "current: NULL when empty");

    pid_t *pids1 = xmalloc(sizeof(pid_t));
    pids1[0] = 100;
    job_add(&jt, 100, pids1, 1, "first");
    tap_ok(job_current(&jt) != NULL, "current: non-NULL after first add");
    tap_is_int(job_current(&jt)->id, 1, "current: is job 1");

    pid_t *pids2 = xmalloc(sizeof(pid_t));
    pids2[0] = 200;
    job_add(&jt, 200, pids2, 1, "second");
    tap_is_int(job_current(&jt)->id, 2, "current: updated to job 2");

    /* Remove current — should fall back to previous */
    job_remove(&jt, 2);
    tap_ok(job_current(&jt) != NULL, "current: non-NULL after removing current");
    tap_is_int(job_current(&jt)->id, 1, "current: falls back to previous");

    job_table_destroy(&jt);
}

static void test_next_id_monotonic(void)
{
    job_table_t jt;
    job_table_init(&jt);

    int i;
    for (i = 0; i < 200; i++) {
        pid_t *pids = xmalloc(sizeof(pid_t));
        pids[0] = (pid_t)(1000 + i);
        int id = job_add(&jt, pids[0], pids, 1, "cycle");
        (void)id;
        jt.jobs[jt.count - 1].state = JOB_DONE;
        jt.jobs[jt.count - 1].notified = true;
        job_reap_done(&jt);
    }
    tap_is_int(jt.next_id, 201, "next_id: monotonically grows after 200 add/remove cycles");
    tap_is_int(jt.count, 0, "next_id: table is empty after all removed");
    job_table_destroy(&jt);
}

static void test_full_table(void)
{
    job_table_t jt;
    job_table_init(&jt);

    int i;
    bool all_ok = true;
    for (i = 0; i < JOB_TABLE_MAX; i++) {
        pid_t *pids = xmalloc(sizeof(pid_t));
        pids[0] = (pid_t)(1000 + i);
        int id = job_add(&jt, pids[0], pids, 1, "fill");
        if (id < 0) {
            all_ok = false;
        }
    }
    tap_ok(all_ok, "full_table: all 64 adds succeeded");
    tap_is_int(jt.count, JOB_TABLE_MAX, "full_table: count is max");

    /* One more should fail */
    pid_t *extra = xmalloc(sizeof(pid_t));
    extra[0] = 9999;
    int overflow_id = job_add(&jt, 9999, extra, 1, "overflow");
    tap_is_int(overflow_id, -1, "full_table: returns -1 when full");
    free(extra); /* caller owns pids on failure */

    job_table_destroy(&jt);
}

int main(void)
{
    tap_plan(55);

    test_job_table_init();
    test_job_add();
    test_job_find_by_id();
    test_job_find_by_pid();
    test_job_remove();
    test_job_parse_spec();
    test_job_reap_done();
    test_job_reset();
    test_job_current_previous();
    test_next_id_monotonic();
    test_full_table();

    return tap_done();
}
