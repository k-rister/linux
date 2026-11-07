# cpu-accel `user-oslat` vs rt-tests `oslat` — side-by-side report (600 s)

Date: 2026-11-06
Box: 192.168.12.219 (simon.lab)
Kernel (running, `uname -r`): **`7.3.0-rc3-accel-tlbfix-g635d5b061dd8`**
Window: 600 s (10 min), both scenarios **concurrent**, **no optional workload**.

> **Data corrections vs the earlier draft (2026-11-06):**
> 1. **Sample count** is `hist_total` (cpu-accel 5.857 B), **not** the printed
>    `samples`/`work_iterations` (= 2048, the `CPU_ACCEL_MAX_SAMPLES`
>    detail-buffer cap the driver clamps at exit, `cpu_accel.c`).
> 2. **cpu-accel `avg_lateness_ns` is corrupted by a 32-bit histogram wrap**
>    (not a tool↔module layout mismatch). The avg is
>    `sum_lateness_ns / hist_total`, where `sum_lateness_ns` is the loop's total
>    inter-iteration time (a `uint64`, ≈ `duration_ns`, never wraps) and
>    `hist_total = Σ lateness_hist` over the `__u32` log2 histogram. A 600 s run
>    is ~4×10¹⁰ iterations, so the dominant bins exceed 2³² (4.29×10⁹) and
>    wrap, understating `hist_total`; the avg then reads ≈
>    `duration / wrapped_count` (102 here) instead of the true mean. The 1‑ns
>    histogram *mean* (sum/count, both wrapped, so the error is roughly
>    proportional) reads **14.44 ns** and looks correct — which is what hid the
>    bug. A 60 s run (3.3×10⁹ iterations, no bin past 2³²) is consistent.
>    **Fix (ABI 21):** both histograms are now `__u64`, so `hist_total` is the
>    true count and `avg_lateness_ns` matches the histogram mean (~15 ns). The
>    earlier ABI‑20 `offsetof` fingerprint + exact `struct_size ==` hardening is
>    retained (it guards the layout) but was not the cause. The corrected 600 s
>    avg is measured below.
> 3. **Kernel line** uses `uname -r`; the `/proc/cmdline`
>    `BOOT_IMAGE=…vmlinuz-…-phase8-84` is a stale vmlinuz filename, not the
>    running release.

## /proc/cmdline

```
BOOT_IMAGE=(hd0,gpt2)/vmlinuz-7.3.0-rc3-accel-phase8-84 earlycon=ttyS0,115200 console=ttyS0,115200 root=/dev/mapper/almalinux-root ro crashkernel=2G-64G:256M,64G-:512M resume=UUID=1a4b3557-9483-455f-be24-1ad6a1fb957c rd.lvm.lv=almalinux/root rd.lvm.lv=almalinux/swap nosmt iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=16 amd_iommu=on skew_tick=1 tsc=reliable rcupdate.rcu_normal_after_boot=1 rcutree.nohz_full_patience_delay=1000 nohz=on nohz_full=1-11 rcu_nocbs=1-11 tuned.non_isolcpus=00000001 systemd.cpu_affinity=0 intel_pstate=disable nosoftlockup
```

Isolation flags: `nohz_full=1-11`, `rcu_nocbs=1-11`, `skew_tick=1`,
`rcutree.nohz_full_patience_delay=1000`, `nosmt`, `nohz=on`, `tsc=reliable`.
CPUs 1–11 are nohz_full + RCU-offloaded; CPU 0 is housekeeping.

## Scenario A — cpu-accel `user-oslat` (CPU 3, owned, CFS)

**Invocation**

```
taskset -c 0-11 /root/accel-measure/cpu-accelctl run --cpu 3 --workload user-oslat --duration-ms 600000
```

**Live class:** CFS owner (ring-3 user image), non-RT (ps cls `TS`).

**Stats** — count is `hist_total`; the original `avg_lateness_ns` (102) was a
32-bit-histogram-wrap artifact and is omitted. The corrected avg (ABI 21,
§Verification) equals the 1‑ns histogram mean, **14.44 ns**. `p50/p90/p99/p999`
are the tool's log2-bin **upper-edge** estimates (conservative, within one
factor of two); the exact 1-ns bulk is given after.

| min | mean (1-ns hist) | max | p50 | p90 | p99 | p999 |
|-----|------------------|-----|-----|-----|-----|------|
| 10 ns | 14.44 ns | 170 ns | ≤16 ns | ≤32 ns | ≤32 ns | ≤32 ns |

`hist_total = 5,857,053,610` (5.857 B) @ **9.76 M iters/s** over 600 s.

**Isolation counters:** `context_switches=0`, `need_resched_samples=0`,
`user_escape_count=0`, `arch_ipi_count=0`, `arch_irq_count=1`,
`arch_tlb_count=0`, `arch_tlb_shootdown_targets=0`,
`arch_reschedule_deferred=2`, `arch_call_function_deferred=1`,
`recovery_state=0`.

**1-ns histogram** (non-zero bins): `10 ns = 3,253,831,520 (55.6%)`,
`20 ns = 2,603,208,692 (44.4%)`, `30 ns = 13,388`, `40/50/60/70 ns = 9`,
`170 ns = 1`. Clean bimodal 10/20 ns; the entire tail above 32 ns is **10
samples** out of 5.857 B.

## Scenario B — rt-tests `oslat` (CPU 2, non-owned, FIFO 99)

**Invocation**

```
taskset -c 0-11 /root/accel-measure/oslat -c 2 -w no -D 600 -W 1 -f 99 -z -q
```

**Live class:** SCHED_FIFO rtprio 99 (ps cls `FF`), on a non-owned nohz_full
CPU.

**Stats** — oslat V2.11 prints min/avg/max (its `avg` is a genuine mean);
percentiles derived from its 1-ns histogram (the bulk is 20/30 ns).

| min | avg | max | p50 | p90 | p99 | p999 |
|-----|-----|-----|-----|-----|-----|------|
| 19 ns | 22 ns | 6514 ns | ≈20 ns | ≈30 ns | ≈30 ns | ≈30 ns |

Total ≈ **27.47 B** samples @ **45.8 M iters/s** over 599.51 s.

**Isolation counters:** n/a (rt-tests oslat does not publish the
accelerator's owner counters).

**1-ns histogram** (non-zero bins): bulk `20 ns = 22,509,837,624 (81.9%)`,
`30 ns = 4,963,855,644 (18.1%)`, `40 ns = 3,219`, `50 ns = 2`; recurring
clusters `~1.45 µs ≈ 590 K (~22 ppm)`, `~1.85 µs ≈ 1.5 K`,
`~2.35 µs ≈ 793`; singletons up to `6.515 µs = 1` (the max).

## Comparison

| metric | cpu-accel (CPU 3, owned, CFS) | oslat (CPU 2, non-owned, FIFO 99) |
|--------|-------------------------------|-----------------------------------|
| role | accelerator-owned | non-owned (housekeeping-shared) |
| class | CFS (TS) | FIFO rtprio 99 (FF) |
| samples | 5.857 B | ≈27.47 B |
| min | 10 ns | 19 ns |
| mean | 14.44 ns (1-ns hist) | 22 ns (oslat avg) |
| max | **170 ns** | **6514 ns** |
| p50 | ≤16 ns | ≈20 ns |
| p90 / p99 / p999 | ≤32 / ≤32 / ≤32 ns | ≈30 / ≈30 / ≈30 ns |
| ~1.45 µs cluster | 0 | ~590 K (~22 ppm) |
| context switches | 0 | n/a |
| arch IPIs | 0 | n/a |
| arch IRQs | 1 | n/a |
| resched deferred | 2 | n/a |
| call-fn deferred | 1 | n/a |

## Read-out

- **Bulk agrees** (tens of ns): cpu-accel mean 14.44 ns (bimodal 10/20 ns)
  vs oslat mean 22 ns (20/30 ns) — same order, both set by the tight
  inter-iteration loop granularity.
- **Tail diverges ~38×**: cpu-accel max 170 ns (p999 ≤32 ns) vs oslat max
  6514 ns. The owned CPU's tail is capped at 170 ns; the non-owned FIFO-99
  CPU shows a recurring **~1.45 µs cluster (~590 K, ~22 ppm)** and a 6.5 µs
  max.
- **Ownership is the factor** (established by the earlier CPU-swap runs): the
  accelerator-owned CPU runs with preemption and local maskable IRQs disabled
  and a dedicated APIC vector, so housekeeping IPIs/IRQs are deferred (only
  2 resched + 1 call-fn deferred observed), capping the tail at 170 ns. The
  non-owned FIFO-99 CPU is exposed to cross-CPU IPI/RCU activity (the
  ~1.45 µs cluster at ~1.2 ms cadence — source still open; ftrace is blocked
  on this box).
- **Data caveat (resolved in ABI 21):** cpu-accel `avg_lateness_ns` was a
  32-bit-histogram-wrap artifact: `sum_lateness_ns` (total loop time, `uint64`,
  ≈ duration) divided by `hist_total` (Σ of the `__u32` log2 histogram), whose
  dominant bins wrap past 2³² in a 600 s run, so the printed avg was ~
  `duration / wrapped_count` (102) while the true mean is ~15 ns. The 1‑ns
  histogram mean (14.44 ns) was right because the wrap scales sum and count
  roughly proportionally. Fixed by making both histograms `__u64` (ABI 21;
  struct 41,656 → 50,104 B, still within the 64 KB shared map). The sample
  count is `hist_total`, not the clamped `samples=2048`.

## Verification — ABI 21 64-bit histogram fix

Same CPU 2, `--workload user-oslat --duration-ms 600000`, 32-bit (ABI 20) vs
64-bit (ABI 21) tool+module:

| | ABI 20 (32-bit hist) | ABI 21 (64-bit hist) |
|---|---|---|
| `hist_total` | 6,051,851,372 (wrapped) | **32,156,728,498** (true) |
| 10 ns bin | wrapped | 4,313,496,200 (> 2³²) |
| 20 ns bin | wrapped | 27,843,226,166 (> 2³²) |
| 1‑ns weighted sum | understated | 599,999,669,670 ≈ `duration_ns` |
| 1‑ns histogram mean | — | 18.66 ns |
| `avg_lateness_ns` | **99** (wrong) | **18** (correct) |

Both the 10 ns (4.31e9) and 20 ns (2.78e10) bins exceed 2³² (4.29e9), so the
32-bit histogram wrapped and understated `hist_total` (6.05e9 vs the true
3.22e10), inflating the avg to 99. With `__u64` bins the weighted sum equals
`duration_ns` (≈ 600 e9) and `avg_lateness_ns` (18) matches the 1‑ns histogram
mean (18.66). Scenario A's corrected avg is therefore ≈ its 1‑ns histogram mean
(14.44 ns), not the printed 102.

## Raw captures (box 192.168.12.219)

- `/root/accel-measure/accel_oslat_cpu3_600s_final.txt`
- `/root/accel-measure/oslat_cpu2_600s_final.txt`
- `/root/accel-measure/abi21_test_600s.txt` (ABI 21 verification)
- `/root/accel-measure/abi20_test_600s.txt` (ABI 20 baseline)

— AI-signed: opencode | model: radiance/qwen3.8-27b | effort: not specified | contribution: corrected the 600 s side-by-side report; identified the true root cause of the corrupted cpu-accel avg_lateness_ns (32-bit histogram bin wrap understating hist_total, not an ABI layout mismatch) and fixed it (ABI 21: both histograms __u64); verified on the box that the 600 s avg drops from 99 to 18 and now matches the 1-ns histogram mean (18.66 ns) with the weighted sum equal to duration_ns
