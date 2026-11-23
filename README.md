# GPTPS night-window harnesses

Measurement harnesses for the nightly ERP batch-window contract discussed in
[Fikoko/GPTPS issue #12](https://github.com/Fikoko/GPTPS/issues/12). Every
harness drives real PROGRAM child processes (or, for pass one, real scheduled
work) through the **public GPTPS API only** — no engine internals touched.
Written for Windows, MinGW gcc; nothing Windows-specific in the API usage
except the cancel-thread pattern noted below.

They are offered as regression-test material for the night-window contract:
each harness ends with explicit HOLDS/FAILS lines and a nonzero exit code on
any failure, so they can run unattended.

## Passes

| file | measured against | what it proves |
|---|---|---|
| `pilot_bench_190.c` | v1.9.0 | EDF start order follows deadlines, not submit order; `shed_late` refuses an already-late job at admission (GPTPS_E_DENIED); dead-letter accounting balances |
| `pilot_bench_110.c` + `erp_job.c` | v1.10.0, re-verified v1.13.0 | gptps_retry rules: exit 2 -> NEVER (dead-letters at once, retry budget untouched), exit 3 -> delayed retry (469/485 ms measured against a 300 ms rule), exit 1 -> per-policy retries; every dead letter carries the LAST `stderr_tail_bytes` of the child's stderr |
| `pilot_bench_113_extra.c` + `erp_stop_job.c` + `erp_redrive_job.c` | v1.13.0 | `stop_grace_ms`: cancel mid-run, child catches Ctrl+Break, rolls back, engine reports GPTPS_E_CANCELLED 172 ms after cancel (grace 2000 ms), tail carries ROLLBACK-EVIDENCE; dead-letter re-drive after a licence renewal finishes GPTPS_OK |

Sample outputs from actual runs are in `outputs/`.

## Build & run

Place the harness files at the root of a GPTPS source checkout (the tag you
want to measure), then, with MinGW gcc on PATH:

```bat
rem pass one (scheduler add-ons)
gcc -O2 -Wall -Iinclude -Iaddons src\*.c addons\gptps_sched.c pilot_bench_190.c -o pilot_190.exe
pilot_190.exe

rem pass two (retry add-on + stderr tails)
gcc -O2 -Wall -Iinclude -Iaddons src\*.c addons\gptps_retry.c pilot_bench_110.c -o pilot_110.exe
gcc -O2 erp_job.c -o erp_job.exe
pilot_110.exe

rem pass three (stop_grace_ms + re-drive; no add-ons)
gcc -O2 -Wall -Iinclude -Iaddons src\*.c pilot_bench_113_extra.c -o pilot_113_extra.exe
gcc -O2 erp_stop_job.c -o erp_stop_job.exe
gcc -O2 erp_redrive_job.c -o erp_redrive_job.exe
pilot_113_extra.exe
```

(The scheduler add-ons EDF and shed_late both live in `gptps_sched.c`.) The
helper programs must sit next to the harness executable; the harness finds
them via its own path.

## Contract notes these harnesses rely on

- **Drain before shutdown.** `gptps_dead_letter_drain` must be called before
  `gptps_shutdown`; teardown frees the engine. Doing it in the wrong order is
  a use-after-free (we hit it; it is harness code's job to get this right).
- **MANUAL + cancel.** `gptps_step` runs an admitted child to completion on
  the pumping thread, so a cancel that should interrupt a running PROGRAM job
  has to come from another host thread (`gptps_cancel` is safe from any
  thread). A same-thread cancel after completion is a harmless E_NOTFOUND.
- **A cancelled item does not dead-letter** (by design). The morning drain
  sees real failures only.
- `stderr_tail_bytes` evidence arrives on FAILED / dead-letter, not on
  FINISHED — a successful job needs no evidence.

## Licence

MIT, (c) nightops00dev. Use, copy and fold into your own test suite freely.
