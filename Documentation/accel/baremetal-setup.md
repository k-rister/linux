# CPU accelerator baremetal setup guide

Date: 2026-10-30

This guide covers the minimal steps to collect baremetal OS latency
measurements using the `user-oslat` workload on the `CPU_ACCEL`
prototype.

## Kernel configuration

The kernel must be built with:

```
CONFIG_SMP=y
CONFIG_MMU=y
CONFIG_GENERIC_IRQ_MIGRATION=y
CONFIG_RCU_NOCB_CPU=y
CONFIG_NO_HZ_FULL=y
CONFIG_CPU_ACCEL=y          # or =m for module
```

On x86 with a local APIC (the common case), no additional config is
required. The driver is at `drivers/cpu_accel/`; the x86 backend is at
`arch/x86/kernel/accel.c`.

## Boot parameters

Select one online CPU as the accelerator target (call it `N`). The
boot command line must include:

```
isolcpus=managed_irq,N  nohz_full=N  rcu_nocbs=N  irqaffinity=<all-other-cpus>
```

Example for an 8-CPU system with CPU 2 as the target:

```
isolcpus=managed_irq,2  nohz_full=2  rcu_nocbs=2  irqaffinity=0-1,3-7
```

| parameter | purpose |
|-----------|---------|
| `isolcpus=managed_irq,N` | removes N from the managed-IRQ and no-HZ housekeeping masks |
| `nohz_full=N` | makes N tickless (no periodic scheduler tick) |
| `rcu_nocbs=N` | offloads RCU callback processing from N to housekeeping CPUs |
| `irqaffinity=...` | pins housekeeping IRQs away from N |

All four are required. The driver's x86 admission check
(`x86_cpu_accel_user_enter`) rejects a target that is not
RCU-offloaded with `-EOPNOTSUPP` and a `pr_warn` naming the missing
boot parameters.

The target CPU must not be CPU 0 (the driver rejects it in `CONFIG`).

## Loading the driver

If built as a module:

```
insmod drivers/cpu_accel/cpu_accel.ko
```

The device appears at `/dev/cpu_accel`.

## Running the measurement

Build the tool:

```
make -C tools/cpu_accel cpu-accelctl
```

Run a 5-second measurement at a 1 ms sample period:

```
./cpu-accelctl run --workload user-oslat --cpu 2 \
    --period-us 1000 --duration-ms 5000
```

The tool forks a child that re-execs itself as an isolated ring-3
worker. The worker enters the user image via IRET, polls `rdtsc` in a
tight `pause` loop, and measures the lateness of each 1 ms deadline.
On exit it reports:

- `samples` / `work_iterations` — number of deadlines hit
- `max_lateness_ns` / `min_lateness_ns` / `last_lateness_ns`
- `hist_total` and non-zero `lateness_hist_bin` entries
- `hist_p50_ns`, `hist_p90_ns`, `hist_p99_ns`, `hist_p999_ns`
  (upper-edge estimates from the log2 histogram)
- IRQ, softirq, workqueue, context-switch, and IPI counters
- `preempt_count_entry` / `preempt_count_exit` (should be 0 / 1)
- `migration_detected` (should be 0)

For a full stress-class matrix (baseline, 5-CPU load, vmstat, LRU,
SLUB, ring resize, call-function, IRQ work, timerlat), use the WCL
report script:

```
CPU_ACCELCTL=$PWD/cpu-accelctl CPU_ACCEL_WCL_CPU=2 \
sh tools/cpu_accel/test-wcl-report.sh
```

## Interpreting results

The body of the distribution (p50/p90) reflects the polling granularity
of the 1 ms period check (typically 8-16 ns). The tail above 16 ns is
sparse housekeeping activity landing on the isolated CPU. A max on the
order of tens of microseconds is normal for a bounded 5-second window.

The measurement is an **envelope**, not a hard bound. SMIs, unrelated
NMIs, and firmware interrupts can still hit the owner and inflate the
tail. The `irq_count`, `softirq_count`, and `arch_irq_count` counters
correlate with the tail samples.

## Caveats

- **TSC calibration**: `cycles_per_ns` is computed at startup by
  `cpu_accel_calibrate_cycles_per_ns()`. On variable-frequency CPUs
  (C-states, turbo) this may be inaccurate, scaling all lateness
  values.
- **THP**: `khugepaged`'s `lru_add_drain_all()` requests a generic
  owner stop on its scan period. The WCL report script sets THP to
  `never` during the matrix to keep it out of the window. For manual
  runs, set `never` in
  `/sys/kernel/mm/transparent_hugepage/enabled` before measuring.
- **Watchdog**: without `--persistent`, the default watchdog fires at
  the configured duration. Use `--persistent` for longer runs and
  `cpu-accelctl stop` to end them.
- **Single-CPU**: only one CPU can be owned at a time. The device
  serializes access through a single `mutex`.

## Verifying the setup

A quick sanity check before a real measurement:

```
# 1. Confirm the target CPU is RCU-offloaded
cat /sys/devices/system/cpu/cpu2/rcu_nocbs   # should print "2"

# 2. Confirm no managed IRQs land on the target
grep "^2:" /proc/interrupts | wc -l          # should be 0

# 3. Run a short measurement
./cpu-accelctl run --workload user-oslat --cpu 2 \
    --period-us 1000 --duration-ms 1000
```

If step 3 returns `-EOPNOTSUPP`, the kernel will print a `pr_warn`
naming the missing boot parameters.

— AI-signed: opencode | model: radiance/qwen3.8-27b | effort: not specified
