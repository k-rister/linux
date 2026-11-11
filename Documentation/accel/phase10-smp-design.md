# CPU accelerator Phase 10 SMP design

Date: 2026-11-11
Branch: `codex/accelerator-oslat`
Status: implemented and validated on Simon

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
IPI. Bounded SPSC queues and static pipeline stages are a later Phase 10 stage
and must not be mixed into the first validation gate.

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
   tests. The control tool exposes a test-only one-worker failure selector for
   the sealed `user-oslat` image.
6. Add the Simon topology audit and a multi-CPU freeze gate.
7. Only after independent workers pass, add bounded SPSC queues and static
   pipeline stages.

## Validation gate

Phase 10 is complete only when all of the following are true on the target host:

* every selected CPU enters and exits as part of the same domain transaction;
* no partial activation, partial stop, or mixed Linux/accelerator state is
  observable;
* each independent `user-oslat` worker reports the requested workload, samples,
  histogram, Linux final mode, and terminal result;
* every worker reports zero context switches, zero migration, and zero user
  escapes;
* one worker fault or forced stop terminates the entire domain and all CPUs
  return to Linux without a host reboot;
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

SPSC queue work is complete only when every queue has a fixed capacity,
cache-line ownership, an explicit full/empty policy, and a memory-ordering
argument. Queue coordination must use shared-memory polling, not routine IPIs.
