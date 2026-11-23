/* pilot_bench_110.c - pilot pass two against GPTPS v1.10.0.
 *
 * Exercises the failure path the night window lives on, all through the
 * gptps_retry add-on and the 1.10 PROGRAM measurements:
 *
 *   item "good"     exits 0                        -> finishes
 *   item "bad"      exits 2 (bad input)             -> dead-lettered AT ONCE,
 *                                                      GPTPS_E_DENIED, 1 attempt,
 *                                                      retries NOT spent
 *   item "licence"  exits 3 (licence)             -> retried once after >= 300 ms
 *                                                      (the "retry much later" rule,
 *                                                      scaled from 1 h for the bench),
 *                                                      then dead-lettered, 2 attempts
 *   item "runtime"  exits 1 (runtime/lock)          -> retried per policy (2 retries,
 *                                                      3 attempts), then dead-lettered
 *
 * Every PROGRAM job's stderr is captured (stderr_tail_bytes = 512) and the
 * morning report reads the tail off the dead letter: the LAST bytes, carrying
 * TAIL-EVIDENCE, not the first chatter.
 *
 * MANUAL mode; wall clock via the OS, not process CPU, because waits now
 * dominate. Not part of the repo; a measurement harness.
 */
#include "gptps.h"
#include "addons/gptps_retry.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
static unsigned long long wall_ms(void) { return GetTickCount64(); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static const char *self_dir(char *out, size_t cap)
{
    char *slash;
    if (!GetModuleFileNameA(NULL, out, (DWORD)cap)) { out[0] = 0; return out; }
    slash = strrchr(out, '\\');
    if (slash) slash[1] = '\0';
    return out;
}
#else
#include <sys/time.h>
#include <unistd.h>
static unsigned long long wall_ms(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ull + tv.tv_usec / 1000ull;
}
static void sleep_ms(int ms) { usleep((unsigned)ms * 1000u); }
static const char *self_dir(char *out, size_t cap)
{
    (void)cap;
    strcpy(out, "");
    return out;
}
#endif

typedef struct {
    const char *name;
    const char *payload;   /* "ms code marker" */
    int  max_retries;
    int  failed_events;
    int  terminal;         /* finished or dead-lettered */
    gptps_status terminal_status;
    unsigned long long last_failed_wall;
    unsigned long long prev_failed_wall;
    int tail_seen;
    int tail_has_evidence;
} item;

static item items[] = {
    { "good",    "50 0 good1",     0, 0, 0, 0, 0, 0, 0, 0 },
    { "bad",     "50 2 bad1",      2, 0, 0, 0, 0, 0, 0, 0 },
    { "licence", "50 3 lic1",      1, 0, 0, 0, 0, 0, 0, 0 },
    { "runtime", "50 1 run1",      2, 0, 0, 0, 0, 0, 0, 0 },
};
#define NITEMS (sizeof items / sizeof items[0])

static gptps_handle g_handles[NITEMS];

static item *item_by_handle(gptps_handle h)
{
    size_t i;
    for (i = 0; i < NITEMS; ++i)
        if (g_handles[i] == h) return &items[i];
    return NULL;
}

static void note_tail(const char *tail, size_t len, item *it)
{
    if (!tail || !len) return;
    it->tail_seen = 1;
    if (len && strstr(tail, "TAIL-EVIDENCE")) it->tail_has_evidence = 1;
}

static void observe(const gptps_event *ev, void *ud)
{
    item *it = item_by_handle(ev->handle);
    (void)ud;
    if (!it) return;
    if (ev->kind == GPTPS_EV_FAILED) {
        ++it->failed_events;
        it->prev_failed_wall = it->last_failed_wall;
        it->last_failed_wall = wall_ms();
        if (ev->struct_size >= sizeof(gptps_event))
            note_tail(ev->stderr_tail, ev->stderr_tail_len, it);
    } else if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DEAD_LETTERED) {
        it->terminal = 1;
        it->terminal_status = ev->status;
        if (ev->struct_size >= sizeof(gptps_event))
            note_tail(ev->stderr_tail, ev->stderr_tail_len, it);
    }
}

static void inspect_dead_letter(const gptps_dead_letter *dl, void *ud)
{
    item *it = item_by_handle(dl->handle);
    int *count = (int *)ud;
    ++*count;
    if (!it) return;
    printf("dead letter: %-8s attempts=%u status=%d tail=%zu bytes evidence=%s\n",
           it->name, dl->attempts, (int)dl->status, dl->stderr_tail_len,
           (dl->stderr_tail && strstr(dl->stderr_tail, "TAIL-EVIDENCE")) ? "yes" : "no");
    if (dl->stderr_tail) { it->tail_seen = 1; it->tail_has_evidence = 1; }
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    gptps_retry *r = NULL;
    char prog[1024];
    const char *argv[3];
    size_t i;
    int drained = 0, all_terminal = 0, step;

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
    REQUIRE(gptps_retry_install(e, &r) == GPTPS_OK);
    /* the vendor's convention from issue #12, as rules */
    REQUIRE(gptps_retry_on_exit(r, NULL, 2, GPTPS_RETRY_NEVER) == GPTPS_OK);
    REQUIRE(gptps_retry_on_exit(r, NULL, 3, 300) == GPTPS_OK);

    self_dir(prog, sizeof prog);
#ifdef _WIN32
    strncat(prog, "erp_job.exe", sizeof prog - strlen(prog) - 1);
#else
    strncat(prog, "erp_job", sizeof prog - strlen(prog) - 1);
#endif
    argv[0] = prog; argv[1] = NULL;

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = "erp_job";
    d.exec = GPTPS_EXEC_PROGRAM;
    d.argv = argv;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 64u << 20;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 0;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    d.stderr_tail_bytes = 512;
    REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);

    for (i = 0; i < NITEMS; ++i) {
        gptps_submit_options o;
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.flags = GPTPS_SUBMIT_POLICY;
        o.policy.struct_size = sizeof o.policy;
        o.policy.max_retries = (uint32_t)items[i].max_retries;
        o.policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
        REQUIRE(gptps_submit_ex(e, "erp_job", items[i].payload,
                                strlen(items[i].payload), &o, &g_handles[i]) == GPTPS_OK);
    }

    /* NIGHT: pump until every item is terminal (children finish async) */
    for (step = 0; step < 20000 && !all_terminal; ++step) {
        size_t ran = 0;
        gptps_status st;
        st = gptps_step(e, &ran);
        REQUIRE(st == GPTPS_OK);
        all_terminal = 1;
        for (i = 0; i < NITEMS; ++i) if (!items[i].terminal) all_terminal = 0;
        if (!ran) sleep_ms(2);
    }
    REQUIRE(all_terminal);
    /* dead letters must be drained BEFORE gptps_shutdown: teardown frees the
     * engine (engine.c:6767 destroys e->m and 6768 frees e), so a drain after
     * it is a use-after-free - the pass-one harness never drained, which is
     * why only this pass saw the hang. */
    REQUIRE(gptps_dead_letter_drain(e, inspect_dead_letter, &drained) == 3);
    REQUIRE(gptps_shutdown(e) == GPTPS_OK);
    e = NULL;

    /* MORNING: the report */
    printf("GPTPS pilot pass two - v1.10.0, gptps_retry + stderr tails, PROGRAM jobs\n");
    for (i = 0; i < NITEMS; ++i)
        printf("%-8s failed_events=%d terminal=%d status=%d tail=%d evidence=%d\n",
               items[i].name, items[i].failed_events, items[i].terminal,
               (int)items[i].terminal_status, items[i].tail_seen,
               items[i].tail_has_evidence);

    /* findings */
    {
        long long lic_gap = (long long)(items[2].last_failed_wall - items[2].prev_failed_wall);
        int t;
        printf("licence attempt gap: %lld ms (rule: >= 300)\n", lic_gap);

        t = items[1].terminal && items[1].terminal_status == GPTPS_E_DENIED &&
            items[1].failed_events == 1;
        printf("exit 2 dead-lettered at once, E_DENIED, 1 attempt: %s\n", t ? "HOLDS" : "FAILS");
        REQUIRE(t);

        t = items[2].failed_events == 2 && lic_gap >= 300;
        printf("exit 3 waited its extra %d ms before the retry: %s\n", 300, t ? "HOLDS" : "FAILS");
        REQUIRE(t);

        t = items[3].failed_events == 3;
        printf("exit 1 retried per policy (3 attempts total): %s\n", t ? "HOLDS" : "FAILS");
        REQUIRE(t);

        t = items[1].tail_has_evidence && items[2].tail_has_evidence &&
            items[3].tail_has_evidence;
        printf("every dead letter carries its stderr TAIL-EVIDENCE: %s\n", t ? "HOLDS" : "FAILS");
        REQUIRE(t);

        t = items[0].terminal && items[0].terminal_status == GPTPS_OK &&
            items[0].failed_events == 0;
        printf("the good job finished untouched: %s\n", t ? "HOLDS" : "FAILS");
        REQUIRE(t);
    }

    gptps_retry_close(r);
    printf("drained dead letters: %d (expect 3)\n", drained);
    return 0;
}
