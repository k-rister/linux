# Phase 10 blast-radius IRQ affinity audit

## Scope

This note records the semantic review of the IRQ-affinity files in
`Documentation/accel/phase10-blast-radius-semantic-inventory.tsv`:

- `kernel/irq/Makefile`
- `kernel/irq/accel.c`
- `kernel/irq/chip.c`
- `kernel/irq/manage.c`

The review baseline is `40288c9206c1` on
`codex/accelerator-blast-radius-v2`.

## Method

The review compared the baseline-to-current diff with:

- The public declarations and disabled-config fallbacks in
  `include/linux/irq_accel.h`.
- The complete quarantine, migration, effective-route validation, rollback,
  restore, and release control flow in `kernel/irq/accel.c`.
- Managed and unmanaged IRQ startup in `kernel/irq/chip.c`.
- Public, internal, forced, and default affinity selection in
  `kernel/irq/manage.c`.
- The accelerator admission, worker-exit, domain-failure, and domain-release
  call sites in `drivers/cpu_accel/cpu_accel.c`.
- The focused IRQ KUnit cases in `kernel/irq/accel_test.c`.

## Per-file verdicts

| File | Verdict | Review result |
|---|---|---|
| `kernel/irq/Makefile` | pass | `accel.o` is built only with `CONFIG_CPU_ACCEL_CORE`, and the test object is separately gated by `CONFIG_IRQ_KUNIT_TEST`. No production object is added when the accelerator core is disabled. |
| `kernel/irq/accel.c` | pass | CPU reservation uses an atomic compare-and-exchange. Quarantine reserves the CPU before scanning, fails closed for blocked or unmovable IRQs, validates the effective route after migration, drains in-flight handlers, and retains rollback state when cleanup is incomplete. Saved-affinity restoration now filters other accelerator-reserved CPUs so one worker cannot direct an IRQ to another worker that may still be active. |
| `kernel/irq/chip.c` | pass | Normal startup forces affinity selection before chip startup when the requested mask contains a reserved CPU. Managed startup remains shut down when no unreserved target exists and is restarted by `irq_accel_release_cpu()`. The original force warning remains limited to masks with no online CPU. |
| `kernel/irq/manage.c` | pass | Public affinity changes, internal chip programming, forced affinity, and default affinity selection all exclude reserved CPUs. Forced restoration remains available only to the internal rollback path, which now supplies a filtered mask. |

## Invariants verified

- **State transitions and lifetime:** a CPU is reserved before quarantine
  inspection. A successful quarantine returns retained state; an incomplete
  rollback retains both state and reservation for retry. Full restoration
  releases the reservation and frees state only after every saved affinity has
  been restored.
- **Lock ordering and deadlock avoidance:** quarantine holds the sparse-IRQ
  mutex outside each descriptor lock. The affinity callback runs on the CPU
  that currently owns the vector, completes any pending move, and takes only
  the descriptor lock. No remote callback acquires the sparse-IRQ mutex.
- **Error and rollback:** allocation failure, no destination CPU, a blocked
  IRQ, or an incomplete effective-route migration restores previously moved
  IRQs and releases the reservation when rollback succeeds. If rollback is
  incomplete, state and reservation are retained and the error is propagated.
- **Hotplug and owner exit:** callers hold the CPU-hotplug read lock across
  quarantine. Worker restore occurs after that worker exits or admission
  fails. Other reserved CPUs are removed from restored masks because they may
  belong to workers that are still active.
- **No unsafe owned-CPU execution:** migration callbacks run before owner
  entry or after owner exit. Other active owners are excluded from rollback
  destinations, and `synchronize_irq()` drains handlers that were in flight
  before quarantine completed.
- **No silent semantic change:** with `CONFIG_CPU_ACCEL_CORE=n`, the public
  fallbacks are no-ops and all IRQ-core behavior is unchanged. With the core
  enabled, changed behavior is limited to CPUs that are accelerator-reserved.

## Findings

Two defects were found and fixed during this audit:

- Forced quarantine rollback used the complete saved affinity mask. In a
  multi-worker domain, that mask could contain another accelerator-reserved
  CPU and direct an IRQ to a worker that was still active. Rollback now keeps
  only the restoring worker’s CPU and unreserved CPUs, and a focused KUnit
  case covers the mask invariant.
- The accelerator driver relied on a transitive include for
  `task_stack_page()`. The KUnit configuration exposed this as a compile
  failure; the driver now includes `linux/sched/task_stack.h` directly.

No semantic defect remains in this class.

## Evidence

- The complete baseline-to-current diff for all four files was reviewed.
- `scripts/checkpatch.pl --strict --no-signoff --no-tree` reports no errors,
  warnings, or checks for the touched source diff.
- `git diff --check` passes.
- An out-of-tree x86-64 build with `CONFIG_CPU_ACCEL=y` and
  `CONFIG_IRQ_KUNIT_TEST=y` compiles `kernel/irq/accel.o`,
  `kernel/irq/accel_test.o`, and `drivers/cpu_accel/cpu_accel.o`.
- The focused x86-64 KUnit kernel builds, but test execution is
  environment-gated because `qemu-system-x86_64` is not installed. This is
  recorded as a runtime limitation, not a test pass.

## Status

The IRQ-affinity semantic class is **pass**.  No follow-up defect remains for
these four semantic-remainder files; the KUnit runtime execution remains an
environment-gated validation item.
