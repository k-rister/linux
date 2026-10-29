# Owner-exit-before-callback handshake design

Date: 2026-10-29
Branch: `codex/accelerator-oslat`
Status: design, verification recorded, pending implementation

## Problem

Phase 8 confirmed the sync-callback ordering gap on
`7.3.0-rc3-accel-tlbfix-02141-g1c9eb2e109a0`: one bounded run of
`test-call-function-single-owner-stop.sh irq-work` recorded
`irq_work_callback_owner_active_count=1` in the probe's post-sync line
(evidence in the frequency/idle/thermal/power row of the Phase 8 matrix in
`cpu-accel.rst`). A waited `smp_call_function_single()` to an owned CPU can
therefore return while the target owner's exit, including TLB reconciliation
and the `x86_cpu_accel_active_count` publication, has not completed. Every
waited per-CPU MSR policy write (intel_pstate, amd-pstate, p4-clockmod,
intel_epb, the user-facing MSR driver, and the rest audited in that row)
carries the same exposure.

## Current sequence

For a waited call to an owned target CPU, `__smp_call_function_single()` in
`kernel/smp.c` runs:

1. `arch_smp_sync_wait_begin(cpu)`: increments the per-CPU
   `sync_waiters` count. Owner entry rejects `-EBUSY` while
   `sync_waiters` is nonzero (`x86_cpu_accel_owner_enter`), so this reserves
   the CPU against new owners.
2. `generic_exec_single()`: queues the CSD and sends the IPI. The target is
   owned with interrupts masked, so the CSD sits pending.
3. `arch_smp_sync_wait_stop()`: if the CPU is active, requests owner exit
   (`x86_cpu_accel_request_stop_owner_gen`), which for a ring-3 user owner
   delivers the stop NMI.
4. The NMI escape unwinds the user image and restores Linux mode. The CPU
   re-enables interrupts; the pending CSD then runs and its completion
   returns to the caller.
5. The full exit bookkeeping, `x86_cpu_accel_user_exit()` (owner exit +
   `x86_cpu_accel_tlb_reconcile()` + `x86_cpu_accel_owner_exit_finish()`),
   runs later, from the char-device release path
   (`cpu_accel_user_image_release()` in `drivers/cpu_accel/cpu_accel.c`).
   `x86_cpu_accel_owner_exit_finish()` is where the active count drops and
   the `exiting` flag clears.

Steps 4 and 5 are the gap: the CSD (and anything that runs after the wait,
such as a local IRQ-work callback) observes the owner still marked active,
and the waited policy callback executes before TLB reconciliation for that
owner is done.

## Design goals

- A waited call-function callback to a reserved target CPU runs only after
  that owner's exit is fully complete: TLB reconciliation done,
  `x86_cpu_accel_owner_exit_finish()` done, active count dropped.
- No new blocking wait in `__smp_call_function_single()` with preemption
  disabled (`get_cpu()`), so the IPI path does not spin for an NMI escape.
- Non-waited (deferred) call-function and other deferral policies are
  unchanged.
- The reservation semantics of `sync_waiters` (entry `-EBUSY`) are retained.

## Candidate handshakes

### Option A: begin waits for full exit before queueing

`arch_smp_sync_wait_begin()` would, after taking the reservation, request
owner stop if the CPU is active and wait until that owner's exit fully
completes before `generic_exec_single()` queues the CSD.

- Closes the window for every waited caller in one place.
- Blocked as stated: the hook runs with preemption disabled
  (`get_cpu()`), so the wait cannot be a `wait_event`; a bounded
  `cpu_relax()` spin would hold the caller's CPU across an NMI escape and
  the exit, exactly the latency Phase 8 must not add. It would need a
  restructuring of `__smp_call_function_single()` around `get_cpu()`, which
  is a generic-code change with wide blast radius.
- Keep as a fallback only if the stop-context completion below proves
  unsafe.

### Option C: the stop flow completes the full exit (recommended)

Complete the owner's exit in the stop/escape context instead of deferring
it to the release path:

- When the stop NMI lands on an owned user CPU and the image escapes, the
  kernel-side stop path performs the remaining `x86_cpu_accel_user_exit()`
  steps immediately: `x86_cpu_accel_tlb_reconcile()` (local flushes inline)
  followed by `x86_cpu_accel_owner_exit_finish()`.
- The interrupt re-enable that lets the pending CSD run must be ordered
  after `x86_cpu_accel_owner_exit_finish()`. The exact re-enable point in
  the escape path must be located and asserted during implementation.
- The char-device release path then finds no active owner and becomes
  idempotent (it must still handle the never-stopped case).

This keeps the existing queue-before-stop order, adds no wait to the caller,
and closes the window for all consumers that can run once interrupts return,
including local IRQ work.

Remote TLB acknowledgements: `x86_cpu_accel_tlb_reconcile_and_ack()` can
issue a nested `smp_call_function_single()` when the reconcile is remote.
From NMI stop context that is not possible. The probe run showed the common
case is a no-op reconcile (`tlb_gen=0`, `unscoped=0`). The implementation
must therefore: (a) perform local flushes and subscription completions
inline in the stop context; (b) for the remote-ack case, defer only the
acknowledgement and finish, with the finish ordered before the CPU services
queued call-function work (for example, by holding the CSD drain behind the
ack completion while the reservation is held), or fall back to option A for
that case. The chosen treatment must be recorded with the implementation.

## Invariant

While `request->sync_waiters` is nonzero for a CPU, no waited
call-function callback queued after the reservation can execute on that CPU
before `x86_cpu_accel_owner_exit_finish()` for the prior owner has
completed; and no new owner can enter (`-EBUSY`) until the reservation is
released by `arch_smp_sync_wait_end()`.

## Verification before implementation

1. Locate the exact interrupt re-enable point on the NMI-escape return path
   (candidate: the `CPU_ACCEL_VECTOR` system-vector entry in
   `arch/x86/entry/entry_fred.c` and the escape handler in
   `arch/x86/kernel/accel.c`) and confirm it can be ordered after the exit
   finish without breaking the recovery contract.
2. Confirm the stop context (NMI) can call `x86_cpu_accel_tlb_reconcile()`
   for the local case without taking forbidden locks or sleeping.
3. Confirm the `x86_cpu_accel_owner_wait` wake path and
   `cpu_accel_user_image_release()` remain correct when the exit has
   already completed in the stop context (idempotency, no double
   `atomic_dec` of the active count).
4. Audit other consumers of the early IPI service window (none expected:
   the CPU is not doing useful work while owned).

## Implementation verification

Findings for the four verification items above (2026-10-29):

1. **Interrupt re-enable point.** The implementation does not order a
   single re-enable point; it completes the owner exit in the stop NMI
   handler itself (the driver's `cpu_accel_user_nmi`, NMI context on the
   owned CPU). While the CPU is in NMI context, and afterwards in user
   mode with the image's forced `IF=0`, no vector (call-function CSD,
   IRQ work, or any other IPI) can be serviced. The first interrupt
   re-enable after the escape - whether at the escape tail's syscall
   entry or at `local_irq_enable()` in the USER_EXIT handler - is
   therefore ordered after `x86_cpu_accel_owner_exit_finish()` by
   construction.
2. **NMI-safety of the local reconcile.** Verified against the current
   tree: `x86_cpu_accel_owner_exit()` takes only raw spinlocks and calls
   `mmu_owner_unregister()` (raw spinlocks plus `mmput_async()`),
   `__flush_tlb_all()` takes no locks,
   `mmu_owner_subscription_complete_all()` runs only atomics and
   `schedule_work()` (the production subscriber,
   `mmu_gather_completion_owner_ops`, is atomic-context safe), and
   `x86_cpu_accel_owner_exit_finish()` takes a raw spinlock, one
   `atomic_dec()`, and `wake_up_all()`. No blocking lock and no sleeping.
   The `stop_inflight` spin at the end of `x86_cpu_accel_owner_exit()`
   is bounded: the requester decrements `stop_inflight` immediately
   after the asynchronous NMI send, and the requester CPU spins in the
   call-function wait loop, so it always runs.
3. **Release-path idempotency.** `x86_cpu_accel_user_exit()` is already
   idempotent: `x86_cpu_accel_owner_exit()` publishes the exit with
   `atomic_xchg(&request->active, 0)` and
   `x86_cpu_accel_owner_exit_finish()` is a no-op when the exit did not
   happen, so the global active count is decremented exactly once. One
   telemetry correction is required: the release path overwrites
   `shared->arch_tlb_shootdown_targets` from a delta based on the
   return value of `x86_cpu_accel_user_exit()`, which reads zero when
   the stop-context exit already ran; the NMI handler captures the exit
   snapshot and the release path prefers it.
4. **Early service-window consumers.** None: the owned CPU performs no
   useful work, and the deferred reschedule/call-function IPIs that
   `x86_cpu_accel_owner_exit()` re-sends to the local CPU are serviced
   after the exit has completed.

## Implementation plan

1. `arch/x86/kernel/accel.c`: add exported
   `x86_cpu_accel_user_stop_exit(unsigned int cpu)`, running
   `x86_cpu_accel_owner_exit()`, then `x86_cpu_accel_tlb_reconcile()`
   (always the local branch: the stop target is the calling CPU, so no
   remote acknowledgement is needed and none can be issued from NMI),
   then `x86_cpu_accel_owner_exit_finish()`, returning the
   `tlb_shootdown_targets` snapshot like `x86_cpu_accel_user_exit()`.
   Assert the calling-CPU invariant with `WARN_ON_ONCE`. Add the
   non-LAPIC stub next to the `user_enter`/`user_exit` stubs in
   `arch/x86/include/asm/cpu_accel.h`.
2. `drivers/cpu_accel/cpu_accel.c`: `cpu_accel_user_nmi()` calls
   `x86_cpu_accel_user_stop_exit()` (after the drop check, before
   redirecting to the escape tail) and records the snapshot plus a done
   flag in `struct cpu_accel_device`. The release path uses the captured
   snapshot for `shared->arch_tlb_shootdown_targets` when the
   stop-context exit ran.
3. Build the candidate kernel, boot it on `accelerator-dev`, and run the
   test plan below; the `irq-work` probe must show
   `irq_work_callback_owner_active_count=0`.

## Test plan

1. Rebuild and boot the candidate kernel on `accelerator-dev`.
2. Re-run the bounded `irq-work` probe per
   `phase8-irq-work-probe-test-plan.md`: expect
   `irq_work_callback_owner_active_count=0` with PASS across the bounded
   retry set; any nonzero reading fails the handshake.
3. Re-run the `sync`, `smp-call-on-cpu`, and `work-on-cpu` harness modes
   (regression for the waited path).
4. Re-run the watchdog admission run and the perf owner-stop run on the
   candidate build.
5. Record all results with the kernel build in the Phase 8 matrix; close
   the row only when the handshake run evidence lands.

## Out of scope

- Stress loops, hotplug races, and other Phase 8 rows (separate plans).
- Changes to the deferred (non-waited) call-function policy.
