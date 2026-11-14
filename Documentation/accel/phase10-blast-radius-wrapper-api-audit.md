# Phase 10 blast-radius wrapper API audit

## Scope

This note records the line-level review of the three retained owner-aware
workqueue wrappers in commit `bc4b96e4d9ec`:

- `cpu_accel_flush_workqueue()`
- `cpu_accel_drain_workqueue()`
- `cpu_accel_destroy_workqueue()`

The review compares each wrapper with the ordinary workqueue operation it
replaces and identifies the remaining evidence needed before review closure.

## Implementation review

| Wrapper | Ordinary operation | Owner coordination | Completion boundary |
|---|---|---|---|
| `cpu_accel_flush_workqueue()` | `__flush_workqueue()` | Reserves all possible CPUs for a `WQ_PERCPU` queue and requests active owners to stop | One flush of all currently queued work |
| `cpu_accel_drain_workqueue()` | `drain_workqueue()` | Holds the same reservation across every chained-work flush round | Repeated flush until the queue is empty |
| `cpu_accel_destroy_workqueue()` | `__destroy_workqueue()` | Uses the internal `sync_wait` argument to reserve and drain before destruction | Workqueue teardown after its drain completes |

### `cpu_accel_flush_workqueue()`

The wrapper calls `workqueue_sync_wait_begin()` and then the ordinary
`__flush_workqueue()` path. If the queue is not `WQ_PERCPU`,
`workqueue_sync_wait_begin()` returns false and the call degenerates to the
ordinary flush. If the queue is per-CPU, the wrapper reserves all possible
CPUs, requests active owners to stop, performs the ordinary flush, and releases
the reservation afterward.

This preserves the original one-shot flush completion boundary. The additional
all-CPU reservation is broader than the workqueue’s allowed CPU mask, but it is
intentional: a rescuer may execute work away from the pool CPU, and the wrapper
must prevent a new owner from entering while that work is still pending.

### `cpu_accel_drain_workqueue()`

The wrapper calls `workqueue_sync_wait_begin()` and then the ordinary
`drain_workqueue()` path. The reservation remains held across every flush round
inside `drain_workqueue()`, so chained work retains the ordinary drain
semantics. As with flush, non-per-CPU queues take the ordinary path.

This preserves the original repeated-flush-until-empty boundary. The broad CPU
reservation is again deliberate to cover rescuer execution and prevent a new
owner from entering during any drain round.

### `cpu_accel_destroy_workqueue()`

The wrapper calls `__destroy_workqueue(wq, true)`. Inside that function, the
`sync_wait` argument causes the same reservation and owner-stop sequence to
surround the internal drain before the queue is destroyed. Non-per-CPU queues
continue through the ordinary destroy path.

This preserves the original destroy boundary: pending work is drained before
the workqueue is torn down, and delayed work that was never queued still
follows the ordinary `destroy_workqueue()` limitation and must be canceled by
the caller.

## Findings

No wrapper-level semantic defect was identified in this review:

- Each wrapper delegates to the ordinary workqueue operation.
- Each wrapper preserves the original completion boundary.
- Each wrapper only adds owner coordination for `WQ_PERCPU` queues.
- Each wrapper releases its reservation after the underlying operation.
- The disabled-configuration fallbacks in
  `include/linux/cpu_accel_workqueue.h` call the ordinary operations.
- The x86 owner-stop callback is nonblocking:
  `x86_cpu_accel_request_stop_owner_gen()` publishes the stop request under a
  raw spinlock and does not wait for the owner to exit.
- Whenever `workqueue_sync_wait_begin()` succeeds, the wrapper or destroy path
  calls `workqueue_sync_wait_end()` after the underlying operation.
- The underlying flush, drain, and destroy operations are void and have no
  early error return that could bypass reservation release.

The broad all-possible-CPU reservation is intentional and documented in the
kernel comments. It is wider than the target workqueue’s CPU mask, but it is
required to cover rescuer execution and prevent new owners from entering while
work remains pending.

## Remaining evidence

The following items are still required before the wrapper API contract can be
marked review-closed:

- A lock-order and control-flow review covering owner entry, owner exit,
  cancellation, teardown, and hotplug interactions.
- Targeted stress evidence for flush, drain, and destroy with active and
  exiting owners.
- Confirmation that no wrapper can sleep in an atomic context or deadlock with
  the owner exit handshake.
- Stress evidence that an owner exit cannot deadlock with a wrapper wait.

## Targeted harness

`tools/cpu_accel/test-sync-work-owner-stop.sh` now provides dedicated
`sync_workqueue_flush`, `sync_workqueue_drain`, and `sync_workqueue_destroy`
cases. Each case starts an active owner on the target CPU, triggers the
wrapper from a separate work CPU, and requires the owner to exit after the
stop request. The companion probe queues per-CPU work before invoking the
selected wrapper and records work execution through the existing trace events.

The probe object and script syntax have been validated. Runtime execution on
the target host is still required before this section can be closed.

## Status

Wrapper API review is **in progress**. The implementation-level semantic audit
is complete, but the concurrency, stress, and rollback evidence required by
`Documentation/accel/phase10-blast-radius-review-close-plan.md` is not yet
recorded.
