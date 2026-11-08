# OSlat side-by-side — cpu-accel `user-oslat` (ABI 21) vs rt-tests `oslat` — 24 h run, 2026-11-07

Concurrent **24 h** comparison run, in the standard side-by-side format, to verify the
ABI-21 `avg_lateness_ns` fix (64-bit histograms) and owned-CPU isolation hold at full
scale (4.5×10¹² samples / day).

- **Box:** `192.168.12.219` (simon.lab)
- **Kernel:** `7.3.0-rc3-accel-tlbfix-g635d5b061dd8`
- **cmdline:** `nosmt skew_tick=1 rcutree.nohz_full_patience_delay=1000 nohz_full=1-11 rcu_nocbs=1-11`
- **Module:** `cpu_accel`, ABI 21, `srcversion 54621BD98AB74849843B188`
- **Tool:** `cpu-accelctl-abi21` (ABI-21)
- **Run:** both scenarios **concurrent**, 24 h each (cpu-accel `--persistent`), wall
  2026-11-06 20:46:20 → 2026-11-07 20:46 CST (2026-11-08 02:46:20 UTC start)
- **Isolation:** CPUs 1-11 `nohz_full` + `rcu_nocbs` + `skew_tick=1`; CPU 0 housekeeping; SMT off

---

## Scenario A — cpu-accel `user-oslat` (CPU 3, owned, CFS)

- **Invocation:** `taskset -c 0-11 cpu-accelctl-abi21 run --cpu 3 --workload user-oslat --duration-ms 86400000 --persistent`
- **Live class:** CFS owner (ring-3 user image); manager CFS, `--persistent` (watchdog = clean stop at the 24 h deadline).

| min | mean (1-ns hist) | avg (module) | max | p50 | p90 | p99 | p999 |
|-----|------------------|--------------|-----|-----|-----|-----|------|
| 10 ns | 18.99 ns | **18 ns** | 140 ns | ~20 | ~20 | ~20 | ~20 |

`hist_total = 4,550,915,536,535` (4.551e12) @ **52.67 M iters/s** over 86400 s
(percentiles: 1-ns view ~20 ns; log2 hist upper bound ≤32 ns)

**Isolation counters (full 24 h):** context_switches=0, need_resched_samples=0,
user_escape_count=0, arch_ipi_count=0, arch_irq_count=1, arch_tlb_count=0,
arch_tlb_shootdown_targets=0, arch_reschedule_deferred=2, arch_call_function_deferred=1,
workqueue_queued=1, migration=0

**1-ns histogram (non-zero bins):**

| ns | count | % |
|----|-------|---|
| 10 | 461,857,617,169 | 10.15 % |
| 20 | 4,089,057,902,332 | 89.85 % |
| 30 | 17,020 | ~4e-7 % |
| 40 | 6 | — |
| 60 | 1 | — |
| 70 | 6 | — |
| 140 | 1 | — |

Weighted 1-ns sum = 86,399,734,729,790 ns ≈ duration (86,400×10⁹ ns).

---

## Scenario B — rt-tests `oslat` (CPU 2, non-owned, FIFO 99)

- **Invocation:** `taskset -c 0-11 oslat -c 2 -w no -D 86400 -W 1 -f 99 -z -q`
- **Live class:** SCHED_FIFO rtprio 99.

| min | avg | max | p50 | p90 | p99 | p999 |
|-----|-----|-----|-----|-----|-----|------|
| 19 ns | 22 ns | 23181 ns | ~20 | ~30 | ~30 | ~30 |

Total ≈ **3.953e12** (sum of 1-ns histogram) @ **45.8 M iters/s** over 86329.7 s

**1-ns histogram (key bins):**

| ns | count | notes |
|----|-------|-------|
| 20 | 3,233,056,762,752 | 81.8 % |
| 30 | 720,344,679,499 | 18.2 % |
| 40 | 293 | |
| ~1450 | ≈ 86,100,000 | **housekeeping-IPI cluster (~21.8 ppm)** |
| ~1850 | ≈ 225,000 | secondary cluster |
| ~2350 | ≈ 78,000 | secondary cluster |
| … | scattered | up to max 23181 ns (single 23.18 µs event) |

---

## Comparison

| metric | cpu-accel (CPU 3, owned, CFS) | oslat (CPU 2, non-owned, FIFO 99) |
|--------|-------------------------------|-----------------------------------|
| samples | 4.551e12 | 3.953e12 |
| iters/s | 52.67 M | 45.8 M |
| min / mean / max | 10 / 18.99 (avg 18) / 140 ns | 19 / 22 / 23181 ns |
| p50 / p90 / p99 / p999 | ~20 / ~20 / ~20 / ~20 ns | ~20 / ~30 / ~30 / ~30 ns |
| >1 µs events | 0 | ~86 M (~21.8 ppm cluster + tail) |
| context switches / arch IPIs / arch IRQs | 0 / 0 / 1 | n/a |
| resched deferred / call-fn deferred | 2 / 1 | n/a |

---

## Verification — the fix holds at full 24 h scale

1. **`avg_lateness_ns` is still correct.** Module `avg_lateness_ns = 18 ns` matches the
   1-ns histogram mean (**18.99 ns**) at **4.551e12** samples. The ABI-21 fix holds over
   the entire 24 h run.
2. **No counter wrap at 4.55e12 samples.** The 20 ns bin alone is `4,089,057,902,332`
   (4.09e12, ~1000× 2^32) and is held exactly as `__u64`; `hist_total` is the true sum.
   64-bit headroom to overflow is ~10^4 years at this rate.
3. **Full-run integrity.** Weighted 1-ns sum = 86,399,734,729,790 ns ≈ duration
   (86,400×10⁹ ns); the loop ran the full 24 h.
4. **Owned-CPU isolation stable all day.** 0 context switches, 0 arch IPIs, 0 escapes,
   worst sample ≤140 ns across 4.55e12 iterations.

## Read-out

Same shape as the 10-min run, confirmed at scale: the accelerator-owned CPU (cpu-accel)
and the non-owned FIFO CPU (oslat) both average ~18-22 ns. The owned CPU stays perfectly
clean for 24 h (no IPIs/context switches, max 140 ns) and runs a leaner loop (52.67 vs
45.8 M iters/s); oslat's ~1.45 µs housekeeping-IPI cluster grew ~linearly with duration
(~86 M over 24 h vs ~590 K over 600 s, ≈ the 144× duration ratio) and its rarest
outlier reached 23.18 µs (a single event). Data accuracy is confirmed at 4.55e12 samples.

---

## Appendix — raw output

Scenario A (cpu-accel) summary line (abridged to the relevant fields):

```
cpu=3 state=3 workload=7 duration_ns=86400000000000 period_ns=1000000
min_lateness_ns=10 max_lateness_ns=140 last_lateness_ns=20
user_active_ns=86400000974954 user_escape_count=0
context_switches=0 need_resched_samples=0 workqueue_queued=1 workqueue_executed=0
arch_irq_count=1 arch_ipi_count=0 arch_tlb_count=0 arch_tlb_shootdown_targets=0
arch_reschedule_deferred=2 arch_call_function_deferred=1 migration_detected=0
hist_total=4550915536535 hist_p50_ns=32 hist_p90_ns=32 hist_p99_ns=32 hist_p999_ns=32
avg_lateness_ns=18
```

Scenario A histograms:

```
lateness_hist_bin=4 range_ns=[8,16)  count=461857617169
lateness_hist_bin=5 range_ns=[16,32) count=4089057919352
lateness_hist_bin=6 range_ns=[32,64) count=7
lateness_hist_bin=7 range_ns=[64,128) count=6
lateness_hist_bin=8 range_ns=[128,256) count=1
linear_ns_hist_ns=10  count=461857617169
linear_ns_hist_ns=20  count=4089057902332
linear_ns_hist_ns=30  count=17020
linear_ns_hist_ns=40  count=6
linear_ns_hist_ns=60  count=1
linear_ns_hist_ns=70  count=6
linear_ns_hist_ns=140 count=1
```

Scenario B (oslat) tail (summary):

```
oslat V 2.11   Core: 2   Counter Freq: 3703 (MHz)
  0.020 (us): 3233056762752
  0.030 (us): 720344679499
  0.040 (us): 293
  ... (cluster at ~1.45 us: ~86.1M; ~1.85 us: ~225K; ~2.35 us: ~78K) ...
 23.182 (us): 1
 32.000 (us): 0 (including overflows)
     Minimum:  0.019 (us)
     Average:  0.022 (us)
     Maximum:  23.181 (us)
     Max-Min:  23.162 (us)
    Duration: 86329.734 (sec)
```

— AI-signed: opencode | model: radiance/qwen3.8-27b | effort: not specified |
contribution: authored this 24 h concurrent side-by-side run and report (ABI-21 verification at scale).
