# CPU accelerator Phase 10 SMP design

Date: 2026-11-11
Branch: `codex/accelerator-oslat`
Status: independent SMP stage and bounded SPSC static pipeline implemented and
validated on Simon

## Objective

Extend the singleton `/dev/cpu_accel` administrative domain from one accelerator
CPU to a setup-time set of accelerator CPUs. Phase 10 deliberately provides a
static multi-worker execution model, not an accelerator scheduler.

ABI 23 and ABI 24 remain compatibility floors. Their commands, structures,
encoded sizes, and `cpu_accel_shared` layout must not change. SMP support is an
experimental ABI 25 extension using new commands and new argument objects.

## Execution model

A Phase 10 domain contains one fixed worker for every selected CPU. The CPU set
is supplied while the domain is idle and is immutable while any worker is
active. Changing the set requires a terminal result, `RESET`, and a new domain
configuration.

The first execution stage has independent workers. There is no cross-worker
communication, no shared writable workload state, and no routine accelerator
IPI. Bounded SPSC queues and static pipeline stages are an additional Phase 10
stage and must not replace the independent-worker validation gate.

Admission requires distinct online CPUs, the boot CPU to remain in Linux, a
usable Linux housekeeping CPU outside the domain, and each selected CPU to pass
the existing architecture and isolation admission checks. The complete physical
core, including every SMT sibling, must belong to the same mode. The initial
implementation also requires all workers to be on one NUMA node.

## Lifecycle

Domain activation is transactional:

1. Validate every selected CPU and prepare every worker.
2. Reserve workqueue and IRQ resources for every selected CPU.
3. Enter every worker.
4. Publish `RUNNING` only after every worker reports active.

If any step fails before all workers are active, the driver stops or unwinds
every worker it started, restores Linux ownership for every selected CPU, and
publishes `ERROR`. A partially active or mixed Linux/accelerator domain is not
an observable success state.

Normal stop and failure are also domain-wide. A stop request is visible to every
worker. A worker fault, watchdog expiry, or emergency stop fails the whole
domain. Per-worker diagnostics remain available, but the domain does not
continue with a subset of workers. The aggregate terminal state is published
only after every worker is terminal and every CPU has been restored to Linux.

Manager death follows the existing singleton-domain semantics: release requests
the domain lifecycle exit and waits for all workers to reach a terminal state
before resources are released.

A protected user image reaches `CPU_ACCEL_IOC_USER_EXIT` with preemption
disabled and interrupts masked.  The ioctl dispatcher therefore handles that
command before the normal sleeping domain-mutex path and acquires the mutex with
non-sleeping try-lock retries.  Manager status or stop polling can no longer
schedule the owner before terminal telemetry is captured.

## ABI 25 extension

ABI 25 adds a domain configuration object and separate worker objects. It does
not grow `cpu_accel_config`, `cpu_accel_shared`, or any ABI 23/24 object.

The domain object supplies the CPU list and common workload parameters. For
kernel-backed workloads, the manager configures the domain and starts all
workers. For user workloads, each worker process receives the descriptor, is
pinned to its selected CPU, and joins the domain with its own image metadata.
Each user worker then enters from its own syscall context. This preserves the
existing rule that a protected user image is admitted from the task and memory
map that will execute it; the manager never pins or redirects another process's
address space.

Recovery uses a new ABI 25 worker-indexed escape command. The ABI 23 escape
command remains worker-0-only and retains its frozen encoding.

Per-worker state, timestamps, counters, histograms, and terminal records are
reported through a separate ABI 25 mapping. The legacy shared mapping remains
the authoritative ABI 23/24 control surface and reports a domain summary when
ABI 25 is selected.

For a user workload, the manager creates one attached workload region for each
worker. Each worker maps only its corresponding region and per-worker telemetry
mapping. Workers do not share writable workload state in the first SMP stage.

The bounded pipeline stage adds a second user region type. It uses
`CPU_ACCEL_IOC_DOMAIN_QUEUE_CONFIG` (`0x16`) after domain configuration and
before start. The command is valid only for `USER_PIPELINE`, requires a linear
topology, and accepts exactly `cpu_count - 1` queues. Worker `i` is the
producer for queue `i` and the consumer for queue `i - 1`; consequently, worker
0 only produces, the final worker only consumes, and every interior worker does
both. The first implementation supports 2 to 16 workers and at most 15 queues.

Pipeline queue regions use `CPU_ACCEL_REGION_TYPE_SPSC_QUEUE`. The region type
occupies the 32-bit field that was reserved in the experimental ABI 25 region
creation object; `CPU_ACCEL_REGION_TYPE_WORKLOAD` is zero and remains the
default for all non-pipeline regions. Queue capacities are uniform per domain,
must be powers of two, and range from 1 to 1024 slots. Queue admission requires
an attached, bidirectionally mapped region, one manager mapping, no owner
mappings, a valid queue header, and a unique region for every queue index.

The fixed queue layout is 256 bytes followed by capacity slots:

1. immutable header;
2. producer-owned 64-byte cache line;
3. consumer-owned 64-byte cache line;
4. 64-byte alignment pad;
5. 16-byte `{sequence, value}` slots beginning at offset 256.

Each queue has exactly two owner mappings after workers join: the producer and
the consumer. A worker's sealed VMA therefore contains its workload region,
its telemetry mapping, and only the queue regions for its expected input and
output edges. Both endpoints publish readiness bits before joining and wait for
both bits, ensuring queue admission observes the final owner mappings.
The readiness word also carries a producer-done bit. A producer release-sets
that bit only after its final enqueue. A consumer acquire-loads it while the
queue is empty, drains every already-published record, and then terminates.
Interior workers propagate the done bit to their output queue after draining
their input queue.

An ABI 25 SDK that finds only ABI 24 may fall back to ABI 24 for ordinary
single-CPU operation. If the caller explicitly requests an SMP domain, the SDK
must fail with a clear unsupported-version error rather than silently reducing
the request to one CPU.

## First implementation stages

1. Refactor the single worker image and observation into an array of worker
   contexts while preserving the legacy one-CPU path.
2. Add experimental ABI 25 domain configuration, worker join, worker escape,
   per-worker telemetry, and domain summary ioctls.
3. Implement transactional kernel-backed multi-CPU activation and rollback.
4. Implement user-worker join and per-worker entry for concurrent independent
   `user-oslat` workers.
5. Add domain-wide stop, fault propagation, manager-death release, and reset
   tests. The control tool exposes a test-only one-worker forced-stop selector
   that invokes the ABI 25 worker escape command after the domain is running.
   Sealed workloads do not perform failure injection with `exit_group()`.
6. Add the Simon topology audit and a multi-CPU freeze gate.
7. Only after independent workers pass, add bounded SPSC queues and static
   pipeline stages.

## Bounded queue contract

The queue is full when `producer.head - consumer.tail == capacity` and empty
when both indices are equal. Producers spin without overwriting a full slot;
consumers spin without consuming an empty slot. Workloads run for the requested
duration and then drain naturally rather than discarding queued records. The
producer-done bit terminates that drain only after the queue is empty.

The producer writes the slot, then release-stores `producer.head`. The consumer
acquire-loads `producer.head`, reads the slot, and release-stores
`consumer.tail`. The producer acquire-loads `consumer.tail` before reusing a
slot. The producer release-sets the done bit after its final enqueue, and the
consumer acquire-loads that bit while the queue is empty. These pairs publish
slot contents, publish consumption, terminate drain, and preserve release/
acquire ordering for slot reuse. Queue coordination uses shared-memory polling
only; the driver does not issue routine accelerator IPIs.

## Validation gate

Phase 10 is complete only when all of the following are true on the target host:

* every selected CPU enters and exits as part of the same domain transaction;
* no partial activation, partial stop, or mixed Linux/accelerator state is
  observable;
* each independent `user-oslat` worker reports the requested workload, samples,
  histogram, Linux final mode, and terminal result;
* a healthy `user-pipeline` run reports every worker as successful, exactly
  `worker_count - 1` queues, matching producer and consumer checksums, equal
  enqueue and dequeue counts, zero final backlog, and at least one observed
  full or empty event;
* healthy-run workers report zero context switches, zero migration, and zero
  user escapes; the selected forced-stop worker reports exactly one escape and
  zero context switches and migrations;
* one worker fault or forced stop terminates the entire domain and all CPUs
  return to Linux without a host reboot; the forced-stop gate requires the
  selected worker to report one escape, the other workers to report zero
  escapes, and the domain to finish in Linux mode;
* manager crash, descriptor movement, `execve()`, and fork behavior remain
  correct;
* ABI 23 and ABI 24 compatibility gates still pass;
* SMP latency tails and cross-worker contention are reported against the
  declared 2x single-CPU review threshold;
* module refcount returns to zero and accelerator dmesg contains no errors.

## Simon validation record

On 2026-11-11, Simon ran kernel
`7.3.0-rc3-accel-tlbfix-gecf563755ca7` with the `cpu-partitioning` tuned
profile unchanged. CPUs 5 and 7 were selected for the SMP domain; the observed
Linux housekeeping mask remained separate from the target CPUs.

The full Phase 10 gate passed with CPUs 5 and 7, a 1000 ms duration, and a
1000 us period. The run covered independent `user-oslat` workers, topology
rejection, transactional activation rollback, manager death, one-worker failure
propagation, and final Linux-mode reset.

A separate healthy-run capture reported:

* worker 0 on CPU 5: `COMPLETE`, result 0, 51,914,184 samples, p50/p99 32 ns,
  zero context switches, zero migration, and zero user escapes;
* worker 1 on CPU 7: `COMPLETE`, result 0, 51,914,065 samples, p50/p99 32 ns,
  zero context switches, zero migration, and zero user escapes.

An ABI 24 single-CPU baseline on CPU 5 completed with 54,651,155 samples,
p50/p90/p99/p999 32 ns, and maximum lateness 110 ns. Both SMP workers
therefore reported a 32 ns p99, below the 64 ns 2x review threshold. Each
worker produced 95.0% of the single-CPU sample count, showing no material
cross-worker contention in the independent-worker stage.

The intentional one-worker failure capture reported worker 0 on CPU 5 as
`COMPLETE` with result 0 and worker 1 on CPU 7 as `ERROR` with result -3. The
domain returned to Linux mode and reset successfully without a host reboot.
After the final gates, the module refcount was zero, no `cpu_accel` worker
threads remained, the failure-injection parameter was reset to `UINT_MAX`, and
dmesg contained no accelerator errors after the final module load.

The bounded SPSC static-pipeline stage was then validated with the manager-side
ABI 25 worker escape and the non-sleeping `USER_EXIT` lock path.  The full
Phase 10 gate passed on CPUs 5, 7, 9, and 11 with a 1000 ms duration and 1000 us
period.  It covered independent `user-oslat` workers, the bounded
`user-pipeline`, topology rejection, transactional activation rollback, manager
death, one-worker forced stop, and final Linux-mode reset.

In the captured four-CPU independent-worker run, workers on CPUs 5, 7, 9, and 11
produced 51,283,259, 51,283,218, 51,283,213, and 51,270,875 samples
respectively.  Each reported p50/p99 32 ns, maximum lateness of 110, 270, 230,
and 280 ns, zero context switches, zero migration, and zero user escapes.

In the captured four-CPU pipeline run, all three capacity-one queues reported
2,652,145 enqueued and dequeued records, zero final backlog, matching producer
and consumer checksums of 238,693,050, and nonzero full or empty pressure.
Workers on CPUs 5, 7, 9, and 11 reported maximum lateness of 1,910, 760, 2,320,
and 2,790 ns, zero context switches, zero migration, and zero user escapes.

The forced-stop gate requires the selected worker to finish as `ESCAPED` with
result 0 and one user escape, while every other worker finishes as `COMPLETE`
with result 0 and no escape.  The corrected four-CPU gate passed those checks,
returned the domain to Linux mode, and reset it without a host reboot.

The Phase 9 ABI 23/24 freeze gate also passed on CPU 5 with the same module.
After the final gates, module refcount was zero, no `cpu_accel` worker threads
remained, and dmesg contained no `BUG`, `WARNING`, `Oops`,
`scheduling while atomic`, or accelerator error.  The EDS4100 serial captures
for the final two-CPU Phase 10, four-CPU Phase 10, and Phase 9 gates contained
only their explicit start and end console markers.
