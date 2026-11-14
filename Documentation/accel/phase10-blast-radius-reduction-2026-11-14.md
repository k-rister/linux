# Phase 10 SMP 4-CPU `user-oslat` vs rt-tests `oslat` — 10-Minute Blast-Radius Reduction Smoke Test

## Header

- **Host:** `simon.lab.gtristers.consone.net`
- **Kernel:** `Linux simon.lab.gtristers.consone.net 7.3.0-rc3-accel-tlbfix-00522-gda8c5e8e7e62 #2 SMP PREEMPT_DYNAMIC Sat Nov 14 12:00:20 CST 2026 x86_64 GNU/Linux`
- **Tuned profile:** `cpu-partitioning`
- **Window:** `2026-11-14T22:15:15Z` to `2026-11-14T22:25:16Z`
- **Duration:** 600 seconds requested
- **Concurrency:** both tools ran concurrently on four CPUs each
- **Workloads:** nop-only / no optional workload
- **cpu-accel ABI:** 25
- **Completion:** `user-oslat` exit `0`; rt-tests `oslat` exit `0`

## Boot command line and isolation

```text
BOOT_IMAGE=(hd0,gpt2)/vmlinuz-7.3.0-rc3-accel-tlbfix-00522-gda8c5e8e7e62 earlycon=ttyS0,115200 console=ttyS0,115200 root=/dev/mapper/almalinux-root ro crashkernel=2G-64G:256M,64G-:512M resume=UUID=1a4b3557-9483-455f-be24-1ad6a1fb957c rd.lvm.lv=almalinux/root rd.lvm.lv=almalinux/swap nosmt iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=16 amd_iommu=on skew_tick=1 tsc=reliable rcupdate.rcu_normal_after_boot=1 rcutree.nohz_full_patience_delay=1000 nohz=on nohz_full=4-11 rcu_nocbs=4-11 tuned.non_isolcpus=0000000f systemd.cpu_affinity=0-3 intel_pstate=disable nosoftlockup isolcpus=managed_irq,4-11 irqaffinity=0-3
```

Observed isolation flags: `nohz_full=4-11`, `rcu_nocbs=4-11`, `skew_tick=1`, `rcutree.nohz_full_patience_delay=1000`, `tuned.non_isolcpus=0000000f`, `systemd.cpu_affinity=0-3`, `isolcpus=managed_irq,4-11`, `irqaffinity=0-3`

## `user-oslat`

### Invocation

```text
taskset -c 0-3 /root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T2215Z-current/cpu-accel-testctl run --cpus 5,7,9,11 --duration-ms 600000 --period-us 1000 --workload user-oslat
```

### Scheduler-class observation

The worker classes below were captured by a separate five-second observation
run using the same control tool and CPU placement.

```text
captured_utc=2026-11-14T22:26:44Z
user_pid=9933
--- pstree ---
cpu-accel-testc(9933)-+-exe(9935)
                      |-exe(9936)
                      |-exe(9937)
                      `-exe(9938)
--- ps ---
   9933    9887    9933   0  TS  19      -   0 SL   cpu-accel-testc /root/accel-phase10/evidence/phase10-6f200ac0bcad-20261114T2205Z-current-gates/cpu-accel-testctl run --cpus 5,7,9,11 --duration-ms 10000 --period-us 1000 --workload user-oslat
   9935    9933    9935   5  TS  19      -   0 R    exe             [exe]
   9936    9933    9936   7  TS  19      -   0 Z    exe             [exe] <defunct>
   9937    9933    9937   9  TS  19      -   0 Z    exe             [exe] <defunct>
   9938    9933    9938  11  TS  19      -   0 Z    exe             [exe] <defunct>
```

### Per-worker results

| Worker | CPU | State/result | Samples | Rate (samples/s) | Min (ns) | Status avg (ns) | Weighted mean (ns) | Max (ns) | Tool p50/p90/p99/p999 (ns) | >1 µs |
|---:|---:|---|---:|---:|---:|---:|---:|---:|---|---:|
| 0 | 5 | `3/0` | 29,735,249,726 | 49558667.677 | 10 | 20 | 20.178043034 | 140 | 32/32/32/32 | 0 |
| 1 | 7 | `3/0` | 29,735,246,483 | 49558661.355 | 10 | 20 | 20.178045233 | 250 | 32/32/32/32 | 0 |
| 2 | 9 | `3/0` | 29,735,246,413 | 49558665.027 | 10 | 20 | 20.178045222 | 250 | 32/32/32/32 | 0 |
| 3 | 11 | `3/0` | 29,735,246,466 | 49558658.505 | 10 | 20 | 20.178045240 | 220 | 32/32/32/32 | 0 |

### Isolation counters

| Worker | CPU | Context switches | need_resched | Escapes | Arch IPIs | Arch IRQs | TLB shootdowns | Resched deferred | Call-function deferred | Migration | Recovery state |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 5 | 0 | 0 | 0 | 0 | 1 | 0 | 1 | 0 | 0 | 0 |
| 1 | 7 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 0 | 0 |
| 2 | 9 | 0 | 0 | 0 | 0 | 2 | 0 | 1 | 0 | 0 | 0 |
| 3 | 11 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 0 | 0 |

### Aggregate result

| Metric | Value |
|---|---:|
| Samples | 118,940,989,088 |
| Min | 10 ns |
| Weighted mean | 20.178044682 ns |
| Max | 250 ns |
| Linear-rank p50/p90/p99/p999 | 20/20/30/30 ns |
| Samples >1 µs | 0 |

### 1-ns histogram summary

| Lateness (ns) | Count |
|---:|---:|
| 10 | 22,763,853 |
| 20 | 116,777,780,702 |
| 30 | 2,140,444,468 |
| 40 | 8 |
| 50 | 1 |
| 60 | 8 |
| 70 | 20 |
| 90 | 1 |
| 100 | 4 |
| 110 | 13 |
| 120 | 4 |
| 130 | 2 |
| 140 | 1 |
| 220 | 1 |
| 250 | 2 |

## rt-tests `oslat`

### Invocation

```text
taskset -c 0-3 /root/accel-measure/oslat -c 4,6,8,10 -w no -D 600 -W 1 -f 99 -z -q --json=/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T2215Z-current/rt-tests-oslat.json
```

### Scheduler-class observation

The live-process capture shows the rt-tests worker threads as `SCHED_FIFO 99` on CPUs `4,6,8,10`.

### Per-thread results

| Thread | CPU | Samples | Rate (samples/s) | Min (ns) | Tool avg (ns) | Weighted mean (ns) | Max (ns) | p50/p90/p99/p999 (ns) | >1 µs |
|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| 0 | 4 | 26,429,753,685 | 44085592.709 | 19 | 22.702 | 22.701518871 | 6374 | 20/30/30/30 | 595,469 |
| 1 | 6 | 26,425,734,920 | 44078889.293 | 19 | 22.705 | 22.704971250 | 8772 | 20/30/30/30 | 595,470 |
| 2 | 8 | 26,430,115,903 | 44086196.899 | 19 | 22.701 | 22.701207768 | 8363 | 20/30/30/30 | 595,470 |
| 3 | 10 | 26,429,883,524 | 44085809.284 | 19 | 22.701 | 22.701407362 | 8453 | 20/30/30/30 | 595,470 |

### Aggregate result

| Metric | Value |
|---|---:|
| Samples | 105,715,488,032 |
| Min | 20 ns |
| Weighted mean | 22.702276206 ns |
| Max | 8773 ns |
| Linear-rank p50/p90/p99/p999 | 20/30/30/30 ns |
| Samples >1 µs | 2,381,879 |

### 1-ns histogram summary

| Lateness (ns) | Count |
|---:|---:|
| 20 | 77,457,765,218 |
| 30 | 28,255,340,799 |
| 40 | 123 |
| 50 | 6 |
| 60 | 1 |
| 70 | 1 |
| 100 | 1 |
| 110 | 1 |
| 120 | 1 |
| 730 | 1 |
| 800 | 1 |
| 1010 | 2 |
| 1020 | 1 |
| 1060 | 1 |
| 1090 | 1 |
| 1100 | 1 |
| 1150 | 1 |
| 1160 | 1 |
| 1220 | 7 |
| 1230 | 581 |
| 1239 | 16,035 |
| 1249 | 130,055 |
| 1259 | 303,615 |
| 1269 | 265,651 |
| 1279 | 137,688 |
| 1289 | 192,436 |
| 1299 | 330,544 |
| 1309 | 247,852 |
| 1319 | 81,352 |
| 1329 | 18,638 |
| 1339 | 9,911 |
| 1349 | 10,485 |
| 1359 | 7,826 |
| 1369 | 3,749 |
| 1379 | 2,147 |
| 1389 | 3,047 |
| 1399 | 4,972 |
| 1409 | 26,639 |
| 1419 | 101,973 |
| 1429 | 110,511 |
| 1439 | 40,197 |
| 1449 | 76,725 |
| 1459 | 133,905 |
| 1469 | 60,436 |
| 1479 | 11,026 |
| 1489 | 6,126 |
| 1499 | 5,733 |
| 1509 | 4,987 |
| 1519 | 5,553 |
| 1529 | 6,101 |
| 1539 | 4,595 |
| 1549 | 3,227 |
| 1559 | 2,601 |
| 1569 | 1,133 |
| 1579 | 422 |
| 1589 | 383 |
| 1599 | 321 |
| 1609 | 217 |
| 1619 | 164 |
| 1629 | 100 |
| 1639 | 68 |
| 1649 | 48 |
| 1659 | 59 |
| 1669 | 108 |
| 1679 | 152 |
| 1689 | 139 |
| 1699 | 109 |
| 1709 | 129 |
| 1719 | 149 |
| 1729 | 114 |
| 1739 | 62 |
| 1749 | 143 |
| 1759 | 280 |
| 1769 | 241 |
| 1779 | 144 |
| 1789 | 194 |
| 1799 | 459 |
| 1809 | 1,001 |
| 1819 | 1,292 |
| 1829 | 941 |
| 1839 | 734 |
| 1849 | 495 |
| 1859 | 304 |
| 1869 | 380 |
| 1879 | 653 |
| 1889 | 618 |
| 1899 | 307 |
| 1909 | 202 |
| 1919 | 164 |
| 1929 | 150 |
| 1939 | 100 |
| 1949 | 67 |
| 1959 | 37 |
| 1969 | 25 |
| 1979 | 18 |
| 1989 | 9 |
| 1999 | 10 |
| 2009 | 12 |
| 2019 | 13 |
| 2029 | 14 |
| 2039 | 3 |
| 2049 | 3 |
| 2059 | 2 |
| 2069 | 4 |
| 2079 | 1 |
| 2089 | 1 |
| 2099 | 4 |
| 2109 | 2 |
| 2119 | 3 |
| 2129 | 1 |
| 2139 | 1 |
| 2149 | 3 |
| 2159 | 2 |
| 2169 | 21 |
| 2179 | 39 |
| 2189 | 95 |
| 2199 | 82 |
| 2209 | 150 |
| 2219 | 208 |
| 2229 | 207 |
| 2239 | 148 |
| 2249 | 83 |
| 2259 | 90 |
| 2269 | 79 |
| 2279 | 69 |
| 2289 | 51 |
| 2299 | 41 |
| 2309 | 37 |
| 2319 | 31 |
| 2329 | 22 |
| 2339 | 11 |
| 2349 | 8 |
| 2359 | 2 |
| 2369 | 4 |
| 2379 | 8 |
| 2389 | 7 |
| 2399 | 3 |
| 2409 | 4 |
| 2419 | 2 |
| 2429 | 1 |
| 2439 | 3 |
| 2459 | 2 |
| 2488 | 1 |
| 2508 | 1 |
| 2518 | 1 |
| 2558 | 2 |
| 2598 | 1 |
| 2648 | 1 |
| 2698 | 1 |
| 2718 | 1 |
| 3058 | 1 |
| 3218 | 1 |
| 3228 | 1 |
| 3258 | 1 |
| 3308 | 1 |
| 3318 | 6 |
| 3338 | 5 |
| 3348 | 2 |
| 3358 | 7 |
| 3368 | 14 |
| 3378 | 18 |
| 3388 | 9 |
| 3398 | 14 |
| 3408 | 29 |
| 3418 | 24 |
| 3428 | 32 |
| 3438 | 31 |
| 3448 | 30 |
| 3458 | 36 |
| 3468 | 22 |
| 3478 | 24 |
| 3488 | 28 |
| 3498 | 18 |
| 3508 | 15 |
| 3518 | 15 |
| 3528 | 12 |
| 3538 | 10 |
| 3548 | 12 |
| 3558 | 9 |
| 3568 | 15 |
| 3578 | 7 |
| 3588 | 7 |
| 3598 | 7 |
| 3608 | 5 |
| 3618 | 9 |
| 3628 | 2 |
| 3638 | 2 |
| 3658 | 3 |
| 3668 | 5 |
| 3678 | 1 |
| 3688 | 1 |
| 3698 | 1 |
| 3707 | 1 |
| 3717 | 1 |
| 3737 | 2 |
| 3767 | 1 |
| 3777 | 2 |
| 3797 | 1 |
| 3817 | 1 |
| 3877 | 1 |
| 3917 | 1 |
| 4037 | 1 |
| 4587 | 1 |
| 4956 | 1 |
| 5186 | 1 |
| 6375 | 1 |
| 6985 | 1 |
| 7295 | 1 |
| 7414 | 1 |
| 8364 | 1 |
| 8454 | 1 |
| 8773 | 1 |

## Raw and supporting artifacts

- Artifact directory: `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T2215Z-current`
- `meta.txt`
- `user-oslat.txt`
- `rt-tests-oslat.json`
- `rt-tests-oslat.txt`
- `live-processes.txt`
- `post-run.txt`
- `pre-dmesg.txt`
- `post-dmesg.txt`
- `sha256sums.txt`
- Parsed summary: `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T2215Z-current/parsed-summary.json`

Artifact hashes:

```text
6fbd77a46d7a45d43dfa7bad620bac624480739bc0618df3fb5fc38dc3a66690  /root/accel-phase10/evidence/phase10-6f200ac0bcad-20261114T2205Z-current-gates/cpu-accel-testctl
3e590206dbfb8c13a33beff3e87a4e191bca9e5894b8a22a82d9a9d5558d90d3  /root/accel-measure/oslat
0ea5ae4b8b3b9958567191b6fe44082156cf1b80846916de9fcbed78c4eb2cd2  /lib/modules/7.3.0-rc3-accel-tlbfix-00522-gda8c5e8e7e62/kernel/drivers/cpu_accel/cpu_accel.ko
```
