# OSlat side-by-side — cpu-accel `user-oslat` (ABI 22) vs rt-tests `oslat` — 12 h run, 2026-11-08

Concurrent **12 h** comparison run to validate the ABI-22 workload-agnostic shared-region
implementation and confirm that the ABI-21 64-bit histogram correction remains correct after
the region-layout change.

- **Box:** `192.168.12.219` (simon.lab)
- **Kernel:** `7.3.0-rc3-accel-tlbfix-gd16a5ac600cc-dirty #88 SMP PREEMPT_DYNAMIC`
- **Module:** `cpu_accel`, ABI 22
- **Tool:** `cpu-accelctl` (the controller name at run time; later renamed to
  `cpu-accel-testctl`)
- **Run:** both scenarios concurrent, wall 2026-11-08 15:19:54 CST →
  2026-11-09 03:19:55 CST; both workloads exited 0, the module refcount returned to 0,
  and `dmesg` reported no accelerator errors
- **Isolation:** CPUs 1-11 `nohz_full` + `rcu_nocbs` + `skew_tick=1`; CPU 0 housekeeping;
  SMT off
- **cmdline (full `/proc/cmdline`):** `BOOT_IMAGE=(hd0,gpt2)/vmlinuz-7.3.0-rc3-accel-phase8-85 earlycon=ttyS0,115200 console=ttyS0,115200 root=/dev/mapper/almalinux-root ro crashkernel=2G-64G:256M,64G-:512M resume=UUID=1a4b3557-9483-455f-be24-1ad6a1fb957c rd.lvm.lv=almalinux/root rd.lvm.lv=almalinux/swap nosmt iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=16 amd_iommu=on skew_tick=1 tsc=reliable rcupdate.rcu_normal_after_boot=1 rcutree.nohz_full_patience_delay=1000 nohz=on nohz_full=1-11 rcu_nocbs=1-11 tuned.non_isolcpus=00000001 systemd.cpu_affinity=0 intel_pstate=disable nosoftlockup`

---

## Scenario A — cpu-accel `user-oslat` (CPU 3, owned, CFS)

- **Invocation:** `./cpu-accelctl run --cpu 3 --duration-ms 43200000 --period-us 1000 --workload user-oslat`
- **Live class:** CFS owner (ring-3 user image), CPU 3, nice 0
- **ABI-22 region state:** `region_count=1`, `region_set_epoch=11`

| min | mean (1-ns hist) | avg (module) | max | p50 / p90 / p99 / p999 |
|-----|------------------|--------------|-----|-------------------------|
| 10 ns | 20.051 ns | **20 ns** | 170 ns | 32 / 32 / 32 / 32 ns |

The percentile fields are tool-reported from the log2 histogram; the dominant 1-ns bins are
20 and 30 ns.

`hist_total = samples = 2,154,499,048,202` (2.154e12) @ **49.873 M iters/s** over exactly
43200 s.

**Isolation counters (full 12 h):** context_switches=0, need_resched_samples=0,
user_escape_count=0, arch_ipi_count=0, arch_irq_count=1, arch_tlb_count=0,
arch_tlb_shootdown_targets=0, arch_reschedule_deferred=2, arch_call_function_deferred=1,
workqueue_queued=1, workqueue_executed=0, migration=0, recovery_state=0.

**1-ns histogram (non-zero bins):**

| ns | count | % |
|----|-------|---|
| 10 | 220,250,275 | 0.010223 % |
| 20 | 2,143,069,531,820 | 99.469505 % |
| 30 | 11,209,266,093 | 0.520273 % |
| 40 | 5 | — |
| 50 | 1 | — |
| 60 | 2 | — |
| 70 | 5 | — |
| 170 | 1 | — |

Weighted 1-ns sum = 43,199,871,122,830 ns, within 0.129 s of the requested 43200 s.
`user_active_ns=43,200,000,935,017`, about 0.935 ms above the requested duration, showing
that the owner remained active through the deadline and stopped cleanly.

---

## Scenario B — rt-tests `oslat` (CPU 2, non-owned, FIFO 99)

- **Invocation:** `./oslat -c 2 -w no -D 43200 -W 1 -f 99 -z -q --json=abi22_rt_side_by_side_12h_20261108T2119Z.oslat.json`
- **Live class:** SCHED_FIFO rtprio 99, CPU 2

| min | avg | max | p50 | p90 | p99 | p999 |
|-----|-----|-----|-----|-----|-----|------|
| 19 ns | 21.826 ns | 645,036 ns | ~20 ns | ~30 ns | ~30 ns | ~30 ns |

Total = **1,979,301,472,733** samples @ **45.854 M iters/s** over 43164.871 s as reported
by rt-tests.

**1-ns histogram (key bins):**

| ns | count | notes |
|----|-------|-------|
| 20 | 1,624,045,783,662 | 82.04 % |
| 30 | 355,212,930,452 | 17.96 % |
| 40 | 41,963 | |
| 1300–1600 | 42,154,209 | dominant housekeeping-IPI cluster |
| ≥1000 | 42,710,410 | 21.58 ppm of all samples |
| ≥32000 | 51 | includes the 645.036 µs maximum |

The non-owned FIFO CPU again shows the recurring roughly 1.4–1.5 µs cluster. At 42.15
million events in 12 h, the count is approximately one half of the 86.1 million events
observed in the earlier 24 h ABI-21 run, consistent with linear scaling with duration.

---

## Comparison

| metric | cpu-accel (CPU 3, owned, CFS) | oslat (CPU 2, non-owned, FIFO 99) |
|--------|-------------------------------|-----------------------------------|
| samples | 2.154e12 | 1.979e12 |
| iters/s | 49.873 M | 45.854 M |
| min / mean / max | 10 / 20.051 (avg 20) / 170 ns | 19 / 21.826 / 645,036 ns |
| samples >1 µs | 0 | 42,710,410 |
| samples ≥32 µs | 0 | 51 |
| context switches / arch IPIs / arch IRQs | 0 / 0 / 1 | n/a |
| resched deferred / call-fn deferred | 2 / 1 | n/a |
| user escapes / migrations | 0 / 0 | n/a |

---

## Verification — ABI-22 shared regions and 64-bit counters

1. **`avg_lateness_ns` remains correct.** The module reports 20 ns while the weighted 1-ns
   histogram mean is 20.051 ns. The module uses integer division, so this is the expected
   truncated result.
2. **No histogram wrap at 2.154e12 samples.** The 20 ns bin alone contains
   2,143,069,531,820 samples, about **499× 2³²**, and `hist_total` exactly equals the
   sample count. This verifies that the ABI-21 correction carried into the ABI-22 explicit
   shared-region layout.
3. **The ABI-22 region path was active.** `region_count=1` and `region_set_epoch=11`
   confirm that user-oslat telemetry came from the workload-agnostic shared region rather
   than the removed fixed workload-data mapping.
4. **Run integrity is clean.** The weighted histogram sum covers effectively the entire
   requested duration, `user_active_ns` is within 1 ms of the deadline, the workload
   returned to Linux mode (`mode=0`), both commands exited 0, and the module refcount
   returned to 0.
5. **Owned-CPU isolation is stable for 12 h.** There were no context switches, arch IPIs,
   user escapes, or migrations; the worst observed sample was 170 ns.

## Read-out

ABI-22 has now passed a 12 h user-oslat validation at 2.154e12 samples. This is sufficient
to close the histogram-wrap concern for the current ABI: the dominant bin is hundreds of
times beyond the former 32-bit wrap point, and the total and mean remain internally
consistent. The owned CFS CPU was both faster in the bulk loop and dramatically tighter in
the tail than the non-owned FIFO-99 CPU. The non-owned CPU’s recurring housekeeping-IPI
cluster continued to scale approximately linearly with run duration.

---

## Appendix — raw-output provenance

The following local copies were used for this report:

| file | SHA-256 |
|------|---------|
| `/tmp/abi22_rt_side_by_side_12h_20261108T2119Z.accel.txt` | `3b1953bf823e13937262256f653619d8755c6900ff766363a1c447490f8dc86e` |
| `/tmp/abi22_rt_side_by_side_12h_20261108T2119Z.oslat.json` | `736ff90e087d039f7fb7a09f3583ab7e9a884cc2b9b1952a20b3b0b72a0adca5` |
| `/tmp/abi22_rt_side_by_side_12h_20261108T2119Z.oslat.txt` | `9206a5e5356123f42518334e88783510ab26450c57925a5d3f070b593890e64d` |

The original run files remain on simon.lab under `/root/accel-measure/` with the
`abi22_rt_side_by_side_12h_20261108T2119Z.*` prefix.

— AI-signed: Codex | model: rits/zai-org/glm-5-3 | effort: xhigh |
contribution: authored this ABI-22 12 h validation report from the completed run’s captured outputs.
