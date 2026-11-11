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
(`cpu_accel_user_oslat()`). `cpu-accel-testctl` prints the non-zero bins and the
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
- `memory_pressure`: a repeated 128 MiB tmpfs write/read loop pinned to
  the work CPU while the owner runs.

Optional classes, selected explicitly with `CPU_ACCEL_WCL_CLASSES`:

- `device_reload`, `device_teardown`, `device_rebind`, `device_sriov`:
  the four netdevsim device-management lifecycle cases, each run separately.
- `amd_pstate`, `thermal_cooling`, `cpuidle_state`, `dirty_writeback`:
  the four remaining power-policy and storage lifecycle cases. The dirty
  writeback class uses at least a 10 s owner window.
- `resctrl_owner`, `resctrl_task`, `resctrl_monitor`: the three resctrl
  fault-injection paths, each run separately.
- `perf_event`, `ftrace_maintenance`, `tlb_flush`: the perf, ftrace, and
  page-table/TLB source-audit rows.
- `hotplug_up`, `hotplug_down`, `stop_machine`, `stop_machine_fault`,
  `user_escape_fault`, `user_death`, `rcu_callback_routing`: the remaining
  hotplug, stop-machine, fault-injection, exception, and RCU stress rows.
- `migration`: two NUMA memory nodes, three distinct online CPUs, and a
  reader CPU supplied through `CPU_ACCEL_WCL_MIGRATION_READER_CPU`.
- `net_backlog`, `timer_migration`: the owner CPU must be RCU-offloaded and
  distinct from the trigger/work CPU. `timer_migration` also needs a cgroup-v2
  cpuset partition transition. `net_backlog` requires RFS, a veth device,
  network namespaces, Perl, and an RPS-eligible trigger/fallback CPU; the
  owner itself need not be RPS-eligible because a cork-held TCP flow preserves
  the RFS mapping after the sender migrates. `firewall-cmd` is required only
  when firewalld is active.
- `irq_storm`: requires a test NIC via `CPU_ACCEL_IRQ_INTERFACE`.

`test-wcl-report.sh` now defaults to the common portable set and accepts
`CPU_ACCEL_WCL_CLASSES=all` to attempt every class, including the optional
topology-specific rows.

### Class inventory

| Class | Harness |
|-------|---------|
| `baseline` | `test-wcl-report.sh` |
| `load` | `test-wcl-report.sh` |
| `memory_pressure` | `test-wcl-report.sh` |
| `vmstat` | `test-sync-work-owner-stop.sh` |
| `lru` | `test-sync-work-owner-stop.sh` |
| `slub_shrink` | `test-sync-work-owner-stop.sh` |
| `ring_resize` | `test-sync-work-owner-stop.sh` |
| `call_function` | `test-call-function-single-owner-stop.sh` |
| `irq_work` | `test-call-function-single-owner-stop.sh` |
| `timerlat` | `test-timerlat-owner.sh` |
| `device_reload` | `test-netdevsim-reload-owner.sh` |
| `device_teardown` | `test-netdevsim-teardown-owner.sh` |
| `device_rebind` | `test-netdevsim-rebind-owner.sh` |
| `device_sriov` | `test-netdevsim-sriov-owner.sh` |
| `amd_pstate` | `test-amd-pstate-shmem-owner.sh` |
| `thermal_cooling` | `test-thermal-cooling-owner.sh` |
| `cpuidle_state` | `test-cpuidle-state-owner.sh` |
| `dirty_writeback` | `test-dirty-writeback-owner.sh` |
| `resctrl_owner` | `test-resctrl-owner-stop.sh` |
| `resctrl_task` | `test-resctrl-task-owner-stop.sh` |
| `resctrl_monitor` | `test-resctrl-monitor-owner-stop.sh` |
| `perf_event` | `test-perf-owner-stop.sh` |
| `ftrace_maintenance` | `test-ftrace-maintenance.sh` |
| `tlb_flush` | `test-tlb-flush.sh` |
| `hotplug_up` | `test-hotplug-race.sh` |
| `hotplug_down` | `test-hotplug-race.sh` |
| `stop_machine` | `test-stop-machine-inactive-owner.sh` |
| `stop_machine_fault` | `test-stop-machine-owner-stop.sh` |
| `user_escape_fault` | `test-user-escape-fault.sh` |
| `user_death` | `test-user-death-owner.sh` |
| `rcu_callback_routing` | `test-rcu-callback-routing.sh` |
| `migration` | `test-migration-owner.sh` |
| `net_backlog` | `test-sync-work-owner-stop.sh` |
| `timer_migration` | `test-sync-work-owner-stop.sh` |
| `irq_storm` | `test-irq-storm.sh` |

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

### 2026-10-30 re-validation (build #80)

Environment: `7.3.0-rc3-accel-tlbfix-gb381f8577d1a-dirty`,
`isolcpus=managed_irq,2 irqaffinity=0-1,3-7 nohz_full=2 rcu_nocbs=2`,
owner CPU 2, 1 ms sampling period. THP temporarily set to `never`.

baseline (5000 samples):

| range_ns  | [0,1) | [1,2) | [2,4) | [4,8) | [8,16) | [16,32) | [32,64) | [128,256) | [256,512) | [512,1k) | [1k,2k) | [2k,4k) | [4k,8k) | [8k,16k) | [16k,32k) | [32k,64k) | [64k,128k) | [128k,256k) |
|-----------|-------|-------|-------|-------|--------|---------|---------|-----------|-----------|----------|---------|---------|---------|----------|-----------|-----------|------------|-------------|
| count     | 315   | 319   | 579   | 1277  | 2293   | 91      | 1       | 2         | 2         | 5        | 16      | 22      | 32      | 38       | 4         | 2         | 1          | 1           |

p50=16 ns, p90=16 ns, p99=8192 ns, p999=32768 ns, max=247595 ns.
`irq_count=0 softirq_count=0 workqueue_queued=1 context_switches=0`.

load (5000 samples, five busy housekeeping CPUs):

| range_ns  | [0,1) | [1,2) | [2,4) | [4,8) | [8,16) | [16,32) | [64,128) | [128,256) | [512,1k) | [1k,2k) | [2k,4k) | [4k,8k) | [8k,16k) | [16k,32k) | [64k,128k) |
|-----------|-------|-------|-------|-------|--------|---------|----------|-----------|----------|---------|---------|---------|----------|-----------|------------|
| count     | 327   | 320   | 618   | 1239  | 2294   | 130     | 1        | 2         | 3        | 4       | 15      | 19      | 22       | 4         | 2          |

p50=8 ns, p90=16 ns, p99=4096 ns, p999=32768 ns, max=81825 ns.
`irq_count=0 softirq_count=0 workqueue_queued=1 context_switches=0`.

The sync-work classes (vmstat, lru, slub_shrink, ring_resize) hit the
known pre-existing function-graph trace pairing assertion (see Known
limitations); their runs completed with the stop/exit trace pair present
and `state=7`, `arch_ipi_count=2`, `workqueue_queued=1`.
`call_function` passed (`state=7`, `arch_ipi_count=1`); `irq_work`
passed (`state=7`, `arch_ipi_count=2`); `timerlat` passed
(`state=7`, `arch_ipi_count=2`).

### 2026-11-09 memory_pressure (simon.lab)

Environment: `7.3.0-rc3-accel-tlbfix-gd16a5ac600cc-dirty`, ABI 22,
owner CPU 3, work CPU 0, 5 s `user-cyclictest`, 1 ms sampling period.
The class repeatedly wrote and read a 128 MiB tmpfs file from CPU 0
while the owner ran. Transparent hugepages were already in `never` mode.

memory_pressure (5003 samples):

| range_ns  | [0,1) | [1,2) | [2,4) | [4,8) | [8,16) | [16,32) |
|-----------|-------|-------|-------|-------|--------|---------|
| count     | 339   | 328   | 575   | 1205  | 1814   | 742     |

p50=16 ns, p90=32 ns, p99=32 ns, p999=32 ns, max=19 ns.
`state=3`, `irq_count=0`, `softirq_count=0`,
`workqueue_queued=0`, `arch_ipi_count=0`, `arch_tlb_count=0`,
`context_switches=0`, and `migration_detected=0`.

### 2026-11-10 common portable set (simon.lab)

Environment: `7.3.0-rc3-accel-tlbfix-gbff69f41a61b`, ABI 23, owner CPU 6,
work CPU 0, load CPUs 3/4/5/7, 5 s bounded runs, 1 ms sampling period.
Transparent hugepages were already in `never` mode. The first full attempt
failed only in the `lru` class because the harness treated a
`x86_cpu_accel_user_enter()` function-graph entry as proof that ownership was
complete; the LRU drain could therefore run before the architecture owner was
active, and the owner later exited at its timeout with `stop_requested=0`.
The harness now waits for the controller's active marker, which requires the
running accelerator mode and a non-zero `user_active_start_ns` with no end
timestamp. The isolated LRU case and a new full common-set run both pass.

Passing-run evidence:
`/root/accel-measure/phase8-bff69f4-wcl-common-20261110b/summary.tsv`
(SHA-256 `f3db69cb2fc5d96cf840a7e57245896604d560679eaddade5d1645f7a331ba0c`;
69 files, 215373 bytes). The failed diagnostic run is preserved in
`/root/accel-measure/phase8-bff69f4-wcl-common-20261110`. The existing serial
logger recorded a 638-byte delta for the passing run containing only the
expected `drop_caches` and call-function probe load/unload messages.

| class            | exit | state | samples | p50_ns | p90_ns | p99_ns | p999_ns | max_ns | workqueue_queued | arch_ipi_count | arch_nmi_count |
|------------------|------|-------|---------|--------|--------|--------|---------|--------|------------------|----------------|----------------|
| baseline         | 0    | 3     | 5003    | 16     | 32     | 32     | 32      | 19     | 0                | 0              | 0              |
| load             | 0    | 3     | 5003    | 16     | 32     | 32     | 32      | 19     | 1                | 0              | 0              |
| memory_pressure  | 0    | 3     | 5003    | 16     | 32     | 32     | 32      | 19     | 0                | 0              | 0              |
| vmstat           | 0    | 7     | 0       | 0      | 0      | 0      | 0       | 0      | 1                | 1              | 1              |
| lru              | 0    | 7     | 0       | 0      | 0      | 0      | 0       | 0      | 1                | 1              | 1              |
| slub_shrink      | 0    | 7     | 0       | 0      | 0      | 0      | 0       | 0      | 1                | 1              | 1              |
| ring_resize      | 0    | 7     | 0       | 0      | 0      | 0      | 0       | 0      | 1                | 1              | 1              |
| call_function    | 0    | 7     | 0       | 0      | 0      | 0      | 0       | 0      | 0                | 1              | 1              |
| irq_work         | 0    | 7     | 0       | 0      | 0      | 0      | 0       | 0      | 0                | 2              | 51             |
| timerlat         | 0    | 7     | 0       | 0      | 0      | 0      | 0       | 0      | 0                | 1              | 1              |

All rows reported zero IRQ, softirq, TLB, machine-check-exception,
machine-check-poll, and context-switch deltas. The report exited zero after
recording every class, so a class failure can no longer be hidden by the
final summary.

### 2026-11-10 optional and topology-specific set (simon.lab)

Environment: the same `7.3.0-rc3-accel-tlbfix-gbff69f41a61b` boot, ABI 23,
owner CPU 6, work CPU 0, and 5 s bounded runs. The topology-specific classes
were run individually because several need their own CPU or hardware mapping.
The passing evidence is under
`/root/accel-measure/phase8-bff69f4-wcl-optional-20261110/<class>/summary.tsv`
(123 files; combined summary SHA-256
`277e050e2f058440a877f1a56d2b51b992d0ad5f1b68ec9e05c460e34a9782d8`).
The isolated `timer_migration` rerun used the corrected active-marker harness
and is preserved in
`/root/accel-measure/phase8-bff69f4-wcl-timer-20261110T1310`
(summary SHA-256
`82c734b280fa794d1291c6df05002fd38563776ec63d4e50b6fe12c5e8713963`).

| class                | exit | state | samples    | p50_ns | p90_ns | p99_ns | p999_ns | max_ns | workqueue_queued | arch_ipi_count | arch_nmi_count |
|----------------------|------|-------|------------|--------|--------|--------|---------|--------|------------------|----------------|----------------|
| amd_pstate           | 0    | 3     | 250592606  | 32     | 32     | 32     | 32      | 240    | 1                | 0              | 0              |
| thermal_cooling      | 0    | 3     | 837008     | 32     | 32     | 32     | 32      | 280    | 0                | 0              | 1              |
| cpuidle_state        | 0    | 3     | 1114598    | 32     | 32     | 32     | 32      | 230    | 0                | 0              | 1              |
| dirty_writeback      | 0    | 3     | 496229602  | 32     | 32     | 32     | 32      | 250    | 1                | 0              | 0              |
| resctrl_owner        | 0    | 3     | 723985     | 32     | 32     | 32     | 32      | 250    | 0                | 0              | 1              |
| resctrl_task         | 0    | 3     | 788172     | 32     | 32     | 32     | 32      | 230    | 0                | 0              | 1              |
| resctrl_monitor      | 0    | 3     | 48754070   | 32     | 32     | 32     | 32      | 240    | 2                | 0              | 1              |
| perf_event           | 0    | 7     | 0          | 0      | 0      | 0      | 0       | 0      | 0                | 1              | 1              |
| ftrace_maintenance   | 0    | 7     | 0          | 0      | 0      | 0      | 0       | 0      | 1                | 2              | 1              |
| tlb_flush            | 0    | 7     | 0          | 0      | 0      | 0      | 0       | 0      | 0                | 2              | 1              |
| user_escape_fault    | 0    | 7     | 0          | 0      | 0      | 0      | 0       | 0      | 2                | 2              | 1              |
| user_death           | 0    | 5     | 0          | 0      | 0      | 0      | 0       | 0      | 0                | 0              | 0              |
| rcu_callback_routing | 0    | 3     | 5003       | 16     | 32     | 32     | 32      | 19     | 2                | 0              | 0              |
| stop_machine         | 0    | 7     | 0          | 8      | 32     | 32     | 32      | 0      | 0                | 0              | 1              |
| hotplug_up           | 0    | 7     | 0          | 8      | 32     | 32     | 32      | 0      | 0                | 0              | 1              |
| hotplug_down         | 0    | 3     | 5002       | 16     | 32     | 32     | 32      | 19     | 0                | 0              | 0              |
| timer_migration      | 0    | 7     | 0          | 0      | 0      | 0      | 0       | 0      | 0                | 0              | 1              |
| net_backlog          | 0    | 7     | 0          | 0      | 0      | 0      | 0       | 0      | 1                | 0              | 0              |

All passing rows reported zero IRQ, softirq, TLB, machine-check-exception,
machine-check-poll, and context-switch deltas. The terminal classes exited
through owner stop with the expected workqueue, architecture-IPI, and NMI
correlations; `user_death` reached its expected state 5. `timer_migration`
stopped the CPU 1 owner in 9 ms through the cgroup-v2 partition transition.

`net_backlog` passed under Simon's active tuned `cpu-partitioning`
configuration with owner CPU 5 and work/trigger CPU 0. Plain RPS cannot target
CPU 5, so the harness uses a veth pair across a temporary network namespace
and RFS. The receiver is pinned to CPU 5 and establishes the TCP flow. The
sender starts on CPU 5, sends one byte, enables `TCP_CORK`, queues a second
byte, and then migrates to CPU 0. `SIGUSR1` clears the cork without another
`sendmsg()`, preserving the RFS mapping and steering the held byte to CPU 5;
CPU 0 remains the RPS fallback. The WCL wrapper stopped the owner in 126 ms
with state 7, and the direct trimmed-harness validation stopped in 122 ms.
The evidence is in
`/root/accel-measure/phase8-bff69f4-wcl-net-backlog-rfs23-20261110T2243Z`
and
`/root/accel-measure/phase8-bff69f4-wcl-net-backlog-rfs22-20261110T2241Z`.

### 2026-11-10 focused fault-injection and hotplug-race evidence (simon.lab)

The focused runs used the same kernel and ABI 23, owner CPU 5, hotplug CPU 6,
and load CPUs 7-9. `test-stop-machine-owner-stop.sh` passed two 10-second
bounded cases. The success case returned zero, and the injected fault case
returned ``-EIO``. Both executed the callback on all 12 online CPUs, observed
the target owner active before ``stop_machine()``, and observed neither the
target owner nor any accelerator owner active afterward. Evidence is in
`/root/accel-measure/phase8-stop-machine-fault-20261110T233220Z/evidence-stop-machine-owner-stop`.

The loaded hotplug-race harness passed 16 CPU-up and 16 CPU-down iterations.
CPU-up used 3-second bounded runs and a maximum owner-active interval of
38,812,957 ns. CPU-down completed normally with a maximum owner-active interval
of 3,001,202,150 ns and a maximum hotplug wait of 3,012 ms. Both reports include
p50, p90, p99, and maximum latency tails. Evidence is in
`/root/accel-measure/phase8-stop-machine-fault-20261110T233220Z/evidence-hotplug-up-loaded/summary.tsv`
and
`/root/accel-measure/phase8-stop-machine-fault-20261110T233220Z/evidence-hotplug-down-loaded/summary.tsv`.

The standard WCL runner now integrates the general fault injector as
`stop_machine_fault`. On 2026-11-10 it passed on Simon with owner CPU 5,
work CPU 0, and a 10-second bounded window. Evidence is in
`/root/accel-measure/phase8-stop-machine-fault-wcl-20261110T1747Z/evidence-stop-machine-fault/summary.tsv`.

The lock-order harness records the public wrapper's maintenance gate, owner
exit, CPU-hotplug read lock, and stop-machine return sequence. The direct run
passed both success and injected-fault cases, and the subsequent standard WCL
rerun repeated that result. Both traces ordered the events as `stop_machine`
entry, maintenance begin, owner-stop request, owner-exit completion,
maintenance return, `cpus_read_lock()`, `cpus_read_unlock()`, maintenance end,
and `stop_machine` return. Evidence is in
`/root/accel-measure/phase8-stop-machine-lock-order-20261110T1805Z/evidence-stop-machine-lock-order`
and
`/root/accel-measure/phase8-stop-machine-lock-order-wcl-20261110T1812Z/evidence-stop-machine-fault/summary.tsv`.

The remaining optional classes are unsupported or not applicable on this boot:

- `device_reload`, `device_teardown`, and `device_rebind` fail before creating
  the temporary netdevsim device because this kernel does not export
  `unregister_netdevice`; `device_sriov` additionally lacks
  `nsim_esw_legacy_enable`. Their evidence and per-class logs are preserved in
  `/root/accel-measure/phase8-bff69f4-wcl-optional-20261110/device_*`.
- `migration` is not applicable because Simon has one NUMA node.
- `irq_storm` is not applicable because no isolated test NIC is available.

### Phase 8 disposition inventory

| Area | Current disposition | Evidence |
|------|----------------------|----------|
| Common portable WCL set | Passed on `7.3.0-rc3-accel-tlbfix-gbff69f41a61b` | `/root/accel-measure/phase8-bff69f4-wcl-common-20261110b/summary.tsv` |
| Optional portable WCL classes | Passed individually on the same kernel | `/root/accel-measure/phase8-bff69f4-wcl-optional-20261110/<class>/summary.tsv` |
| `timer_migration` | Passed with the corrected active-marker harness | `/root/accel-measure/phase8-bff69f4-wcl-timer-20261110T1310/summary.tsv` |
| `net_backlog` | Passed with RFS and a cork-held TCP flow under the active tuned `cpu-partitioning` profile | `/root/accel-measure/phase8-bff69f4-wcl-net-backlog-rfs23-20261110T2243Z/summary.tsv` |
| `stop_machine_fault` | Passed success and injected `-EIO` cases with owner drain on all online CPUs and gate-before-hotplug-lock ordering, both directly and through the standard WCL runner | `/root/accel-measure/phase8-stop-machine-fault-20261110T233220Z/evidence-stop-machine-owner-stop`; `/root/accel-measure/phase8-stop-machine-fault-wcl-20261110T1747Z/evidence-stop-machine-fault/summary.tsv`; `/root/accel-measure/phase8-stop-machine-lock-order-wcl-20261110T1812Z/evidence-stop-machine-fault/summary.tsv` |
| Higher-volume hotplug races | Passed 16 loaded CPU-up and 16 loaded CPU-down iterations | `/root/accel-measure/phase8-stop-machine-fault-20261110T233220Z/evidence-hotplug-{up,down}-loaded/summary.tsv` |
| Netdevsim lifecycle classes | Unsupported on this kernel because required symbols are absent | `/root/accel-measure/phase8-bff69f4-wcl-optional-20261110/device_*/summary.tsv` |
| `migration` | Not applicable on this one-NUMA-node host | No runtime run |
| `irq_storm` | Not applicable without an isolated test NIC | No runtime run |
| Broader lifecycle rows | Source-classified in `Documentation/accel/cpu-accel.rst`; runtime runs remain outside this WCL report | `Documentation/accel/cpu-accel.rst` Phase 8 matrix |

## Known limitations and open items

- No hard bound is claimed. The measured maxima are envelopes for the
  listed bounded classes on one VM image; the row's disposition remains
  "selected invariants measured; no hard bound".
- Percentiles are log2-bin upper-edge estimates (factor-of-two ceiling).
- The historical `test-sync-work-owner-stop.sh` assertion failure "trace did
  not pair owner stop with owner exit" was root-caused on 2026-11-10 as a
  harness readiness race: `x86_cpu_accel_user_enter()` was visible to
  function graph before architecture ownership was complete. Waiting for the
  active marker fixes the observed LRU failure; the earlier VM-image runs
  should be rerun with the updated harness before relying on their assertion
  status.
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
- `timer_migration` passed on the earlier `rcu_nocbs=1-11` boot. After the
  later repartition, `net_backlog` passed through RFS steering of a cork-held
  TCP flow; `irq_storm` remains deferred to a test NIC.
- Disposition of out-of-tree direct `stop_machine_cpuslocked()` callers
  remains open; the public `stop_machine()` wrapper lock order is validated.
- Machine-check injection remains out of scope (see the exceptions/NMI
  row).

## Reproduction

```
make -C tools/cpu_accel cpu-accel-testctl test-sync-wait-prime
env CPU_ACCEL_TESTCTL=... CPU_ACCEL_SYNC_WAIT_PRIME=... \
    CPU_ACCEL_SINGLE_PROBE=.../cpu_accel_call_function_single_probe.ko \
    sh tools/cpu_accel/test-wcl-report.sh
```

Per-class logs and the summary TSV land in `$CPU_ACCEL_TRACE_DIR`
(default: a fresh `/tmp/cpu-accel-wcl.XXXXXX`).

— AI-signed: opencode | model: radiance/qwen3.8-27b | effort: not specified
— AI-signed: opencode | model: radiance/qwen3.8-27b | effort: not specified | contribution: added 2026-10-30 re-validation section (build #80)
