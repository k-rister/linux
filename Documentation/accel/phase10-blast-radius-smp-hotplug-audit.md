# Phase 10 blast-radius SMP and hotplug audit

## Scope

This note records the semantic review of the SMP, stop-machine, CPU
online/offline, and freeze/thaw files in
`Documentation/accel/phase10-blast-radius-semantic-inventory.tsv`:

- `include/linux/smp.h`
- `kernel/smp.c`
- `kernel/cpu.c`
- `kernel/stop_machine.c`

The review baseline is `40288c9206c1` on `codex/accelerator-blast-radius-v2`.

## Method

The review compared the baseline-to-current diff with:

- The x86 implementation of `arch_smp_sync_wait_begin()`, `stop()`, and `end()`.
- The owner-entry checks for sync waiters, maintenance state, and TLB flushes.
- The complete `_cpu_up()`, `_cpu_down()`, `freeze_secondary_cpus()`, and
  `thaw_secondary_cpus()` control flow.
- The only current caller of `stop_machine_from_inactive_cpu()`.
- Prior freeze/thaw and stop-machine lock-order validation recorded in the
  project history.

## Per-file verdicts

| File | Verdict | Review result |
|---|---|---|
| `include/linux/smp.h` | pass | The three architecture hooks are declared only under `CONFIG_CPU_ACCEL_CORE` and become empty inline functions otherwise. Their documented begin/stop/end ordering matches every call site. |
| `kernel/smp.c` | pass | Synchronous single, many, and `smp_call_on_cpu()` paths reserve the target before queueing, request an active owner to stop without waiting inside the hook, and release the reservation after completion. CSD-lock reuse is handled without introducing a blocking operation in atomic context. |
| `kernel/cpu.c` | pass | CPU down uses drain-mode maintenance before the hotplug write lock; CPU up uses the inactive-CPU-compatible maintenance window. Freeze/thaw holds a terminate-mode gate across the entire suspend window and releases it exactly once, including the empty-frozen-CPU and early-failure paths. |
| `kernel/stop_machine.c` | pass | Ordinary `stop_machine()` drains owners before taking the CPU-hotplug read lock. The inactive-CPU variant uses a nonblocking try-window and returns `-EBUSY` when owner drainage is not already possible; its only current caller propagates that error and rolls back its CPU mask update. |

## Invariants verified

- **State transitions and lifetime:** every successful `begin()` is paired with
  `end()` on the same control-flow path; `_cpu_up()` and `_cpu_down()` nest
  correctly inside the broader freeze/thaw maintenance window.
- **Lock ordering and deadlock avoidance:** owner maintenance is opened before
  CPU-hotplug locks. The inactive-CPU path never sleeps and uses a try-only
  reservation. Synchronous SMP hooks only increment wait counts and publish
  stop requests.
- **Error and rollback:** `_cpu_up()` releases its maintenance window on every
  `out` path. `stop_machine_from_inactive_cpu()` returns `-EBUSY` before
  modifying stop-machine state when the try-window fails. The sole caller,
  `cache_ap_online()`, propagates the error and clears its CPU mask bit.
- **Hotplug and owner exit:** CPU-down drains bounded owners and escalates only
  past their deadline; CPU-up closes admission before the inactive rendezvous.
  Freeze/thaw prevents the retained primary CPU from acquiring an owner during
  suspend.
- **No unsafe owned-CPU execution:** all-CPU stop-machine paths use terminate
  mode. Synchronous per-CPU calls request owner exit before waiting on the
  target CPU.
- **No silent semantic change:** with `CONFIG_CPU_ACCEL_CORE=n`, the SMP hooks
  are no-ops and the hotplug/stop-machine paths are semantically unchanged.

## Findings

No semantic defect remains in this class.  The earlier freeze/thaw issue—an
unmatched maintenance release on an early hibernate-resume failure—is fixed by
the `x86_cpu_accel_freeze_maintenance` gate and release-once logic in
`thaw_secondary_cpus()`.

## Evidence

- The semantic inventory contains exactly four rows for this class, and all
  four now have `pass` verdicts.
- The complete baseline-to-current diff for all four files was reviewed.
- Prior project evidence records a passing x86 freeze/thaw object build and a
  stop-machine lock-order test with owner stop and exit trace ordering.

## Status

The SMP, stop-machine, CPU online/offline, and freeze/thaw semantic class is
**pass**.  No follow-up defect remains for these four semantic-remainder files.
