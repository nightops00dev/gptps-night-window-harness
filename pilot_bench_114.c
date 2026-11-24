/* pilot_bench_114.c - pilot pass four against GPTPS v1.14.0 (ABI 2.11).
 *
 * Measures the two additions our issue #12 notes produced:
 *
 *   1. gptps_dead_letter.attempts_run - the attempts that RAN, next to the
 *      scheduled count. Expected on the pass-two scenario:
 *        bad     (exit 2, NEVER)  attempts=2, attempts_run=1
 *        licence (exit 3, +300ms) attempts=2, attempts_run=2
 *        runtime (exit 1, 2 retries) attempts=3, attempts_run=3
 *
 *   2. gptps_shutdown_ex's dead_letter callback - a shutdown hands the host
 *      every dead letter it still holds, INCLUDING the ones the shutdown
 *      itself makes for work it cuts off (status GPTPS_E_SHUTDOWN). We leave
 *      the three pass-two dead letters UNDRAINED, queue two more items and
 *      never step them, then shut down: the callback must deliver 5 items,
 *      oldest first, tails included, before the engine is freed.
 *
 * MANUAL, concurrency 1, PROGRAM children. Not part of the repo.
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
#endif

typedef struct {
    const char *name;
    const char *payload;
    int max_retries;
    int terminal;
} item;

static item items[] = {
    { "good",    "50 0 good1", 0, 0 },
    { "bad",     "50 2 bad1",  2, 0 },
    { "licence", "50 3 lic1",  1, 0 },
    { "runtime", "50 1 run1",  2, 0 },
};
#define NITEMS 4

static gptps_handle g_handles[NITEMS];

static void observe(const gptps_event *ev, void *ud)
{
    size_t i;
    (void)ud;
    for (i = 0; i < NITEMS; ++i)
        if (g_handles[i] == ev->handle &&
            (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DEAD_LETTERED))
            items[i].terminal = 1;
}

/* what the shutdown callback hands over, copied out (valid only for the call) */
#define MAXDL 8
typedef struct {
    char marker[32];
    gptps_status status;
    uint32_t attempts, attempts_run;
    int tail_evidence;
} dlrec;
static dlrec g_dl[MAXDL];
static int g_ndl = 0;

/* stderr tails are length-delimited, NOT NUL-terminated: search bounded */
static int tail_contains(const char *tail, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    if (!tail || len < n) return 0;
    for (i = 0; i + n <= len; ++i)
        if (memcmp(tail + i, needle, n) == 0) return 1;
    return 0;
}

static void on_dl(const gptps_dead_letter *dl, void *ud)
{
    dlrec *r;
    char buf[128];
    size_t n;
    const char *m;
    (void)ud;
    if (g_ndl >= MAXDL) return;
    r = &g_dl[g_ndl++];
    memset(r, 0, sizeof *r);
    /* payload is length-delimited, not necessarily NUL-terminated */
    n = dl->payload_len < sizeof buf - 1 ? dl->payload_len : sizeof buf - 1;
    if (dl->payload && n) memcpy(buf, dl->payload, n);
    buf[n] = 0;
    /* payload is "ms code marker"; the marker names the item */
    m = strrchr(buf, ' ');
    snprintf(r->marker, sizeof r->marker, "%s", m ? m + 1 : "?");
    r->status = dl->status;
    r->attempts = dl->attempts;
    r->attempts_run = dl->attempts_run;
    r->tail_evidence = tail_contains(dl->stderr_tail, dl->stderr_tail_len,
                                     "TAIL-EVIDENCE");
}

static dlrec *find(const char *marker)
{
    int i;
    for (i = 0; i < g_ndl; ++i)
        if (strcmp(g_dl[i].marker, marker) == 0) return &g_dl[i];
    return NULL;
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    gptps_retry *r = NULL;
    gptps_shutdown_options so;
    char prog[1024];
    const char *argv[2];
    size_t i;
    int all_terminal = 0, step, t;
    dlrec *x;

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
    REQUIRE(gptps_retry_on_exit(r, NULL, 2, GPTPS_RETRY_NEVER) == GPTPS_OK);
    REQUIRE(gptps_retry_on_exit(r, NULL, 3, 300) == GPTPS_OK);

    self_dir(prog, sizeof prog);
    strncat(prog, "erp_job.exe", sizeof prog - strlen(prog) - 1);
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

    for (step = 0; step < 20000 && !all_terminal; ++step) {
        size_t ran = 0;
        REQUIRE(gptps_step(e, &ran) == GPTPS_OK);
        all_terminal = 1;
        for (i = 0; i < NITEMS; ++i) if (!items[i].terminal) all_terminal = 0;
        if (!ran) sleep_ms(2);
    }
    REQUIRE(all_terminal);

    /* two more items, queued and NEVER stepped: the shutdown must cut them
     * off and hand them over as GPTPS_E_SHUTDOWN dead letters */
    {
        gptps_handle h1, h2;
        gptps_submit_options o;
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.flags = GPTPS_SUBMIT_POLICY;
        o.policy.struct_size = sizeof o.policy;
        o.policy.max_retries = 0;
        o.policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
        REQUIRE(gptps_submit_ex(e, "erp_job", "50 0 queued1", strlen("50 0 queued1"), &o, &h1) == GPTPS_OK);
        REQUIRE(gptps_submit_ex(e, "erp_job", "50 0 queued2", strlen("50 0 queued2"), &o, &h2) == GPTPS_OK);
    }

    /* the 1.14 shutdown: every retained dead letter + the ones it makes now */
    memset(&so, 0, sizeof so);
    so.struct_size = sizeof so;
    so.dead_letter = on_dl;
    REQUIRE(gptps_shutdown_ex(e, &so) == GPTPS_OK);

    printf("GPTPS pilot pass four - v1.14.0, shutdown_ex + attempts_run\n");
    printf("shutdown handed over %d dead letters (expect 5)\n", g_ndl);
    for (step = 0; step < g_ndl; ++step)
        printf("  %-8s status=%d attempts=%u attempts_run=%u tail=%s\n",
               g_dl[step].marker, (int)g_dl[step].status,
               g_dl[step].attempts, g_dl[step].attempts_run,
               g_dl[step].tail_evidence ? "yes" : "no");

    t = g_ndl == 5;
    printf("5 dead letters handed over before the engine was freed: %s\n",
           t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    x = find("bad1");
    t = x && x->attempts == 2 && x->attempts_run == 1;
    printf("exit 2: attempts=2 but attempts_run=1 (NEVER refused #2): %s\n",
           t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    x = find("lic1");
    t = x && x->attempts == 2 && x->attempts_run == 2;
    printf("exit 3: attempts=2, attempts_run=2 (both ran): %s\n",
           t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    x = find("run1");
    t = x && x->attempts == 3 && x->attempts_run == 3;
    printf("exit 1: attempts=3, attempts_run=3 (all ran): %s\n",
           t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    x = find("queued1");
    t = x && x->status == GPTPS_E_SHUTDOWN && x->attempts_run == 0;
    printf("queued item cut off by shutdown: E_SHUTDOWN, 0 runs: %s\n",
           t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    t = find("bad1")->tail_evidence && find("lic1")->tail_evidence &&
        find("run1")->tail_evidence;
    printf("tails survive the shutdown handover: %s\n", t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    gptps_retry_close(r);
    printf("PASS FOUR COMPLETE\n");
    return 0;
}
