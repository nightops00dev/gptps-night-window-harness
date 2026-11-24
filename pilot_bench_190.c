/* pilot_bench_190.c - the night-batch pilot, pass one, against GPTPS v1.9.0.
 *
 * The issue #12 scenario at bench scale: the MRP consolidation job is big and
 * slow, the three morning reports are small and due hours earlier. Under a
 * first-come ordering the MRP job submitted first would hold the single
 * executor slot for its whole run and every report would wait behind it.
 * This run installs gptps_sched_edf (earliest deadline first) and
 * gptps_sched_shed_late (refuse work already past its due time) and measures:
 *
 *   1. start order vs deadline order (EDF must invert the FIFO order),
 *   2. the shed-late constraint refusing a job whose deadline already passed,
 *   3. clean completion of every on-time job, in deadline order.
 *
 * MANUAL mode, single concurrency slot, so admission order is observable.
 * Timings are process CPU ms, which track wall time in single-threaded mode.
 * Not part of the repo; a measurement harness.
 */
#include "gptps.h"
#include "addons/gptps_sched.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *name;
    uint64_t    deadline_ms;   /* from its submit until due */
    long        run_ms;        /* simulated work */
    uint64_t    declared_mem;
    int         start_seq;     /* fill at task start */
    long        start_clock;
    uint64_t    start_now;
    int         finished;
} job;

static job jobs[] = {
    /* submitted in THIS order: MRP first, so FIFO would run it first */
    { "mrp",       3600000u, 600, 512u << 20, -1, 0, 0, 0 },
    { "rpt_fin",    600000u, 100,  64u << 20, -1, 0, 0, 0 },
    { "rpt_sales", 1200000u, 100,  64u << 20, -1, 0, 0, 0 },
    { "rpt_inv",   1800000u, 100,  64u << 20, -1, 0, 0, 0 },
    { "rpt_stale",       1u, 100,  64u << 20, -1, 0, 0, 0 }, /* due 1 ms after submit */
};
#define NJOBS (sizeof jobs / sizeof jobs[0])

static int    g_seq;
static int    g_outage;
static uint64_t g_dead_letters;

static void spin_ms(long ms)
{
    clock_t t0 = clock();
    volatile unsigned long sink = 0;
    while ((long)((clock() - t0) * 1000 / CLOCKS_PER_SEC) < ms) ++sink;
}

static gptps_status run_job(gptps_ctx *ctx, void *ud)
{
    size_t n, i;
    const char *p = (const char *)gptps_payload(ctx, &n);
    job *j = (job *)ud;
    unsigned long sum = 0;
    j->start_seq = g_seq++;
    j->start_clock = clock();
    j->start_now = gptps_now_ms(ctx);
    for (i = 0; i < n; ++i) sum += (unsigned char)p[i];
    spin_ms(j->run_ms);
    return gptps_result_set(ctx, &sum, sizeof sum);
}

static void observe(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_DEAD_LETTERED) ++g_dead_letters;
}

static int pump(gptps *e)
{
    int step;
    size_t ran = 1;
    for (step = 0; step < 1000 && ran; ++step)
        if (gptps_step(e, &ran) != GPTPS_OK) return 0;
    return ran == 0;
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    gptps_sched *edf = NULL, *shed = NULL;
    gptps_handle h[NJOBS];
    size_t i;
    int started_ok = 0;

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 4096ull << 20;
    REQUIRE(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    REQUIRE(gptps_register_observer(e, observe, NULL) == GPTPS_OK);

    /* admission discipline: EDF owns the scheduler seam, shed_late is a
     * constraint that composes with it */
    REQUIRE(gptps_sched_edf(e, &edf) == GPTPS_OK);
    /* grace 0: the stale job is 20 ms past its 1 ms deadline by first
     * admission, well outside any grace, so it must be refused */
    REQUIRE(gptps_sched_shed_late(e, 0, &shed) == GPTPS_OK);

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 0;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    for (i = 0; i < NJOBS; ++i) {
        d.name = jobs[i].name;
        d.run = run_job;
        d.user_data = &jobs[i];
        d.default_cost.mem_bytes = jobs[i].declared_mem;
        REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);
    }

    /* NIGHT: submit in FIFO-adverse order - the big MRP job first */
    for (i = 0; i < NJOBS; ++i) {
        gptps_submit_options o;
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.flags = GPTPS_SUBMIT_DEADLINE;
        o.deadline_in_ms = jobs[i].deadline_ms;
        REQUIRE(gptps_submit_ex(e, jobs[i].name, jobs[i].name,
                                strlen(jobs[i].name), &o, &h[i]) == GPTPS_OK);
    }
    /* let the stale job's 1 ms deadline elapse so shed_late sees it past due */
    spin_ms(20);
    REQUIRE(pump(e));
    REQUIRE(gptps_shutdown(e) == GPTPS_OK);

    /* MORNING: the ledger */
    for (i = 0; i < NJOBS; ++i)
        if (jobs[i].start_seq >= 0) ++started_ok;

    printf("GPTPS pilot pass one - v1.9.0, EDF + shed_late, single slot\n");
    printf("%-10s %-14s %-9s %-10s %s\n",
           "job", "deadline(ms)", "started", "start_seq", "result");
    for (i = 0; i < NJOBS; ++i) {
        const char *res = jobs[i].start_seq >= 0 ? "finished"
                        : "not started";
        printf("%-10s %-14llu %-9s %-10d %s\n",
               jobs[i].name,
               (unsigned long long)jobs[i].deadline_ms,
               jobs[i].start_seq >= 0 ? "yes" : "no",
               jobs[i].start_seq,
               res);
    }
    printf("shed_late refused: %llu (expect 1: rpt_stale)\n",
           (unsigned long long)gptps_sched_shed_count(shed));
    printf("dead letters:      %llu\n", (unsigned long long)g_dead_letters);

    /* the assertion that makes this a pilot, not a smoke test:
     * among the jobs that started, start order must follow strictly
     * increasing deadline order (EDF), and MRP must be LAST even though it
     * was submitted FIRST. That inversion is EDF doing its job. */
    {
        int inv_ok = 1;
        uint64_t last_dl = 0;
        int k;
        for (k = 0; k < 4; ++k) {
            job *j = NULL;
            int s;
            for (s = 0; s < (int)NJOBS; ++s)
                if (jobs[s].start_seq == k) { j = &jobs[s]; break; }
            if (!j) { inv_ok = 0; break; }
            if (j->deadline_ms <= last_dl) { inv_ok = 0; break; }
            last_dl = j->deadline_ms;
        }
        /* the stale job must have been refused, never started */
        if (jobs[4].start_seq >= 0) inv_ok = 0;
        printf("EDF order (deadline order, MRP last): %s\n",
               inv_ok ? "HOLDS" : "FAILS");
        REQUIRE(inv_ok);
    }

    /* the printed ledger is asserted too, not just shown: 4 jobs started,
     * exactly one shed refusal, exactly one dead letter */
    REQUIRE(started_ok == 4);
    REQUIRE(gptps_sched_shed_count(shed) == 1);
    REQUIRE(g_dead_letters == 1);
    printf("started=4, shed=1, dead letters=1: HOLDS\n");

    gptps_sched_close(edf);
    gptps_sched_close(shed);
    (void)g_outage;
    return 0;
}
