# Phase 10 blast-radius include and small-call-site audit

## Scope

This note records the semantic review of all 14 remaining rows in
`Documentation/accel/phase10-blast-radius-semantic-inventory.tsv`:

- `fs/smb/server/transport_ipc.c`
- `include/asm-generic/tlb.h`
- `include/linux/arm_ffa.h`
- `include/linux/mhi_ep.h`
- `include/linux/mm_types.h`
- `include/linux/mm_types_task.h`
- `kernel/exit.c`
- `kernel/fork.c`
- `kernel/rcu/srcutree.c`
- `kernel/rcu/tree.c`
- `kernel/rcu/tree_nocb.h`
- `kernel/time/timer_migration.c`
- `net/core/dev.c`
- `samples/ftrace/sample-trace-array.c`

The review baseline is `40288c9206c1` on
`codex/accelerator-blast-radius-v2`. The class contains header-state additions,
small producer/teardown repairs, accelerator admission predicates, and
per-CPU work rendezvous call sites; it is not a mechanical wrapper conversion
class.

## Method

The review compared the complete baseline-to-current diff for each file with:

- The accelerator admission, maintenance-window, and per-CPU sync-wait
  contracts in `arch/x86/kernel/accel.c`.
- The retained workqueue wrapper contracts in `kernel/workqueue.c`.
- The MM execution-owner registry and lock order in `mm/owner.c`.
- The x86 TLB-generation and batched-unmap completion paths in
  `arch/x86/mm/tlb.c` and `mm/mmu_gather.c`.
- Each subsystem's producer, callback, timer, IRQ, and teardown lifecycle.

## Per-file verdicts

| File | Verdict | Review result |
|---|---|---|
| `fs/smb/server/transport_ipc.c` | pass | Soft reset publishes `SERVER_STATE_RESETTING` while holding `startup_lock`, then releases the lock before canceling the delayed heartbeat work. A startup event that acquires the lock during cancellation observes the reset state and cannot rearm the timer, while the heartbeat callback can still acquire the lock and complete. |
| `include/asm-generic/tlb.h` | pass | The x86-only `mmu_gather` completion fields are initialized in `__tlb_gather_mmu()` and consumed only by the x86 gather-completion paths. Non-x86 builds and no-gather x86 builds retain the original generic TLB semantics. |
| `include/linux/arm_ffa.h` | pass | The notifier relinquish contract now states that the callback registration is removed and the notification unbound, but an invocation that already copied the callback out is not awaited. The caller must keep callback data alive through that in-flight invocation. |
| `include/linux/mhi_ep.h` | pass | The added `unregistering` and `powering_down` flags are initialized by controller registration and gate IRQ processing, reset-worker restart, and power-up during teardown. They do not alter the normal powered-up state machine. |
| `include/linux/mm_types.h` | pass | The `mm_struct` execution-owner lock, list, and update-depth fields support the non-task owner registry. Dynamic initialization in `fork.c` and every static `mm_struct` initializer supply the lock and list state. |
| `include/linux/mm_types_task.h` | pass | The clarified `arch_tlbbatch_flush()` contract preserves the original requirement that no CPU may use a stale translation after return; it makes the lazy-TLB exception explicit as “unable to use the affected mm until an equivalent flush completes.” |
| `kernel/exit.c` | pass | The config-gated early exit tracepoint is emitted after `PF_EXITING` is set and before the owner can sleep in the later exit path. The additional `sched_process_exit` export permits the accelerator module to register its later death callback. |
| `kernel/fork.c` | pass | `mm_init()` initializes the execution-owner lock, list, and update depth before the mm can be published or used. The initialization is paired with the static-mm initializers reviewed elsewhere. |
| `kernel/rcu/srcutree.c` | pass | `cleanup_srcu_struct()` brackets its per-CPU callback-work flush with the architecture sync-wait hooks only when that work is pending or executing. The original cleanup, barrier, and leak-on-error semantics are unchanged. |
| `kernel/rcu/tree.c` | pass | `rcu_current_cpu_is_offloaded()` exports the existing per-CPU NOCB state with an explicit preempt-disabled contract. The only current caller is accelerator admission, which holds preemption disabled across the check. |
| `kernel/rcu/tree_nocb.h` | pass | NOCB GP and callback kthreads are created before being bound to the RCU housekeeping mask. The original wake/park and priority setup are preserved, while callback execution is kept away from CPUs isolated from kernel noise. |
| `kernel/time/timer_migration.c` | pass | Timer-migration isolate/unisolate work is queued on the selected CPU inside a sync-wait reservation and flushed before the reservation is released. The two-phase available/unavailable transition and cpumask construction are unchanged. |
| `net/core/dev.c` | pass | Per-CPU backlog flush work is queued and flushed inside the same sync-wait reservation. The original high-priority workqueue, flush mask, and fallback allocation semantics are preserved. |
| `samples/ftrace/sample-trace-array.c` | pass | Replacing `timer_delete()` with `timer_delete_sync()` ensures the timer callback has completed before `cancel_work_sync()` waits for the work it may queue. This closes the sample's timer-to-work teardown race. |

## Invariants verified

- **State transitions and lifetime:** the KSMBD reset state is published before
  cancellation, MHI teardown flags cover the IRQ/reset-worker window, the FFA
  callback-lifetime rule is explicit, and static/dynamic `mm_struct`
  initializers cover the execution-owner registry.
- **Lock ordering and deadlock avoidance:** KSMBD no longer waits for delayed
  work while holding `startup_lock`; the MM registry documents
  `owner->lock` before `mm->execution_owner_lock`; and NOCB kthread creation
  preserves the original GP/CB relationship.
- **Error and rollback:** accelerator-disabled builds retain no-op sync-wait
  and owner-update helpers; SRCU cleanup still leaks on invalid caller state;
  and the sample teardown order prevents a timer callback from racing work
  cancellation.
- **Hotplug and owner exit:** timer migration, network backlog flush, and SRCU
  cleanup reserve the exact target CPU before waiting for per-CPU work.
- **Accelerator ownership:** all synchronous per-CPU work waits use the
  architecture sync-wait begin/stop/end sequence around the original queue and
  flush operations.
- **No silent semantic change:** the MM and TLB changes preserve the original
  completion boundaries, the RCU admission helper only exposes existing state,
  and the KSMBD/MHI/sample changes repair producer/teardown races rather than
  narrowing completion scope.

## Findings

One documentation defect was found and fixed in this change:

1. **Incorrect Arm FFA relinquish contract:** the original comment claimed that
   relinquish waits for in-flight callback invocations and may sleep. The
   implementation removes the notifier entry under `notify_lock`, performs an
   atomic SMCCC unbind, and does not wait for a callback already copied out of
   the table. The comment now requires the caller to keep callback data alive
   through any in-flight invocation.

No unresolved semantic defect remains in the 14-file population.

## Evidence

- The complete 14-file baseline-to-current diff was reviewed with 30 lines of
  context and indexed as ephemeral captures `cap_759` and `cap_760`.
- Supporting symbol and initializer searches were indexed as `cap_761` and
  `cap_762`.
- Focused validation for this class is recorded in the phase 10 review-closure
  plan after `git diff --check`, strict checkpatch, focused object builds, and
  the accelerator Sphinx build pass.

## Status

The include-path and small-call-site semantic class is **pass**. All 90
semantic-remainder rows now have final `pass` verdicts.
