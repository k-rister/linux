# CPU accelerator Phase 8 handoff

Date: 2026-10-28  
Repository: `/home/krister/github/linux`  
Branch: `codex/accelerator-oslat`

## Status

Phase 8, adversarial isolation validation, remains open. The audit still has
unclassified activity classes, incomplete fault-injection and event-counter
coverage, and no run-specific worst-case latency report. The disposition and
exit criteria are in `cpu-accel.rst`.

The latest pushed commit is `c73189d085cd` (`Documentation: accel: clarify
sync callback ordering`). It records an important ordering gap for waited
per-CPU callbacks: `smp_call_function_single()` queues its callback before the
x86 sync-wait hook requests accelerator-owner exit. Syscall entry enables
interrupts before dispatch, so that callback can run before
`CPU_ACCEL_IOC_USER_EXIT` finishes TLB reconciliation and owner exit. A waited
callback therefore does not prove that an operation such as the Intel P-state
MSR update ran after ownership ended. A stronger owner-exit-before-callback
handshake is still required.

The watchdog-admission case has one bounded passing run on
`7.3.0-rc3-accel-tlbfix-02141-g1c9eb2e109a0`. Frequency, idle, thermal, and
other power-policy behavior remains unclassified or untested.

## Worktree

Two uncommitted files contain a more discriminating IRQ-work ordering probe:

- `tools/cpu_accel/tlb-flush-probe/cpu_accel_call_function_single_probe.c`
  checks `x86_cpu_accel_any_active()` from inside the IRQ-work callback and
  reports the callback count after the synchronous wait.
- `tools/cpu_accel/test-call-function-single-owner-stop.sh` requires the
  callback to complete and the callback-time owner-active count to remain zero.

The probe built successfully against the local kernel source, and the shell
script syntax check plus `git diff --check` passed. The updated files were
copied to `/tmp/cpu-accel-irq-work-test-20261028/` on the VM. **The VM test was
not run**: automated review rejected the proposed kernel stress action while
the Phase 8 goal was paused. Do not bypass that rejection through another
execution path.

## Probe review (resumed session, 2026-10-28)

The pending probe changes were reviewed against the x86 owner lifecycle and
the documented sync-callback gap. The review settled the test semantics; the changes are
ready for a driver-sized commit.

**Predicate.** `x86_cpu_accel_any_active()` reads
`x86_cpu_accel_active_count`. The count is incremented only at owner entry
(`x86_cpu_accel_owner_enter`, `arch/x86/kernel/accel.c`) and decremented only
in `x86_cpu_accel_owner_exit_finish()`, which both exit paths
(`x86_cpu_accel_direct_owner_exit()`, `x86_cpu_accel_user_exit()`) invoke
strictly after `x86_cpu_accel_tlb_reconcile()`. Observing an active count from
the IRQ-work callback therefore proves that at least one owner's exit
sequence, including TLB reconciliation, had not completed when the callback
ran.

**Attribution precondition.** The count is global, not per-CPU. The harness
runs a single ring-3 owner on `target_cpu` and nothing else in the harness
holds ownership; the test plan requires that no other accelerator owner is
active on the VM during the run so a nonzero reading can be attributed to the
target CPU's exit.

**Ordering.** For the probe's path, `smp_call_function_single()` maps to
`__smp_call_function_single()` in `kernel/smp.c`, which queues the CSD via
`generic_exec_single()` and only then calls
`arch_smp_sync_wait_stop()`, which requests owner exit. The queue-before-stop
ordering stated in the handoff is confirmed in source for this path.

**Happens-before.** The CSD callback must complete before
`smp_call_function_single()` returns, and the IRQ-work callback must complete
before `irq_work_sync()` in `cpu_accel_irq_work_probe_stop()`, which runs
after the synchronous call returns. If the IRQ-work callback observes an
active owner, the CSD callback, which completed earlier, also ran before the
owner's exit completed. A positive probe reading therefore confirms the
documented gap for the waited call-function path itself, not only for local
IRQ work.

**Settled test semantics.** A harness pass (including
`irq_work_callback_owner_active_count=0`) means no deferred IRQ-work callback
in that run observed an incomplete owner exit. A nonzero
`irq_work_callback_owner_active_count` in the `post_sync` line is the intended
diagnostic result: it is recorded as evidence confirming the ordering gap and
triggers the owner-exit-before-callback handshake design. Precondition
failures, timeouts, and kernel-error matches remain genuine test failures,
not gap evidence.

**Verdict.** The probe and its harness checks are sound under the
single-owner precondition; the vermagic check in the harness enforces
handoff step 2 (booted-kernel match) automatically.

## Next steps

1. [Done] Review the probe’s ownership predicate and expected callback
   ordering against the current x86 owner lifecycle and the documented gap.
   See the review section above; the settled changes are committed.
2. When work resumes, establish the VM’s current booted kernel before relying
   on the copied probe. The last task notes report kernel
   `7.3.0-rc3-accel-tlbfix-02141-g1c9eb2e109a0`; a console recovery excerpt
   elsewhere in the session reported `02104`, so verify with `uname -r`.
3. Obtain a reviewable, non-disruptive test plan that clears the test-action
   gate, then run the focused probe and record whether its callback observed
   an active owner. If it does, treat that as confirmation of the ordering
   gap and design the required handshake before broadening stress coverage.
4. Keep changes driver-sized and update the Phase 8 matrix and evidence in
   `Documentation/accel/cpu-accel.rst` as each disposition is established.

Do not commit or push the two current probe changes until they have been
reviewed and their intended test semantics are settled.
