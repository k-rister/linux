# Phase 10 SMP 4-CPU `user-oslat` vs rt-tests `oslat` — 24-Hour Exact-Current Soak Test

## Header

- **Host:** `simon.lab.gtristers.consone.net`
- **Kernel:** `Linux simon.lab.gtristers.consone.net 7.3.0-rc3-accel-tlbfix-00522-gda8c5e8e7e62 #2 SMP PREEMPT_DYNAMIC Sat Nov 14 12:00:20 CST 2026 x86_64 GNU/Linux`
- **Tuned profile:** `cpu-partitioning`
- **Window:** `2026-11-15T00:51:30Z` to `2026-11-16T00:51:31Z`
- **Duration:** 86,400 seconds requested; wall-clock window was 86,401 seconds
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
taskset -c 0-3 /root/accel-phase10/evidence/phase10-6f200ac0bcad-20261114T2205Z-current-gates/cpu-accel-testctl run --cpus 5,7,9,11 --duration-ms 86400000 --period-us 1000 --workload user-oslat
```

### Scheduler-class observation

The worker classes below were captured by a separate ten-second observation run using the same control tool and CPU placement. The accelerator workers are CFS (`TS`, nice 0) on CPUs 5, 7, 9, and 11.

```text
captured_utc=2026-11-15T00:51:23Z
user_pid=11075 rt_pid=11076
--- pstree ---
cpu-accel-testc(11075)-+-exe(11078)
                       |-exe(11079)
                       |-exe(11080)
                       `-exe(11081)
--- ps ---
  11078   11075   11078   5  TS  19      -   0 R    exe             [exe]
  11079   11075   11079   7  TS  19      -   0 Z    exe             [exe] <defunct>
  11080   11075   11080   9  TS  19      -   0 Z    exe             [exe] <defunct>
  11081   11075   11081  11  TS  19      -   0 Z    exe             [exe] <defunct>
```

### Per-worker results

| Worker | CPU | State/result | Samples | Rate (samples/s) | Min (ns) | Status avg (ns) | Weighted mean (ns) | Max (ns) | Tool p50/p90/p99/p999 (ns) | >1 µs |
|---:|---:|---|---:|---:|---:|---:|---:|---:|---|---:|
| 0 | 5 | `3/0` | 4,274,916,573,054 | 49478201.077 | 10 | 20 | 20.210845676 | 180 | 32/32/32/32 | 0 |
| 1 | 7 | `3/0` | 4,274,916,287,143 | 49478197.768 | 10 | 20 | 20.210847029 | 320 | 32/32/32/32 | 0 |
| 2 | 9 | `3/0` | 4,274,916,287,143 | 49478197.768 | 10 | 20 | 20.210847029 | 260 | 32/32/32/32 | 0 |
| 3 | 11 | `3/0` | 4,274,916,287,120 | 49478197.768 | 10 | 20 | 20.210847029 | 250 | 32/32/32/32 | 0 |

### Isolation counters

| Worker | CPU | Context switches | need_resched | Escapes | Arch IPIs | Arch IRQs | TLB shootdowns | Resched deferred | Call-function deferred | Migration | Recovery state |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 5 | 0 | 0 | 0 | 0 | 1 | 0 | 1 | 0 | 0 | 0 |
| 1 | 7 | 0 | 0 | 0 | 0 | 1 | 0 | 1 | 1 | 0 | 0 |
| 2 | 9 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 |
| 3 | 11 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 |

### Aggregate result

| Metric | Value |
|---|---:|
| Samples | 17,099,665,434,460 |
| Min | 10 ns |
| Weighted mean | 20.210846691 ns |
| Max | 320 ns |
| Linear-rank p50/p90/p99/p999 | 20/20/30/30 ns |
| Samples >1 µs | 0 |

### 1-ns histogram summary

| Lateness (ns) | Count |
|---:|---:|
| 10 | 5,046,682 |
| 20 | 16,739,114,555,909 |
| 30 | 360,545,830,740 |
| 40 | 1,017 |
| 50 | 32 |
| 60 | 20 |
| 70 | 6 |
| 80 | 12 |
| 90 | 1 |
| 100 | 3 |
| 110 | 16 |
| 120 | 8 |
| 130 | 5 |
| 140 | 4 |
| 180 | 1 |
| 230 | 1 |
| 250 | 1 |
| 260 | 1 |
| 320 | 1 |

## rt-tests `oslat`

### Invocation

```text
taskset -c 0-3 /root/accel-measure/oslat -c 4,6,8,10 -w no -D 86400 -W 1 -f 99 -z -q --json=/root/accel-measure/phase10-4cpu-user-oslat-and-rt-tests-oslat-24h-20261115T0050Z/rt-tests-oslat.json
```

### Scheduler-class observation

The live-process capture shows the rt-tests worker threads as `SCHED_FIFO 99` on CPUs 4, 6, 8, and 10. The tool-reported per-thread measurement duration is 86,329.680 seconds.

```text
captured_utc=2026-11-15T00:51:28Z
user_pid=11075 rt_pid=11076
--- ps ---
  11050   11049   11050   0  TS  19      -   0 Ss   bash            bash -c run_id=phase10-4cpu-user-oslat-and-rt-tests-oslat-24h-20261115T0050Z; evidence=/root/accel-measure/$run_id; mkdir -p "$evidence"; tool=/root/accel-phase10/evidence/phase10-6f200ac0bcad-20261114T2205Z-current-gates/cpu-accel-testctl; taskset -c 0-3 "$tool" run --cpus 5,7,9,11 --duration-ms 10000 --period-us 1000 --workload user-oslat >"$evidence/user-oslat-observation.txt" 2>&1 & upid=$!; taskset -c 0-3 /root/accel-measure/oslat -c 4,6,8,10 -w no -D 10 -W 1 -f 99 -z -q --json="$evidence/rt-tests-oslat-observation.json" >"$evidence/rt-tests-oslat-observation.txt" 2>&1 & rtpid=$!; sleep 5; { echo "captured_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "user_pid=$upid rt_pid=$rtpid"; echo "--- pstree ---"; pstree -p "$upid"; echo "--- ps ---"; ps --ppid "$upid" -o pid=,ppid=,tid=,psr=,cls=,pri=,rtprio=,ni=,stat=,comm=,args=; } > "$evidence/scheduler-live-processes-workers.txt"; { echo "captured_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "user_pid=$upid rt_pid=$rtpid"; echo "--- ps ---"; ps -eLo pid=,ppid=,tid=,psr=,cls=,pri=,rtprio=,ni=,stat=,comm=,args= | grep -E "/root/accel-measure/oslat|oslat"; } > "$evidence/rt-tests-scheduler-observation.txt"; wait "$upid"; wait "$rtpid"
  11076   11050   11076   0  TS  19      -   0 SLl  oslat           /root/accel-measure/oslat -c 4,6,8,10 -w no -D 10 -W 1 -f 99 -z -q --json=/root/accel-measure/phase10-4cpu-user-oslat-and-rt-tests-oslat-24h-20261115T0050Z/rt-tests-oslat-observation.json
  11076   11050   11086   4  FF 139     99   - RLl  oslat           /root/accel-measure/oslat -c 4,6,8,10 -w no -D 10 -W 1 -f 99 -z -q --json=/root/accel-measure/phase10-4cpu-user-oslat-and-rt-tests-oslat-24h-20261115T0050Z/rt-tests-oslat-observation.json
  11076   11050   11087   6  FF 139     99   - RLl  oslat           /root/accel-measure/oslat -c 4,6,8,10 -w no -D 10 -W 1 -f 99 -z -q --json=/root/accel-measure/phase10-4cpu-user-oslat-and-rt-tests-oslat-24h-20261115T0050Z/rt-tests-oslat-observation.json
  11076   11050   11088   8  FF 139     99   - RLl  oslat           /root/accel-measure/oslat -c 4,6,8,10 -w no -D 10 -W 1 -f 99 -z -q --json=/root/accel-measure/phase10-4cpu-user-oslat-and-rt-tests-oslat-24h-20261115T0050Z/rt-tests-oslat-observation.json
  11076   11050   11089  10  FF 139     99   - RLl  oslat           /root/accel-measure/oslat -c 4,6,8,10 -w no -D 10 -W 1 -f 99 -z -q --json=/root/accel-measure/phase10-4cpu-user-oslat-and-rt-tests-oslat-24h-20261115T0050Z/rt-tests-oslat-observation.json
  11095   11050   11095   3  TS  19      -   0 S    grep            grep -E /root/accel-measure/oslat|oslat
```

### Per-thread results

| Thread | CPU | Samples | Rate (samples/s) | Min (ns) | Tool avg (ns) | Weighted mean (ns) | Max (ns) | p50/p90/p99/p999 (ns) | >1 µs |
|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| 0 | 4 | 3,799,844,406,379 | 44015504.359 | 19 | 22.738 | 22.737665117 | 6444 | 20/30/30/30 | 86,390,016 |
| 1 | 6 | 3,799,238,850,520 | 44008489.902 | 19 | 22.741 | 22.741289205 | 8543 | 20/30/30/30 | 86,390,017 |
| 2 | 8 | 3,799,874,286,988 | 44015850.481 | 9 | 22.737 | 22.737486322 | 7993 | 20/30/30/30 | 86,390,017 |
| 3 | 10 | 3,799,827,869,440 | 44015312.804 | 19 | 22.738 | 22.737764073 | 8393 | 20/30/30/30 | 86,390,033 |

### Aggregate result

| Metric | Value |
|---|---:|
| Samples | 15,198,785,413,327 |
| Min | 9 ns |
| Weighted mean | 22.738551069 ns |
| Max | 8543 ns |
| Linear-rank p50/p90/p99/p999 | 20/30/30/30 ns |
| Samples >1 µs | 345,560,083 |

### 1-ns histogram summary

| Lateness (ns) | Count |
|---:|---:|
| 10 | 1 |
| 20 | 11,081,446,862,611 |
| 30 | 4,116,992,868,416 |
| 40 | 122,182 |
| 50 | 3 |
| 60 | 4 |
| 90 | 2 |
| 100 | 11 |
| 110 | 7 |
| 120 | 1 |
| 130 | 2 |
| 140 | 1 |
| 340 | 1 |
| 400 | 1 |
| 930 | 1 |
| 1050 | 2 |
| 1070 | 1 |
| 1120 | 1 |
| 1220 | 95 |
| 1230 | 20,720 |
| 1239 | 1,020,595 |
| 1249 | 12,169,441 |
| 1259 | 41,961,050 |
| 1269 | 46,052,771 |
| 1279 | 21,565,978 |
| 1289 | 29,794,077 |
| 1299 | 53,424,451 |
| 1309 | 32,803,305 |
| 1319 | 8,652,470 |
| 1329 | 2,555,263 |
| 1339 | 1,693,042 |
| 1349 | 1,498,674 |
| 1359 | 1,099,885 |
| 1369 | 813,599 |
| 1379 | 596,507 |
| 1389 | 484,966 |
| 1399 | 548,734 |
| 1409 | 4,215,207 |
| 1419 | 15,226,992 |
| 1429 | 14,905,532 |
| 1439 | 5,494,236 |
| 1449 | 11,799,941 |
| 1459 | 19,048,572 |
| 1469 | 7,884,567 |
| 1479 | 1,682,406 |
| 1489 | 1,043,041 |
| 1499 | 753,853 |
| 1509 | 717,065 |
| 1519 | 1,057,455 |
| 1529 | 1,051,953 |
| 1539 | 669,557 |
| 1549 | 525,601 |
| 1559 | 470,524 |
| 1569 | 204,032 |
| 1579 | 71,375 |
| 1589 | 48,333 |
| 1599 | 45,149 |
| 1609 | 34,761 |
| 1619 | 23,806 |
| 1629 | 19,531 |
| 1639 | 11,743 |
| 1649 | 6,418 |
| 1659 | 6,570 |
| 1669 | 13,153 |
| 1679 | 21,882 |
| 1689 | 18,768 |
| 1699 | 13,974 |
| 1709 | 20,108 |
| 1719 | 19,960 |
| 1729 | 9,771 |
| 1739 | 5,333 |
| 1749 | 12,169 |
| 1759 | 30,543 |
| 1769 | 42,607 |
| 1779 | 32,596 |
| 1789 | 26,452 |
| 1799 | 42,541 |
| 1809 | 88,060 |
| 1819 | 173,431 |
| 1829 | 185,169 |
| 1839 | 134,684 |
| 1849 | 115,094 |
| 1859 | 74,566 |
| 1869 | 48,177 |
| 1879 | 53,573 |
| 1889 | 73,905 |
| 1899 | 53,784 |
| 1909 | 26,727 |
| 1919 | 18,367 |
| 1929 | 16,538 |
| 1939 | 13,680 |
| 1949 | 10,303 |
| 1959 | 6,311 |
| 1969 | 3,976 |
| 1979 | 2,967 |
| 1989 | 3,093 |
| 1999 | 2,854 |
| 2009 | 1,716 |
| 2019 | 1,296 |
| 2029 | 1,266 |
| 2039 | 877 |
| 2049 | 551 |
| 2059 | 450 |
| 2069 | 339 |
| 2079 | 291 |
| 2089 | 224 |
| 2099 | 196 |
| 2109 | 198 |
| 2119 | 191 |
| 2129 | 156 |
| 2139 | 117 |
| 2149 | 115 |
| 2159 | 122 |
| 2169 | 182 |
| 2179 | 570 |
| 2189 | 2,008 |
| 2199 | 5,388 |
| 2209 | 10,628 |
| 2219 | 16,646 |
| 2229 | 21,579 |
| 2239 | 19,021 |
| 2249 | 14,652 |
| 2259 | 15,454 |
| 2269 | 18,088 |
| 2279 | 16,922 |
| 2289 | 11,728 |
| 2299 | 9,543 |
| 2309 | 13,367 |
| 2319 | 13,697 |
| 2329 | 7,716 |
| 2339 | 3,528 |
| 2349 | 2,525 |
| 2359 | 2,709 |
| 2369 | 3,318 |
| 2379 | 4,295 |
| 2389 | 8,434 |
| 2399 | 14,965 |
| 2409 | 17,541 |
| 2419 | 12,422 |
| 2429 | 5,232 |
| 2439 | 3,130 |
| 2449 | 3,063 |
| 2459 | 2,758 |
| 2468 | 1,980 |
| 2478 | 1,320 |
| 2488 | 1,146 |
| 2498 | 1,078 |
| 2508 | 881 |
| 2518 | 793 |
| 2528 | 816 |
| 2538 | 515 |
| 2548 | 276 |
| 2558 | 202 |
| 2568 | 149 |
| 2578 | 128 |
| 2588 | 102 |
| 2598 | 74 |
| 2608 | 58 |
| 2618 | 53 |
| 2628 | 59 |
| 2638 | 39 |
| 2648 | 34 |
| 2658 | 33 |
| 2668 | 22 |
| 2678 | 20 |
| 2688 | 17 |
| 2698 | 15 |
| 2708 | 11 |
| 2718 | 10 |
| 2728 | 11 |
| 2738 | 6 |
| 2748 | 6 |
| 2758 | 8 |
| 2768 | 4 |
| 2778 | 8 |
| 2788 | 8 |
| 2798 | 4 |
| 2808 | 2 |
| 2818 | 4 |
| 2838 | 4 |
| 2848 | 1 |
| 2858 | 1 |
| 2868 | 1 |
| 2878 | 1 |
| 2908 | 1 |
| 2938 | 1 |
| 2958 | 1 |
| 3008 | 1 |
| 3018 | 1 |
| 3068 | 1 |
| 3508 | 1 |
| 3578 | 1 |
| 3598 | 1 |
| 3608 | 1 |
| 3618 | 12 |
| 3628 | 15 |
| 3638 | 13 |
| 3648 | 33 |
| 3658 | 55 |
| 3668 | 132 |
| 3678 | 256 |
| 3688 | 404 |
| 3698 | 502 |
| 3707 | 683 |
| 3717 | 998 |
| 3727 | 1,256 |
| 3737 | 1,386 |
| 3747 | 1,577 |
| 3757 | 1,987 |
| 3767 | 2,610 |
| 3777 | 3,100 |
| 3787 | 3,061 |
| 3797 | 3,088 |
| 3807 | 3,424 |
| 3817 | 3,507 |
| 3827 | 3,544 |
| 3837 | 3,452 |
| 3847 | 3,743 |
| 3857 | 3,566 |
| 3867 | 3,243 |
| 3877 | 2,870 |
| 3887 | 2,897 |
| 3897 | 2,538 |
| 3907 | 2,327 |
| 3917 | 2,020 |
| 3927 | 1,743 |
| 3937 | 1,592 |
| 3947 | 1,507 |
| 3957 | 1,230 |
| 3967 | 1,075 |
| 3977 | 938 |
| 3987 | 790 |
| 3997 | 649 |
| 4007 | 544 |
| 4017 | 471 |
| 4027 | 410 |
| 4037 | 379 |
| 4047 | 316 |
| 4057 | 266 |
| 4067 | 207 |
| 4077 | 188 |
| 4087 | 167 |
| 4097 | 154 |
| 4107 | 119 |
| 4117 | 125 |
| 4127 | 103 |
| 4137 | 83 |
| 4147 | 85 |
| 4157 | 80 |
| 4167 | 50 |
| 4177 | 58 |
| 4187 | 48 |
| 4197 | 24 |
| 4207 | 28 |
| 4217 | 23 |
| 4227 | 16 |
| 4237 | 19 |
| 4247 | 17 |
| 4257 | 11 |
| 4267 | 12 |
| 4277 | 14 |
| 4287 | 10 |
| 4297 | 6 |
| 4307 | 11 |
| 4317 | 7 |
| 4327 | 6 |
| 4337 | 8 |
| 4347 | 4 |
| 4357 | 9 |
| 4367 | 4 |
| 4377 | 6 |
| 4387 | 5 |
| 4397 | 5 |
| 4407 | 10 |
| 4417 | 5 |
| 4427 | 3 |
| 4437 | 5 |
| 4447 | 5 |
| 4457 | 4 |
| 4467 | 2 |
| 4487 | 3 |
| 4497 | 5 |
| 4507 | 3 |
| 4517 | 3 |
| 4547 | 2 |
| 4557 | 1 |
| 4567 | 1 |
| 4587 | 1 |
| 4597 | 1 |
| 4607 | 2 |
| 4647 | 3 |
| 4657 | 1 |
| 4667 | 2 |
| 4677 | 1 |
| 4697 | 1 |
| 4717 | 1 |
| 4747 | 1 |
| 4757 | 1 |
| 4787 | 1 |
| 4797 | 1 |
| 4817 | 1 |
| 4857 | 1 |
| 6445 | 1 |
| 7994 | 1 |
| 8394 | 1 |
| 8544 | 1 |

## Run health and artifacts

- **Window:** `2026-11-15T00:51:30Z` to `2026-11-16T00:51:31Z`
- **Exits:** `user-oslat` `0`; rt-tests `oslat` `0`
- **Module refcount before:** 0
- **Module refcount after:** 0
- **NVMe QID 2 timeouts:** none
- **`cpu_accel` runtime errors:** none new during the run
- **Checksum verification:** `sha256sum -c sha256sums.txt` passed for all 14 raw artifacts

Artifact directory: `/root/accel-measure/phase10-4cpu-user-oslat-and-rt-tests-oslat-24h-20261115T0050Z`

Raw and supporting artifacts:

- `meta.txt`
- `user-oslat.txt`
- `rt-tests-oslat.json`
- `rt-tests-oslat.txt`
- `live-processes.txt`
- `scheduler-live-processes-workers.txt`
- `rt-tests-scheduler-observation.txt`
- `user-oslat-observation.txt`
- `rt-tests-oslat-observation.txt`
- `rt-tests-oslat-observation.json`
- `pre-dmesg.txt`
- `post-dmesg.txt`
- `post-run.txt`
- `sha256sums.txt`

Artifact hashes:

```text
6fbd77a46d7a45d43dfa7bad620bac624480739bc0618df3fb5fc38dc3a66690  /root/accel-phase10/evidence/phase10-6f200ac0bcad-20261114T2205Z-current-gates/cpu-accel-testctl
3e590206dbfb8c13a33beff3e87a4e191bca9e5894b8a22a82d9a9d5558d90d3  /root/accel-measure/oslat
0ea5ae4b8b3b9958567191b6fe44082156cf1b80846916de9fcbed78c4eb2cd2  /lib/modules/7.3.0-rc3-accel-tlbfix-00522-gda8c5e8e7e62/kernel/drivers/cpu_accel/cpu_accel.ko
```
