# Phase 10 SMP 4-CPU `user-oslat` vs rt-tests `oslat` — 10-Minute Blast-Radius Reduction Smoke Test

## Header

- **Host:** `simon.lab.gtristers.consone.net`
- **Kernel:** `7.3.0-rc3-accel-tlbfix-00508-g40288c9206c1-dirty` (`#1`)
- **Tuned profile:** `cpu-partitioning`
- **Window:** `2026-11-14T12:21:42Z` to `2026-11-14T12:31:43Z` (`10m 1s` wall)
- **Duration:** 600 seconds requested; `user-oslat` active time was approximately `600.0004 s` per worker, and rt-tests `oslat` reported `599.510 s` per thread
- **Concurrency:** both tools ran concurrently on four CPUs each
- **Workloads:** nop-only / no optional workload
- **cpu-accel ABI:** 25
- **`cpu_accel.ko` SHA-256:** `41711cd3d7e871ee9c7494e7f1f9e9717a5cae5e1d29b1573f7a00601d1055e8`
- **`cpu-accel-testctl` SHA-256:** `6fbd77a46d7a45d43dfa7bad620bac624480739bc0618df3fb5fc38dc3a66690`
- **rt-tests `oslat`:** V2.11; SHA-256 `3e590206dbfb8c13a33beff3e87a4e191bca9e5894b8a22a82d9a9d5558d90d3`
- **Completion:** both tools exited `0`; all four `user-oslat` workers reported `state=3 result=0`; module refcount returned to `0`

## Boot Command Line And Isolation

```text
earlycon=ttyS0,115200 console=ttyS0,115200 root=/dev/mapper/almalinux-root ro crashkernel=2G-64G:256M,64G-:512M resume=UUID=1a4b3557-9483-455f-be24-1ad6a1fb957c rd.lvm.lv=almalinux/root rd.lvm.lv=almalinux/swap nosmt iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=16 amd_iommu=on skew_tick=1 tsc=reliable rcupdate.rcu_normal_after_boot=1 rcutree.nohz_full_patience_delay=1000 nohz=on nohz_full=4-11 rcu_nocbs=4-11 tuned.non_isolcpus=0000000f systemd.cpu_affinity=0-3 intel_pstate=disable nosoftlockup isolcpus=managed_irq,4-11 irqaffinity=0-3
```

Observed isolation flags:

- `nohz_full=4-11`
- `rcu_nocbs=4-11`
- `skew_tick=1`
- `rcutree.nohz_full_patience_delay=1000`
- `nosmt`
- `tuned.non_isolcpus=0000000f`
- `systemd.cpu_affinity=0-3`
- `isolcpus=managed_irq,4-11`
- `irqaffinity=0-3`

## `user-oslat`

### Invocation

```text
taskset -c 0-3 /root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/cpu-accel-testctl run --cpus 5,7,9,11 --duration-ms 600000 --period-us 1000 --workload user-oslat
```

### Scheduler-class observation

The 10-minute run's `live-processes.txt` captured only the process managers.
The classes below were confirmed by separate five-second observation runs:
the manager was `SCHED_OTHER` / CFS on CPU 0, and the worker processes were
`SCHED_OTHER` / CFS, pinned to CPUs `5,7,9,11`.

### Per-worker results

| Worker | CPU | State/result | Samples | Rate (samples/s) | Min (ns) | Status avg (ns) | Weighted mean (ns) | Max (ns) | Tool p50/p90/p99/p999 (ns) | >1 µs |
|---:|---:|---|---:|---:|---:|---:|---:|---:|---|---:|
| 0 | 5 | `3/0` | 29,779,879,301 | 49633095.340 | 10 | 20 | 20.147772758 | 290 | 32/32/32/32 | 0 |
| 1 | 7 | `3/0` | 29,779,875,205 | 49633090.232 | 10 | 20 | 20.147775528 | 240 | 32/32/32/32 | 0 |
| 2 | 9 | `3/0` | 29,779,875,144 | 49633089.632 | 10 | 20 | 20.147775566 | 280 | 32/32/32/32 | 0 |
| 3 | 11 | `3/0` | 29,779,875,048 | 49633093.035 | 10 | 20 | 20.147775564 | 270 | 32/32/32/32 | 0 |

`user-oslat` reports `hist_p*_ns` as the upper edge of the containing log2 bin. The aggregate 1-ns linear-histogram rank values are `p50=20 ns`, `p90=20 ns`, `p99=30 ns`, and `p999=30 ns`.

### Isolation counters

| Worker | CPU | Context switches | need_resched | Escapes | Arch IPIs | Arch IRQs | TLB shootdowns | Resched deferred | Call-function deferred | Migration | Recovery state |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 5 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 1 | 0 | 0 |
| 1 | 7 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 0 | 0 |
| 2 | 9 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 0 | 0 |
| 3 | 11 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 0 | 0 |
| **Total** | **5,7,9,11** | **0** | **0** | **0** | **0** | **0** | **0** | **5** | **4** | **0** | **0** |

### Aggregate result

| Metric | Value |
|---|---:|
| Samples | 119,119,504,698 |
| Aggregate rate | 198532368.239 samples/s |
| Min | 10 ns |
| Weighted mean | 20.147774854 ns |
| Max | 290 ns |
| Tool p50/p90/p99/p999 | 32/32/32/32 ns |
| Linear-rank p50/p90/p99/p999 | 20/20/30/30 ns |
| Samples >1 µs | 0 |

### 1-ns histogram summary

All nonzero aggregate bins:

| Lateness (ns) | Count |
|---:|---:|
| 10 | 31,150,901 |
| 20 | 117,296,916,734 |
| 30 | 1,791,436,983 |
| 40 | 7 |
| 60 | 5 |
| 70 | 23 |
| 100 | 9 |
| 110 | 19 |
| 120 | 4 |
| 130 | 1 |
| 140 | 2 |
| 150 | 1 |
| 160 | 2 |
| 170 | 1 |
| 180 | 1 |
| 230 | 1 |
| 240 | 1 |
| 270 | 1 |
| 280 | 1 |
| 290 | 1 |

## rt-tests `oslat`

### Invocation

```text
taskset -c 0-3 /root/accel-measure/oslat -c 4,6,8,10 -w no -D 600 -W 1 -f 99 -z -q --json=/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/rt-tests-oslat.json
```

### Scheduler-class observation

The 10-minute run's `live-processes.txt` captured only the process managers.
The classes below were confirmed by a separate five-second observation run:
the main process was `SCHED_OTHER` / CFS on CPU 0, and the worker threads were
`SCHED_FIFO 99` on CPUs `4,6,8,10`.

### Per-thread results

Percentiles are computed from the 1-ns-resolution histogram; rt-tests `oslat` JSON does not emit percentile fields. Min/avg/max are the tool’s JSON values converted to nanoseconds.

| Thread | CPU | Samples | Rate (samples/s) | Min (ns) | Tool avg (ns) | Weighted mean (ns) | Max (ns) | p50/p90/p99/p999 (ns) | >1 µs |
|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| 0 | 4 | 26,465,800,993 | 44145720.660 | 19 | 22.671 | 22.670606292 | 7334 | 20/30/30/30 | 592,899 |
| 1 | 6 | 26,461,982,094 | 44139350.626 | 19 | 22.674 | 22.673878017 | 9842 | 20/30/30/30 | 592,900 |
| 2 | 8 | 26,466,275,323 | 44146511.856 | 19 | 22.670 | 22.670199996 | 9262 | 20/30/30/30 | 592,899 |
| 3 | 10 | 26,466,264,175 | 44146493.261 | 19 | 22.670 | 22.670209541 | 9082 | 20/30/30/30 | 592,898 |

### Aggregate result

| Metric | Value |
|---|---:|
| Samples | 105,860,322,585 |
| Aggregate rate | 176578076.404 samples/s |
| Min | 20 ns |
| Weighted mean | 22.671223357 ns |
| Max | 9843 ns |
| Linear-rank p50/p90/p99/p999 | 20/30/30/30 ns |
| Samples >1 µs | 2,371,596 |
| Samples >1 µs | 22.403068 ppm |
| ~1.45 µs cluster (1.30–1.60 µs) | 2,253,170 samples |
| ~1.45 µs cluster (1.30–1.60 µs) | 21.284367 ppm |
| Cluster share of >1 µs | 95.006485% |

### Histogram summary

- Dominant bulk bins:
  - `20 ns`: 77,923,595,546 samples
  - `30 ns`: 27,934,355,358 samples
- The `~1.45 µs` cluster (`1.30–1.60 µs`) contains 2,253,170 samples and accounts for 95.006485% of all samples above 1 µs.
- Per-thread cluster counts:
  - CPU 4: 561,998
  - CPU 6: 510,769
  - CPU 8: 590,910
  - CPU 10: 589,493
- Maximum observed latency: 9842 ns on CPU 6.

## Comparison

| Metric | `user-oslat` | rt-tests `oslat` |
|---|---:|---:|
| Role | Owned, isolated user workload | Non-owned, in-band FIFO workload |
| Scheduler class | CFS / `SCHED_OTHER` | `SCHED_FIFO 99` |
| CPUs | 5, 7, 9, 11 | 4, 6, 8, 10 |
| Samples | 119,119,504,698 | 105,860,322,585 |
| Aggregate rate | 198532368.239/s | 176578076.404/s |
| Min | 10 ns | 20 ns |
| Weighted mean | 20.147774854 ns | 22.671223357 ns |
| Max | 290 ns | 9843 ns |
| Tool/computed p50/p90/p99/p999 | 32/32/32/32 ns tool; 20/20/30/30 ns linear | 20/30/30/30 ns linear |
| Samples >1 µs | 0 | 2,371,596 |
| ~1.45 µs cluster | 0 | 2,253,170 |
| Context switches | 0 | n/a |
| Arch IPIs / IRQs | 0 / 0 | n/a |
| Resched / call-function deferred | 5 / 4 | n/a |
| Migration | 0 | n/a |
| Recovery state | 0 | n/a |

## Read-Out

- Bulk behavior is close: `user-oslat` averaged 20.147774854 ns, rt-tests `oslat` averaged 22.671223357 ns, and both linear-rank p99/p999 values were 30/30 ns and 30/30 ns, respectively.
- `user-oslat` completed 12.433192% more aggregate iterations per second than rt-tests `oslat`.
- Tail behavior diverged sharply: `user-oslat` peaked at 290 ns with 0 samples above 1 µs; rt-tests `oslat` peaked at 9843 ns with 2,371,596 samples above 1 µs (22.403068 ppm).
- The rt-tests maximum was 33.941379× the `user-oslat` maximum.
- The rt-tests `~1.45 µs` cluster contained 2,253,170 samples (21.284367 ppm), or 95.006485% of its above-1-µs samples.
- The divergence is consistent with ownership and isolation: `user-oslat` ran on owned, isolated CFS workers, while rt-tests `oslat` ran as non-owned, in-band FIFO-99 threads.
- All four `user-oslat` workers remained healthy with zero context switches, need-resched samples, escapes, migrations, and recovery events. They reported 0 arch IRQ, 5 deferred reschedule IPIs, and 4 deferred call-function IPIs in total.
- Both processes exited 0, and the cpu-accel module refcount returned to 0.
- No new NVMe QID 2 I/O timeouts appeared. The only cpu-accel dmesg line was the expected out-of-tree module taint at load; there were no runtime errors.

## Raw And Supporting Artifacts On Simon

- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/meta.txt`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/live-processes.txt`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/user-oslat.txt`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/rt-tests-oslat.txt`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/rt-tests-oslat.json`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/pre-dmesg.txt`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/post-dmesg.txt`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/post-run.txt`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/sha256sums.txt`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/cpu_accel.ko`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/cpu-accel-testctl`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/oslat`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/parsed-summary.json`
- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T122142Z/report.md`
- `/root/accel-measure/phase10-blast-v3-scheduler-observation-20261114T123349Z`
- `/root/accel-measure/phase10-blast-v3-scheduler-observation-targetcpus-20261114T123417Z`
