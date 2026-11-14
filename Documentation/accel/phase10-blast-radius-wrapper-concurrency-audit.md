# Phase 10 wrapper concurrency audit

## Scope

This note records the static concurrency review of the three retained
owner-aware workqueue APIs:

- `cpu_accel_flush_workqueue()`
- `cpu_accel_drain_workqueue()`
- `cpu_accel_destroy_workqueue()`

The review covers owner-active behavior, sleep-context requirements, lock
order, owner-exit handshakes, cancellation, teardown, and hotplug races. It
complements the semantic comparison in
`Documentation/accel/phase10-blast-radius-wrapper-api-audit.md`.

## Method

The review followed the complete wrapper control flow in `kernel/workqueue.c`
and the x86 owner entry, stop-request, and exit paths in
`arch/x86/kernel/accel.c`. The symbol map was checked for every producer and
consumer of the `arch_smp_sync_wait_begin()`, `arch_smp_sync_wait_stop()`,
and `arch_smp_sync_wait_end()` handshake. No runtime result is claimed by
this note.

## Owner-active proof

`workqueue_sync_wait_begin()` performs two ordered phases for a `WQ_PERCPU`
workqueue:

1. It increments `sync_waiters` on every possible CPU.
2. It requests each currently active owner to stop.

The order matters. `x86_cpu_accel_owner_enter()` rejects entry when either
`sync_waiters` or `stop_inflight` is nonzero for the target CPU. Once phase
one has incremented all possible CPUs, no new owner can enter any CPU before
`workqueue_sync_wait_end()` releases the reservations. Consequently, phase two
observes every owner that can still be active while the wrapper waits.

The reservation deliberately covers all possible CPUs rather than only the
workqueue's allowed mask. A per-CPU work item can be executed by a reclaim
rescuer on another CPU, so restricting the reservation to the queue's nominal
pool CPUs would leave an admission window during which a new owner could
block work still covered by the flush or drain boundary.

For an already active owner, `arch_smp_sync_wait_stop()` snapshots that
owner's generation under the per-CPU request lock and calls
`x86_cpu_accel_request_stop_owner_gen()` with that generation. The generation
check prevents a stop request from being delivered to a later owner if the
original exits between the snapshot and request. If the owner has already
exited, the request is dropped because `active` is zero.

## Sleep-context proof

All three wrappers are sleep-only APIs:

- flush takes workqueue mutexes and waits for a flush completion.
- drain repeats that flush path until the queue is empty.
- destroy drains, stops a rescuer if present, and takes workqueue mutexes.

Each enabled implementation and each disabled-configuration fallback now
begins with `might_sleep()`. This makes an atomic-context caller detectable
with the kernel's sleep debugging instead of relying only on a later blocking
operation. There is intentionally no atomic-safe owner-aware wrapper: waiting
for a work completion while an owner exits can block.

The sleep assertion is placed before reservation. A misuse is therefore
reported before the wrappers change any per-CPU owner state.

## Lock order

The wrappers do not hold a workqueue lock while entering the owner handshake.
The only lock taken by `arch_smp_sync_wait_begin()` is no lock at all: it
atomically increments the per-CPU waiter count.

`arch_smp_sync_wait_stop()` uses this order:

1. Acquire and release the target CPU's `request->lock` to snapshot the active
   owner generation.
2. Call `x86_cpu_accel_request_stop_owner_gen()`.
3. That function acquires and releases `request->lock` to validate the
   generation and snapshot the nonblocking stop callback.
4. It invokes the callback only after releasing the lock.

The underlying workqueue flush, drain, or destroy operation starts only after
all reservations and stop requests have been issued. Therefore no path holds
`request->lock`, `x86_cpu_accel_ownership_lock`, or a workqueue mutex while
calling the owner stop callback.

## Owner-exit handshake

The owner-exit path cannot deadlock with the wrapper for three reasons:

- The stop callback contract is nonblocking. It publishes a stop request and
  returns; it must not wait for owner exit or TLB acknowledgment.
- `x86_cpu_accel_request_stop_owner_gen()` releases `request->lock` before
  invoking the callback, and `x86_cpu_accel_owner_exit()` waits for
  `stop_inflight` only after it has released that lock.
- The callback runs on the wrapper caller's CPU while the exiting owner runs
  on the target CPU. An active owner cannot simultaneously be the caller of
  its own wrapper wait.

If the stop request and owner exit race, the request generation and `active`
check select one of two safe outcomes. If the request wins, the callback runs
and `stop_inflight` is decremented before the exiting owner completes its
callback-storage wait. If exit wins, `active` is already zero and no stale
request is sent.

`x86_cpu_accel_owner_exit_finish()` publishes owner-exit completion and drops
the active count only after TLB reconciliation and subscription completion.
The wrappers retain their reservations until the underlying workqueue
operation has returned, so no new owner can enter between that completion
boundary and reservation release.

## Cancellation and teardown

The flush and drain wrappers have no error return and no early exit between
reservation begin and end. Their underlying operations are void, so a rollback
path cannot bypass `workqueue_sync_wait_end()`.

The destroy path marks the workqueue destroying, reserves possible CPUs,
drains, and releases the reservation before performing the remaining ordinary
teardown. Its busy-pool warning path returns only after
`workqueue_sync_wait_end()`, so that path cannot leak a reservation. After
drain returns, no work covered by the owner-aware destroy boundary remains,
and later teardown does not need to wait for another owner.

Non-pending delayed work retains the ordinary `destroy_workqueue()` contract:
callers must cancel it before destroy. The wrapper does not broaden that
guarantee.

## Hotplug

The handshake iterates possible CPUs, not merely online CPUs. Reservation
state is permanent per-CPU storage, so CPU offlining cannot destroy it. New
owner admission checks CPU online state and the reservation count under the
ownership lock; an offline CPU cannot acquire a new owner, and an online CPU
cannot acquire one while its reservation is held.

If a wrapper races hotplug maintenance, neither side holds a workqueue mutex
or the global ownership lock across the other side's blocking wait. Both may
request active owners to stop; the stop callbacks remain nonblocking and
generation-checked. The hotplug initiator waits for owner exit, while the
wrapper waits for ordinary work completion; those conditions can progress
independently.

## Findings

No concurrency defect was identified:

- All possible CPUs are reserved before the workqueue operation begins.
- An active owner on any possible CPU is generation-checked and asked to stop.
- New owner admission is closed for the full completion boundary.
- Stop callbacks are invoked without request or workqueue locks held.
- Owner exit waits for stop callbacks only after releasing the request lock.
- All wrapper paths release reservations before returning.
- All wrappers are explicitly sleep-only in enabled and disabled builds.

## Remaining evidence

Static closure is complete. Runtime stress execution for all six owner-active
and owner-exit cases remains pending and is tracked by the regression section
of `Documentation/accel/phase10-blast-radius-review-close-plan.md`.

## Status

Wrapper concurrency review is **statically closed**. Runtime stress evidence
is still required before full review closure.
