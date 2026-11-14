# Phase 10 blast-radius workqueue and scheduling audit

## Scope

This note records the semantic review of the workqueue-core and scheduling
boundary files in
`Documentation/accel/phase10-blast-radius-semantic-inventory.tsv`:

- `include/linux/workqueue.h`
- `kernel/sched/isolation.c`

The review baseline is `40288c9206c1` on `codex/accelerator-blast-radius-v2`.
`kernel/workqueue.c` is direct accelerator support and is already covered by
the Phase 10 wrapper API audit; it is intentionally excluded from the semantic
remainder population.

## Method

The review compared the baseline-to-current diff with:

- The public declarations and disabled-config fallbacks in
  `include/linux/cpu_accel_workqueue.h`.
- The owner reservation and selection behavior in `kernel/workqueue.c`.
- The architecture sync-wait hooks in `include/linux/smp.h`,
  `kernel/smp.c`, and `arch/x86/kernel/accel.c`.
- The complete `housekeeping_update()` control flow, including all early
  returns after the owner reservation begins.

## Per-file verdicts

| File | Verdict | Review result |
|---|---|---|
| `include/linux/workqueue.h` | pass | The added reservation API is declared in the ordinary workqueue header, while its disabled-config fallbacks remain in `linux/cpu_accel_workqueue.h`. The include does not introduce a cycle, and the only other change is whitespace alignment in `flush_scheduled_work()`. |
| `kernel/sched/isolation.c` | pass | `housekeeping_update()` brackets the new housekeeping-mask publication with `arch_smp_sync_wait_begin()`/`stop()`/`end()` on every requested CPU. No early return occurs after reservation begins, so the wait count is always released. Reservations begin before publication so an online race cannot admit an owner to a CPU being isolated. |

## Invariants verified

- **State transitions and lifetime:** the reservation count is incremented
  before the new mask is published and decremented only after all RCU,
  workqueue, timer-migration, and kthread updates complete.
- **Lock ordering and deadlock avoidance:** `arch_smp_sync_wait_begin()` is
  nonblocking and only increments the target’s wait count;
  `arch_smp_sync_wait_stop()` publishes a stop request without waiting for
  owner exit.
- **Error and rollback:** all failure paths before reservation return without
  changing the owner wait count. After reservation begins, the function has no
  early return and unconditionally reaches `arch_smp_sync_wait_end()`.
- **Hotplug and owner exit:** reserving an offline CPU is intentional. The x86
  owner-entry check rejects a CPU with a nonzero sync-wait count, so a CPU
  onlined during the mask update remains gated until the transition finishes.
- **No unsafe owned-CPU execution:** stopping owners before `synchronize_rcu()`
  prevents an active owner on a newly isolated CPU from blocking the grace
  period needed to publish the housekeeping change.
- **No silent semantic change:** with `CONFIG_CPU_ACCEL_CORE=n`, the added
  workqueue declarations resolve to no-op stubs and the scheduler hooks are
  empty inline functions.

## Findings

No semantic defect was identified in either file.

## Evidence

- The semantic inventory contains exactly two rows for this class, and both
  now have `pass` verdicts.
- The complete baseline-to-current diff for both files was reviewed.
- The accelerator documentation build and `git diff --check` pass on the
  x86-audit commit.

## Status

The workqueue-core and scheduling-boundary semantic class is **pass**.  No
follow-up defect remains for these two semantic-remainder files.
