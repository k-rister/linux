# Phase 8 worst-case latency report (2026-10-29)

Scope: the Phase 8 matrix row "No ordinary task or unbounded path;
latency". This report adds the percentile distribution and the
per-stress-class event correlation that the row lists as open. It is a
measured envelope on one bounded test system, not a proof of absence of
every ordinary Linux or unbounded kernel path.

## Method

### Lateness histogram (ABI 18)

`struct cpu_accel_shared` gained a 64-bin log2 lateness histogram
(`lateness_hist[]`, `CPU_ACCEL_LATENESS_HIST_BINS`). Bin 0 counts samples
with exactly 0 ns lateness; bin `i >= 1` counts samples with lateness in
`[2^(i-1), 2^i)` ns. Both owner-side writer paths fill it per sample: the
kernel workload loop (`cpu_accel_run()`) and the user image
(`cpu_accel_user_oslat()`). `cpu-accelctl` prints the non-zero bins and the
upper edge of the bin containing each of the 50th, 90th, 99th, and 99.9th
permille ranks (`hist_p50_ns`, `hist_p90_ns`, `hist_p99_ns`,
`hist_p999_ns`), plus `hist_total`, the histogram's own sample count.
Percentiles from a log2 histogram are upper-edge estimates, i.e. a
conservative overestimate within one factor of two.

Note that `samples` in the status line is `samples_valid`, capped at
`CPU_ACCEL_MAX_SAMPLES` (2048): the user-exit path publishes
`samples_produced = samples_valid` (drivers/cpu_accel/cpu_accel.c). For a
run longer than 2.048 s at a 1 ms period, `hist_total` is the true sample
count.

### Stress-class matrix

`tools/cpu_accel/test-wcl-report.sh` runs one bounded stress class at a
time and records, per class, the final terminal status line and the
lateness histogram. For the duration of the matrix it selects `never` in
transparent hugepage's `enabled` control (restoring the previous mode in
cleanup) because `khugepaged`'s `lru_add_drain_all()` stops every owned
CPU on its scan period and would otherwise end a run before the window
completes.

Classes run on 2026-10-29:

- `baseline`: 5 s `user-oslat` owner on CPU 2, no added stress.
- `load`: same, plus one busy loop pinned to each of CPUs 3, 4, 5, 6, 7.
- `vmstat`, `lru`, `slub_shrink`, `ring_resize`:
  `test-sync-work-owner-stop.sh` cases (bounded `user-hang` owner stopped
  by the respective synchronous work path).
- `call_function`, `irq_work`: `test-call-function-single-owner-stop.sh`
  (the irq-work mode uses a CPU-cycles PMU event on the target CPU).
- `timerlat`: `test-timerlat-owner.sh` (osnoise/timerlat baseline and
  owner interval on CPU 2).

Deferred classes, with the blocking requirement:

- `net_backlog`, `timer_migration`: their owner CPU (1) must be in
  `rcu_nocbs`; user entry returns `-EOPNOTSUPP` when the CPU is not RCU
  offloaded (arch/x86/kernel/accel.c, `x86_cpu_accel_user_enter`). This VM
  is booted with `rcu_nocbs=2`.
- `irq_storm`: requires a test NIC via `CPU_ACCEL_IRQ_INTERFACE`.

## Results

Environment: `7.3.0-rc3-accel-tlbfix-02150-gde9255a04ef7`,
`isolcpus=managed_irq,2 irqaffinity=0-1,3-7 nohz_full=2 rcu_nocbs=2`,
owner CPU 2, 1 ms sampling period. No kernel-error match (BUG/Oops/soft
lockup/panic) in any run; THP mode and the module were restored after the
matrix.

### Lateness distributions (cooperative user-oslat owner, 5 s)

baseline (5000 samples):

| range_ns  | [0,1) | [1,2) | [2,4) | [4,8) | [8,16) | [16,32) | [64,128) | [128,256) | [256,512) | [512,1k) | [1k,2k) | [2k,4k) | [4k,8k) | [8k,16k) |
|-----------|-------|-------|-------|-------|--------|---------|----------|-----------|-----------|----------|---------|---------|---------|----------|
| count     | 359   | 355   | 697   | 1361  | 2153   | 19      | 1        | 1         | 3         | 9        | 11      | 18      | 5       | 8        |

p50=8 ns, p90=16 ns, p99=1024 ns, p999=16384 ns, max=15137 ns.
`irq_count=0 softirq_count=0 workqueue_queued=1 context_switches=0`.

load (5000 samples, five busy housekeeping CPUs):

| range_ns  | [0,1) | [1,2) | [2,4) | [4,8) | [8,16) | [16,32) | [32,64) | [128,256) | [256,512) | [512,1k) | [1k,2k) | [2k,4k) | [4k,8k) | [8k,16k) |
|-----------|-------|-------|-------|-------|--------|---------|---------|-----------|-----------|----------|---------|---------|---------|----------|
| count     | 377   | 349   | 676   | 1393  | 2118   | 17      | 3       | 2         | 2         | 6        | 15      | 16      | 11      | 15       |

p50=8 ns, p90=16 ns, p99=2048 ns, p999=16384 ns, max=16350 ns.
`irq_count=0 softirq_count=0 workqueue_queued=1 context_switches=0`.

Reading: the body of the distribution (p50/p90) is the polling granularity
of the 1 ms period check, and 5 busy housekeeping CPUs did not move it.
The tail above 16 ns is sparse housekeeping activity landing on the
isolated CPU (a handful of samples per bin per run); both runs carry a
similar tail, and neither run shows a monotonic pile-up that would suggest
an accumulating unbounded path in this window. An earlier 5 s baseline
run in the same session reached max=20385 ns and a 1 s sanity run reached
max=92097 ns, so the single-run max in this window (16.3 us) is not the
session worst case; the observed session envelope for the cooperative
owner was on the order of tens of microseconds.

### Stress-class counter correlation (bounded runs, terminal state)

| class         | terminal state | workqueue_queued | workqueue_executed | arch_ipi_count | irq_count | context_switches |
|---------------|----------------|------------------|--------------------|----------------|-----------|------------------|
| vmstat        | 7 (ESCAPED)    | 1                | 0                  | 2              | 0         | 0                |
| lru           | 7 (ESCAPED)    | 1                | 0                  | 2              | 0         | 0                |
| slub_shrink   | 7 (ESCAPED)    | 2                | 0                  | 2              | 0         | 0                |
| ring_resize   | 7 (ESCAPED)    | 1                | 0                  | 2              | 0         | 0                |
| call_function | 7 (ESCAPED)    | 0                | 0                  | 1              | 0         | 0                |
| irq_work      | 7 (ESCAPED)    | 1                | 0                  | 3              | 0         | 0                |

The noncooperative `user-hang` owner produces no lateness samples by
design, so the WCL evidence for these classes is the counter correlation
plus the stop/exit trace pair: in every class the `owner_stop_request`
trace on the housekeeping side is followed by `owner_exit_complete` on
CPU 2 within a few trace lines (microsecond-scale function-graph
intervals, e.g. the lru case showed a ~10.5 us `arch_smp_sync_wait_stop()`
-> `owner_stop_request` -> `owner_exit_complete` span). The queued work
was deferred (workqueue_executed=0 on the owner CPU) or ran on
housekeeping CPUs, and no IRQ was delivered to the owned CPU. The
`arch_ipi_count` of 2 in the sync-work classes is the stop request plus
its accompanying bookkeeping IPI; `irq_work` adds the local-IRQ-work NMI.

### timerlat

Baseline on CPU 2 (no owner, 503 samples): irq thread p50=2307 ns,
p95=6210 ns, p99=7874 ns, max=71528 ns; timerlat thread p50=5680 ns,
p95=10067 ns, p99=13000 ns, max=80167 ns.

During the owner interval the timer IRQ that fires on CPU 2 cannot run
until the owner stops: `irq_max_active_ns=253038607` (253 ms) and
`irq_max_gap_ns=254039000`. That is the expected owner semantics, not a
timer defect: while a CPU is exclusively owned, timer work for it is
deferred to housekeeping CPUs or to the owner stop, and the observed
bound for a missed timer on the owned CPU is the owner's stop latency,
which the stop paths above bound at microsecond scale once the stop
request is issued.

## Known limitations and open items

- No hard bound is claimed. The measured maxima are envelopes for the
  listed bounded classes on one VM image; the row's disposition remains
  "selected invariants measured; no hard bound".
- Percentiles are log2-bin upper-edge estimates (factor-of-two ceiling).
- The `test-sync-work-owner-stop.sh` function-graph assertion "trace did
  not pair owner stop with owner exit" fails on this image for the
  `lru`/`slub_shrink` (and observed) cases even with the pre-change
  module and ABI-17 controller (A/B verified on 2026-10-29): the runs
  themselves complete with the stop/exit pair present in the trace, so
  the WCL data above stands, but the assertion's `=> caller` matching
  needs separate investigation.
- The `test-cpu-accel.sh` CPU-hotplug-gate case fails on this image:
  offlining the owned CPU terminates the `user-oslat` owner through the
  stop path's escape NMI (`state=7`, `user_escape_count=1`, active
  interval ~192 ms) while the test expects a graceful drain to
  `state=3`. A/B verified with the pre-change module and ABI-17
  controller (three consecutive identical failures), so it is not an
  ABI-18 regression. It is the owner-stop-vs-drain design question of
  the "CPU hotplug attempts" matrix row: a cooperative user owner could
  drain within one polling period from a stop-only request, while the
  current `cpu_accel_arch_stop_user()` also raises the escape NMI.
- `net_backlog` and `timer_migration` are deferred to a boot with
  `rcu_nocbs` covering their owner CPU; `irq_storm` is deferred to a test
  NIC.
- Machine-check injection remains out of scope (see the exceptions/NMI
  row).

## Reproduction

```
make -C tools/cpu_accel cpu-accelctl test-sync-wait-prime
env CPU_ACCELCTL=... CPU_ACCEL_SYNC_WAIT_PRIME=... \
    CPU_ACCEL_SINGLE_PROBE=.../cpu_accel_call_function_single_probe.ko \
    sh tools/cpu_accel/test-wcl-report.sh
```

Per-class logs and the summary TSV land in `$CPU_ACCEL_TRACE_DIR`
(default: a fresh `/tmp/cpu-accel-wcl.XXXXXX`).

— AI-signed: opencode | model: radiance/qwen3.8-27b | effort: not specified
