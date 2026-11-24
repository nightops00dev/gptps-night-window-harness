/* pilot_bench_113_extra.c - pilot pass three against GPTPS v1.13.0.
 *
 * Two scenarios that complete the 1.10 story the night window cares about:
 *
 *   A. stop_grace_ms rollback: a long-running PROGRAM job is cancelled
 *      mid-run; the child gets Ctrl+Break, rolls back (ROLLBACK-EVIDENCE in
 *      the stderr tail), and the engine reports GPTPS_E_CANCELLED. We measure
 *      the wall time from gptps_cancel to the terminal event and check it
 *      stays inside the grace window (2000 ms).
 *
 *   B. dead-letter re-drive: a licence job exits 3 and dead-letters at once
 *      (0 retries). Morning: the operator "renews the licence" (drops a
 *      marker file), the dead letter is drained and re-driven unchanged, and
 *      the second run finishes GPTPS_OK. Night-to-morning cycle, closed.
 *
 * MANUAL mode, concurrency 1. Dead letters drained BEFORE gptps_shutdown
 * (the ordering trap pass two documented). Not part of the repo.
 *
 * Review fixes (Fikoko, tests/pilot adaptation): tails are length-delimited
 * and searched bounded; every wait has a wall-clock bound; the licence marker
 * lives next to the harness exe, not in the current directory, so parallel
 * runs do not share it.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

/* stderr tails are length-delimited, NOT NUL-terminated: search bounded */
static int tail_contains(const char *tail, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    if (!tail || len < n) return 0;
    for (i = 0; i + n <= len; ++i)
        if (memcmp(tail + i, needle, n) == 0) return 1;
    return 0;
}

/* ---- shared observation state ------------------------------------------ */

typedef struct {
    const char *name;
    int started, finished, failed, dead;
    gptps_status terminal_status;
    unsigned long long terminal_wall;
    int tail_has_rollback;
} obs;

static obs g_long = { "long", 0, 0, 0, 0, 0, 0, 0 };
static obs g_redrive = { "redrive", 0, 0, 0, 0, 0, 0, 0 };

static gptps_handle h_long, h_redrive;

/* MANUAL runs a task to completion on the pumping thread, so the cancel has
 * to come from a second host thread (gptps_cancel is safe from any thread). */
static unsigned long long t_cancel;
static gptps_status cancel_st = GPTPS_E_INVAL;
static DWORD WINAPI cancel_after_delay(void *arg)
{
    gptps *e = (gptps *)arg;
    Sleep(600);                          /* let the job work a few ticks */
    t_cancel = wall_ms();
    cancel_st = gptps_cancel(e, h_long);
    return 0;
}

static obs *obs_by_handle(gptps_handle h)
{
    if (h == h_long) return &g_long;
    if (h == h_redrive) return &g_redrive;
    return NULL;
}

static void observe(const gptps_event *ev, void *ud)
{
    obs *o = obs_by_handle(ev->handle);
    (void)ud;
    if (!o) return;
    switch (ev->kind) {
    case GPTPS_EV_STARTED: o->started = 1; break;
    case GPTPS_EV_FINISHED:
        o->finished = 1; o->terminal_status = ev->status;
        o->terminal_wall = wall_ms(); break;
    case GPTPS_EV_FAILED:
        ++o->failed; o->terminal_status = ev->status;
        o->terminal_wall = wall_ms();
        if (ev->struct_size >= sizeof(gptps_event) &&
            tail_contains(ev->stderr_tail, ev->stderr_tail_len,
                          "ROLLBACK-EVIDENCE"))
            o->tail_has_rollback = 1;
        break;
    case GPTPS_EV_DEAD_LETTERED: o->dead = 1; break;
    default: break;
    }
}

static int drained = 0;
static void count_dl(const gptps_dead_letter *dl, void *ud)
{
    (void)ud;
    ++drained;
    printf("dead letter: task=%s attempts=%u status=%d tail=%zu bytes\n",
           dl->task_name, dl->attempts, (int)dl->status, dl->stderr_tail_len);
}

/* ---- helpers ------------------------------------------------------------ */

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

static void pump(gptps *e, int steps)
{
    int i;
    for (i = 0; i < steps; ++i) {
        size_t ran = 0;
        if (gptps_step(e, &ran) != GPTPS_OK) return;
        if (!ran) sleep_ms(2);
    }
}

static gptps_status submit_prog(gptps *e, const char *task, gptps_handle *out)
{
    gptps_submit_options o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.flags = GPTPS_SUBMIT_POLICY;
    o.policy.struct_size = sizeof o.policy;
    o.policy.max_retries = 0;
    o.policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    return gptps_submit_ex(e, task, NULL, 0, &o, out);
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    char dir[1024], prog_long[1200], prog_redrive[1200], marker[1200];
    const char *argv_long[2], *argv_redrive[2];
    unsigned long long stop_elapsed;
    FILE *f;
    int t;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 4096ull << 20;
    REQUIRE(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    REQUIRE(gptps_register_observer(e, observe, NULL) == GPTPS_OK);

    self_dir(dir, sizeof dir);
    snprintf(prog_long, sizeof prog_long, "%serp_stop_job.exe", dir);
    snprintf(prog_redrive, sizeof prog_redrive, "%serp_redrive_job.exe", dir);
    /* the marker lives next to the harness, so parallel runs each get their
     * own - a shared cwd file would make one run "renew" another's licence */
    snprintf(marker, sizeof marker, "%slicence.renewed", dir);
    argv_long[0] = prog_long; argv_long[1] = NULL;
    argv_redrive[0] = prog_redrive; argv_redrive[1] = NULL;

    /* scenario B first (fast), then A */
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = "erp_redrive";
    d.exec = GPTPS_EXEC_PROGRAM;
    d.argv = argv_redrive;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 64u << 20;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 0;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    d.stderr_tail_bytes = 512;
    REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);

    d.name = "erp_long";
    d.argv = argv_long;
    d.stop_grace_ms = 2000;
    REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);

    /* --- B: night - licence missing, dead-lettered at once --- */
    remove(marker);
    REQUIRE(submit_prog(e, "erp_redrive", &h_redrive) == GPTPS_OK);
    {
        unsigned long long deadline = wall_ms() + 15000;
        while (!g_redrive.dead) {
            pump(e, 50);
            REQUIRE(wall_ms() < deadline);   /* bounded wait */
        }
    }
    printf("B night: redrive failed=%d dead=%d status=%d\n",
           g_redrive.failed, g_redrive.dead, (int)g_redrive.terminal_status);
    REQUIRE(g_redrive.failed == 1 && g_redrive.dead);

    /* --- B: morning - licence renewed, drain + re-drive unchanged --- */
    f = fopen(marker, "wb");
    REQUIRE(f != NULL);
    fputs("renewed by the morning operator\n", f);
    fclose(f);
    REQUIRE(gptps_dead_letter_drain(e, count_dl, NULL) == 1);

    g_redrive.finished = 0; g_redrive.terminal_status = 0;
    REQUIRE(submit_prog(e, "erp_redrive", &h_redrive) == GPTPS_OK);
    {
        unsigned long long deadline = wall_ms() + 15000;
        while (!g_redrive.finished) {
            pump(e, 50);
            REQUIRE(wall_ms() < deadline);   /* bounded wait */
        }
    }
    printf("B morning: re-drive finished=%d status=%d\n",
           g_redrive.finished, (int)g_redrive.terminal_status);
    t = g_redrive.finished && g_redrive.terminal_status == GPTPS_OK;
    printf("dead-letter re-drive after licence renewal: %s\n", t ? "HOLDS" : "FAILS");
    REQUIRE(t);
    remove(marker);

    /* --- A: cancel a long job mid-run, Ctrl+Break rollback --- */
    {
        HANDLE th;
        unsigned long long deadline;
        REQUIRE(submit_prog(e, "erp_long", &h_long) == GPTPS_OK);
        th = CreateThread(NULL, 0, cancel_after_delay, e, 0, NULL);
        REQUIRE(th != NULL);
        /* this pump blocks inside gptps_step while the child runs; the
         * cancel thread interrupts it with Ctrl+Break after 600 ms */
        deadline = wall_ms() + 30000;
        while (!g_long.failed && !g_long.finished) {
            pump(e, 50);
            REQUIRE(wall_ms() < deadline);   /* bounded wait */
        }
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
        REQUIRE(cancel_st == GPTPS_OK);
    }
    stop_elapsed = g_long.terminal_wall - t_cancel;
    printf("A: cancel -> terminal in %llu ms (grace 2000), status=%d (CANCELLED=%d)\n",
           stop_elapsed, (int)g_long.terminal_status, (int)GPTPS_E_CANCELLED);

    t = g_long.terminal_status == GPTPS_E_CANCELLED;
    printf("cancelled job reported GPTPS_E_CANCELLED: %s\n", t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    t = g_long.tail_has_rollback;
    printf("stderr tail carries ROLLBACK-EVIDENCE: %s\n", t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    t = stop_elapsed >= 50 && stop_elapsed <= 2500;
    printf("stop took %llu ms - rollback happened, inside grace: %s\n",
           stop_elapsed, t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    /* drain (before shutdown!) and close */
    gptps_dead_letter_drain(e, count_dl, NULL);
    printf("total drained dead letters: %d\n", drained);
    REQUIRE(gptps_shutdown(e) == GPTPS_OK);
    printf("PASS THREE COMPLETE\n");
    return 0;
}
