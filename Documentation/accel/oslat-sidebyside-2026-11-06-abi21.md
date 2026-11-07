# OSlat side-by-side — cpu-accel `user-oslat` (ABI 21) vs rt-tests `oslat` — 2026-11-06

Concurrent 600 s comparison run, in the standard side-by-side format, performed to
**verify the ABI-21 `avg_lateness_ns` fix** (64-bit histograms) is working as intended.

- **Box:** `192.168.12.219` (simon.lab)
- **Kernel:** `7.3.0-rc3-accel-tlbfix-g635d5b061dd8`
- **cmdline:** `nosmt skew_tick=1 rcutree.nohz_full_patience_delay=1000 nohz_full=1-11 rcu_nocbs=1-11`
- **Module:** `cpu_accel`, ABI 21, `srcversion 54621BD98AB74849843B188`
- **Tool:** `cpu-accelctl-abi21` (ABI-21)
- **Run:** both scenarios **concurrent**, 600 s each, wall 02:24:19Z → 02:34:21Z UTC
- **Isolation:** CPUs 1-11 `nohz_full` + `rcu_nocbs` + `skew_tick=1`; CPU 0 housekeeping; SMT off

---

## Scenario A — cpu-accel `user-oslat` (CPU 3, owned, CFS)

- **Invocation:** `taskset -c 0-11 cpu-accelctl-abi21 run --cpu 3 --workload user-oslat --duration-ms 600000`
- **Live class:** CFS owner (ring-3 user image); manager CFS.

| min | mean (1-ns hist) | avg (module) | max | p50 | p90 | p99 | p999 |
|-----|------------------|--------------|-----|-----|-----|-----|------|
| 10 ns | 18.93 ns | **18 ns** | 180 ns | ~20 | ~20 | ~20 | ~20 |

`hist_total = 31,698,767,275` (3.170e10) @ **52.83 M iters/s** over 600 s
(percentiles: 1-ns view ~20 ns; log2 hist upper bound ≤32 ns)

**Isolation counters:** context_switches=0, need_resched_samples=0, user_escape_count=0,
arch_ipi_count=0, arch_irq_count=1, arch_tlb_count=0, arch_tlb_shootdown_targets=0,
arch_reschedule_deferred=2, arch_call_function_deferred=1, workqueue_queued=1, migration=0

**1-ns histogram (non-zero bins):**

| ns | count | % |
|----|-------|---|
| 10 | 3,397,601,243 | 10.7 % |
| 20 | 28,301,155,230 | 89.3 % |
| 30 | 10,791 | ~0.00003 % |
| 40 | 3 | — |
| 60 | 2 | — |
| 70 | 5 | — |
| 180 | 1 | — |

Weighted 1-ns sum = 599,999,441,530 ns ≈ duration (600e9 ns).

---

## Scenario B — rt-tests `oslat` (CPU 2, non-owned, FIFO 99)

- **Invocation:** `taskset -c 0-11 oslat -c 2 -w no -D 600 -W 1 -f 99 -z -q`
- **Live class:** SCHED_FIFO rtprio 99.

| min | avg | max | p50 | p90 | p99 | p999 |
|-----|-----|-----|-----|-----|-----|------|
| 19 ns | 22 ns | 6854 ns | ~20 | ~30 | ~30 | ~30 |

Total ≈ **27,536,949,244** (2.754e10, sum of 1-ns histogram) @ **45.93 M iters/s** over 599.51 s

**1-ns histogram (key bins):**

| ns | count | notes |
|----|-------|-------|
| 20 | 22,697,209,479 | 82.4 % |
| 30 | 4,839,147,520 | 17.6 % |
| 40 | 6 | |
| 90 | 2 | |
| 100 | 9 | |
| 860 | 1 | |
| ~1450 | ≈ 590,138 | **housekeeping-IPI cluster (~21 ppm)** |
| ~1850 | ≈ 1,538 | secondary cluster |
| ~2350 | ≈ 545 | secondary cluster |
| … | scattered | up to max 6854 ns |

---

## Comparison

| metric | cpu-accel (CPU 3, owned, CFS) | oslat (CPU 2, non-owned, FIFO 99) |
|--------|-------------------------------|-----------------------------------|
| samples | 3.170e10 | 2.754e10 |
| iters/s | 52.83 M | 45.93 M |
| min | 10 ns | 19 ns |
| mean | 18.93 ns (1-ns) / **18 (module avg)** | 22 ns (oslat avg) |
| max | 180 ns | 6854 ns |
| p50 / p90 / p99 / p999 | ~20 / ~20 / ~20 / ~20 ns | ~20 / ~30 / ~30 / ~30 ns |
| >1 µs events | 0 | ~592 K (~21 ppm cluster + tail) |
| context switches / arch IPIs / arch IRQs | 0 / 0 / 1 | n/a |
| resched deferred / call-fn deferred | 2 / 1 | n/a |

---

## Verification — the fix is working as intended

1. **`avg_lateness_ns` is now correct.** Module `avg_lateness_ns = 18 ns` matches the 1-ns
   histogram mean (**18.93 ns**) at 3.17e10 samples. Before the ABI-21 fix (32-bit hist), the
   same 600 s run gave `avg ≈ 99-102 ns` (a ~7× wrap artifact) while the 1-ns mean still
   read ~18.
2. **64-bit hist holds at 3.17e10 samples.** The 20 ns bin alone is `28,301,155,230`
   (> 2^32 = 4.29e9 — would wrap in 32-bit) and is stored exactly as `__u64`;
   `hist_total = 31,698,767,275` is the true sum.
3. **Full-run integrity.** Weighted 1-ns sum = 599,999,441,530 ns ≈ duration (600e9 ns).

## Read-out

The accelerator-owned CPU (cpu-accel) and the non-owned FIFO CPU (oslat) show the same
~18-22 ns mean wakeup latency. The owned CPU is tighter (all percentiles ~20 ns, zero >1 µs
events, 0 arch IPIs, 0 context switches) and runs a leaner loop (52.8 vs 45.9 M iters/s);
oslat's tail is the ~1.45 µs housekeeping-IPI cluster (~21 ppm) that the isolated owned CPU
never sees. Data accuracy is confirmed: module avg (18 ns) == 1-ns histogram mean (18.93 ns)
at 3.17e10 samples.

---

## Appendix — raw output

Scenario A (cpu-accel) summary line (abridged to the relevant fields):

```
cpu=3 state=3 workload=7 duration_ns=600000000000 period_ns=1000000
min_lateness_ns=10 max_lateness_ns=180 last_lateness_ns=20
user_active_ns=600000923314 user_escape_count=0
context_switches=0 need_resched_samples=0 workqueue_queued=1 workqueue_executed=0
arch_irq_count=1 arch_ipi_count=0 arch_tlb_count=0 arch_tlb_shootdown_targets=0
arch_reschedule_deferred=2 arch_call_function_deferred=1 migration_detected=0
hist_total=31698767275 hist_p50_ns=32 hist_p90_ns=32 hist_p99_ns=32 hist_p999_ns=32
avg_lateness_ns=18
```

Scenario A histograms:

```
lateness_hist_bin=4 range_ns=[8,16)  count=3397601243
lateness_hist_bin=5 range_ns=[16,32) count=28301166021
lateness_hist_bin=6 range_ns=[32,64) count=5
lateness_hist_bin=7 range_ns=[64,128) count=5
lateness_hist_bin=8 range_ns=[128,256) count=1
linear_ns_hist_ns=10  count=3397601243
linear_ns_hist_ns=20  count=28301155230
linear_ns_hist_ns=30  count=10791
linear_ns_hist_ns=40  count=3
linear_ns_hist_ns=60  count=2
linear_ns_hist_ns=70  count=5
linear_ns_hist_ns=180 count=1
```

Scenario B (oslat) tail (summary):

```
oslat V 2.11   Core: 2   Counter Freq: 3703 (MHz)
  0.020 (us): 22697209479
  0.030 (us): 4839147520
  0.040 (us): 6
  ... (cluster at ~1.45 us: ~590138; ~1.85 us: ~1538; ~2.35 us: ~545) ...
  6.855 (us): 1
 32.000 (us): 0 (including overflows)
     Minimum:  0.019 (us)
     Average:  0.022 (us)
     Maximum:  6.854 (us)
    Duration: 599.510 (sec)
```

— AI-signed: opencode | model: radiance/qwen3.8-27b | effort: not specified |
contribution: authored this 2026-11-06 concurrent side-by-side run and report (ABI-21 verification).
