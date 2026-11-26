# CPU Accelerator: Leasing Linux CPUs for Deterministic Dataplanes

## Presentation contract

**Audience:** Software engineers experienced in real-time kernel development and
low-latency performance optimization.

**Format:** This is a two-tier presentation outline with speaker notes:

1. **Part 1** is a 5–10 minute executive summary intended for live delivery. It
   presents the post-blast-radius “final product” and compares it with the
   starting Linux execution model.
2. **Part 2** is a 15–20 minute or longer technical backing section. It is also
   written to be read directly by engineers who want the design journey,
   implementation details, audit results, and evidence without reading a full
   paper.

**Source state:** Code links are pinned to commit
`e4bcafcc86d44b6e8e800f54a9394dbc9ff507d`; the v7.3-final rebase and soak
ladder report is pinned to commit `cee53c1374a5`. Both commits are on
`codex/accelerator-v7.3-rebase`, and the report is
`Documentation/accel/v7.3-rebase-4cpu-user-oslat-and-rt-tests-oslat-2026-11-26.md`.

### Link policy

- Code references use commit-pinned GitHub links with line ranges.
- Documentation references use commit-pinned GitHub links when the referenced
  content is committed.
- The 24-hour report is linked to its result commit on GitHub.
- No credential, recovery secret, or untracked session-transcript content is
  included.

---

# Part 1 — 5–10 minute executive summary

## Slide 1 — Title and thesis

### On-slide content

**Leasing Linux CPUs for deterministic dataplanes**

Linux remains the control plane; selected CPUs execute a protected, statically
admitted dataplane without ordinary Linux scheduling or routine IPIs.

### Speaker notes

Open by positioning this as a single-OS design, not a hypervisor or second
kernel. Linux keeps the responsibilities it is good at: admission, lifecycle
control, resource accounting, observability, and recovery. The accelerator
domain receives a fixed CPU set, explicit memory mappings, and a sealed user
image. Once active, those CPUs do not run the ordinary scheduler, do not
migrate workers, and do not take routine reschedule or call-function IPIs.

The important claim is not that Linux scheduling is inherently bad. The claim
is that a deterministic dataplane benefits from a coarse-grained ownership
boundary: Linux leases whole CPUs, coordinates the kernel paths that can wait
for or interrupt those CPUs, and can recover them without rebooting the host.

**Time:** approximately 45 seconds.

## Slide 2 — The starting point and problem

### On-slide content

- Isolated CPUs reduce ordinary kernel noise, but do not remove it.
- Scheduler events, IPIs, workqueues, IRQ affinity, TLB maintenance, reclaim,
  and debug/tracing paths can still interact with a target CPU.
- The original goal was a bounded, recoverable CPU execution domain—not a new
  scheduler and not a second OS.

### Speaker notes

Frame the starting point for a real-time audience. A conventional low-latency
deployment starts with CPU isolation, NOHz full CPUs, RCU offloading, IRQ
affinity, and real-time scheduling. Those techniques reduce interference, but
the CPU remains an ordinary Linux scheduler domain. Work queued on a per-CPU
workqueue, a TLB shootdown, a stop-machine rendezvous, or an IRQ affinity change
can still create a path that must wait for or interrupt the target CPU.

The project asks what changes if the ownership boundary is explicit. Linux
performs a coarse-grained transition: bring a set of CPUs online under Linux,
admit and validate a workload, then lease those CPUs to an accelerator domain.
There is no fine-grained time sharing and no attempt to make the ordinary
scheduler act like an accelerator scheduler.

**Time:** approximately 75 seconds.

## Slide 3 — The final product

### On-slide content

- Fixed CPU set with one static worker per CPU.
- Transactional domain activation and rollback.
- Protected user image admitted from the executing task’s own memory map.
- Explicit shared-memory regions and bounded SPSC queues.
- Per-worker telemetry, worker-targeted escape, manager-death cleanup, and
  fail-stop domain recovery.
- Experimental ABI 25; ABI 23 and ABI 24 remain frozen compatibility floors.

### Speaker notes

Describe the final product as a small, explicit execution contract. A domain
contains one worker per selected CPU. The CPU set is immutable while active.
Changing it requires reaching a terminal state, resetting, and configuring a new
domain. This intentionally avoids migration, dynamic worker creation, time
slicing, and a general accelerator scheduler.

Activation is transactional. Every worker is prepared, all workqueue and IRQ
resources are reserved, every worker is entered, and `RUNNING` is published only
after all workers report active. If any step fails, every started worker is
stopped or unwound and every selected CPU is restored to Linux. A partially
active mixed Linux/accelerator domain is not a success state.

User images are admitted from the exact task and memory map that will execute
them. The manager does not remotely enter another process’s address space.
Workers communicate through explicit prefaulted regions and bounded SPSC queues,
not unbounded kernel services.

The ABI story is deliberately conservative: ABI 23 and ABI 24 are frozen
compatibility floors, while ABI 25 adds the static multi-worker domain and
bounded queues as a new experimental extension.

**Source links:**

- [ABI 25 version and compatibility floor](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/include/uapi/linux/cpu_accel.h#L9-L10)
- [ABI 25 domain, worker, status, and queue objects](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/include/uapi/linux/cpu_accel.h#L294-L326)
- [ABI command encodings](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/include/uapi/linux/cpu_accel.h#L398-L446)
- [Architecture report](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-report.html#L357-L448)

**Time:** approximately 90 seconds.

## Slide 4 — Post-reduction kernel integration

### On-slide content

- Broad kernel-wide `*_with_sync_wait()` API family removed.
- Per-work owner coordination moved inside ordinary workqueue completion paths.
- Only three whole-workqueue operations remain:
  - `cpu_accel_flush_workqueue()`
  - `cpu_accel_drain_workqueue()`
  - `cpu_accel_destroy_workqueue()`
- Wrapper-family paths reduced from 1,539 to 219.

### Speaker notes

This is the key engineering simplification after the initial implementation.
The first response to synchronous kernel paths was to create a parallel wrapper
for many public workqueue wait APIs. That made the correctness idea visible
everywhere, but it also created a large exported API and a huge source review
surface.

The final product moves the ownership coordination into the workqueue
completion machinery. Ordinary per-work synchronous operations use the internal
sync-wait path directly. Whole-workqueue flush, drain, and destroy operations
still need explicit wrappers because they may wait for arbitrary work in a
per-CPU queue, including work that a rescuer can execute away from the original
pool CPU. Those three wrappers reserve all possible CPUs, request active owners
to stop, run the ordinary workqueue operation, and release the reservation.

No required accelerator functionality was sacrificed by removing the broad API
family. What was sacrificed was the broad exported naming convention and
indiscriminate call-site conversion. The remaining call sites are tied to work
production, teardown, cancellation, resource release, or paths that can deadlock
against an owned CPU.

**Source links:**

- [Architecture sync-wait begin and stop sequence](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/kernel/workqueue.c#L4353-L4374)
- [`cpu_accel_flush_workqueue()`](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/kernel/workqueue.c#L4376-L4400)
- [`cpu_accel_drain_workqueue()`](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/kernel/workqueue.c#L4457-L4481)
- [Ordinary `flush_work()` using internal owner coordination](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/kernel/workqueue.c#L4614-L4618)
- [Synchronous cancel path using internal owner coordination](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/kernel/workqueue.c#L4708-L4764)
- [Destroy-time sync-wait coordination](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/kernel/workqueue.c#L6298-L6397)
- [Retained wrapper declarations and disabled-config fallbacks](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/include/linux/cpu_accel_workqueue.h#L10-L39)
- [Blast-radius reduction report](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-blast-radius-report.html#L78-L146)

**Time:** approximately 90 seconds.

## Slide 5 — v7.3-final rebase and 24-hour result

### On-slide content

| Tool | CPUs | Samples | Weighted mean | Maximum | Samples > 1 µs |
|---|---|---:|---:|---:|---:|
| cpu-accel `user-oslat` | 5, 7, 9, 11 | 17,146,540,420,604 | 20.155598190 ns | 380 ns | 0 |
| rt-tests `oslat` | 4, 6, 8, 10 | 15,240,596,761,793 | 22.676174219 ns | 9,232 ns | 345,581,176 |

- Both tools exited `0`.
- Kernel: `7.3.0-accel-tlbfix-00019-ge4bcafcc86d4`.
- Base: Linux `v7.3` final (`c03c1a14507d…`).
- Validation ladder: 60 seconds, 10 minutes, one hour, and 24 hours.
- `user-oslat` per-CPU maxima: 380, 280, 270, and 330 ns.
- All workers stayed active for the full requested duration.

### Speaker notes

Present this as exact-current empirical evidence, not as a formal hard latency
proof. The branch was rebased onto Linux `v7.3` final, built as kernel
`7.3.0-accel-tlbfix-00019-ge4bcafcc86d4`, and validated with an
increasing-duration ladder. Both workloads ran concurrently for the final 24
hours on Simon: cpu-accel user workers on CPUs 5, 7, 9, and 11, and rt-tests
`oslat` on CPUs 4, 6, 8, and 10. Launchers were constrained to housekeeping
CPUs 0–3.

The cpu-accel workers completed more than 17 trillion samples with a maximum
lateness of 380 ns and no samples above 1 µs. The concurrent rt-tests workers
completed more than 15 trillion samples, with a maximum of 9,232 ns and roughly
345.6 million samples above 1 µs. The two tools are not identical measurement
programs and use different CPU sets and scheduling classes, so this is a
same-host, exact-current comparison rather than a claim that one benchmark is
universally faster.

The health checks matter as much as the latency numbers: both tools exited
successfully, the module refcount returned to zero, no new NVMe QID 2 timeout
appeared, no new cpu-accel runtime error appeared, and all raw artifacts passed
checksum verification.

**Source link:** [v7.3-final rebase validation report](https://github.com/k-rister/linux/blob/cee53c1374a5/Documentation/accel/v7.3-rebase-4cpu-user-oslat-and-rt-tests-oslat-2026-11-26.md#L1-L118)

**Time:** approximately 90 seconds.

## Slide 6 — Explicit boundaries

### On-slide content

- CPU execution ownership and coordination only.
- No device or DMA ownership yet.
- No VFIO/IOMMUFD, BAR/MMIO, NIC interrupt routing, or DMA quiesce.
- No formal hard latency bound.
- NMIs, SMIs, machine checks, firmware effects, and arbitrary platform events
  remain outside the control plane.
- Internal review is complete; external maintainer review is pending.

### Speaker notes

Be explicit about scope. The completed work demonstrates CPU ownership,
protected user execution, static SMP behavior, bounded shared-memory
communication, recovery, and kernel-path coordination. It does not yet own a
device or DMA path, and it has not been integrated with DPDK, a PMD, or a
networking workload.

The 24-hour result is strong empirical evidence for the tested execution model,
but it is not a proof of absence for every Linux path and not a formal hard
latency bound. Firmware and platform events remain outside the software
contract.

Internal audits, builds, runtime gates, wrapper stress cases, and the 24-hour
soak are complete. External subsystem-maintainer reviews have not been requested
or received and must not be represented as sign-offs.

**Source link:** [Explicit boundary section](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-blast-radius-report.html#L197-L205)

**Time:** approximately 45 seconds.

## Slide 7 — Takeaway and next step

### On-slide content

**Takeaway:** A single Linux OS can lease a static set of CPUs to a protected
dataplane, coordinate the kernel paths that can wait for those CPUs, recover
them without rebooting, and sustain a four-CPU v7.3-final 24-hour run with no
measured user-oslat samples above 1 µs.

**Next:** Device and static-DMA ownership; external subsystem review.

### Speaker notes

Close by returning to the core architectural result. This is not a general
replacement for Linux scheduling. It is an explicit ownership contract for
workloads that can tolerate coarse-grained admission and static placement in
exchange for removing routine Linux execution from the active interval.

Phase 11 should begin with device and static-DMA ownership: VFIO/IOMMUFD,
domain-bound lifecycle, fixed DMA mappings, BAR/MMIO exposure, interrupt
masking and routing, quiesce/reset, and fault injection. External reviews remain
a parallel follow-up and are not prerequisites for designing Phase 11.

**Time:** approximately 45 seconds.

---

# Part 2 — Technical backing material

This section expands beyond the nominal 15–20 minute target as needed. It is
organized so interested readers can consume individual sections without
watching the live executive summary.

## Backing slide 1 — Original goals and non-goals

### On-slide content

- Single booted Linux OS and control plane.
- User-controlled coarse-grained CPU lease and return.
- No ordinary scheduler or routine IPI on active accelerator CPUs.
- Prefaulted shared memory and bounded queues.
- Fail-stop recovery and manager-death cleanup without reboot.
- Static multi-CPU domains before dynamic scheduling or device ownership.
- No hypervisor, second kernel, general accelerator scheduler, automatic rapid
  mode switching, or hard-latency claim before platform qualification.

### Speaker notes

The original v1.1 design deliberately started from a simple rule: every CPU
boots online under ordinary Linux control. A user-controlled transition can then
lease selected CPUs to the accelerator and later return them without rebooting.
There is no scheduler-driven time sharing between Linux and accelerator code.

That coarse-grained boundary is important. It avoids trying to make the Linux
scheduler simultaneously optimize for interactive housekeeping work and
deterministic dataplane execution. It also avoids a hypervisor: Linux remains
the only OS, owns the machine, and retains responsibility for admission and
recovery.

The ordering was intentional. CPU ownership, bounded communication, and
no-reboot recovery were prerequisites before device ownership, DMA, PMD
integration, or networking.

**Source link:** [Original design section](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-report.html#L293-L355)

## Backing slide 2 — Control plane and memory model

### On-slide content

- `/dev/cpu_accel` is a singleton administrative domain.
- Opening requires `CAP_SYS_ADMIN`; descriptors may be passed with
  `SCM_RIGHTS`.
- CONTROL, ACCEL_PRIVATE, SHARED_REGION, and future DMA mapping classes are
  separate.
- Regions are created while idle, attached before configuration, and immutable
  while active.
- Active topology, permissions, sizes, and tokens cannot change while running.

### Speaker notes

The control plane is deliberately small. Linux owns configuration, resource
accounting, recovery, and observability. The manager creates or attaches shared
regions and supplies domain configuration, but each user worker joins from its
own syscall context and memory map.

That design avoids having the kernel remotely redirect another process’s
address space. It also makes the active contract explicit: the image, stack,
control mapping, attached regions, and exact VMA set are validated before entry.
Regions remain immutable while active, and region epochs reject stale mappings.

The memory model separates control, private accelerator state, shared
manager/worker memory, and a future DMA class. The DMA class is not implemented
yet, which keeps this phase clearly within CPU execution ownership.

**Source links:**

- [Architecture report: control plane, ABI, and memory model](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-report.html#L357-L448)
- [Driver region and control mmap implementation](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/drivers/cpu_accel/cpu_accel.c#L823-L998)

## Backing slide 3 — Static SMP execution and bounded queues

### On-slide content

- One fixed worker per selected CPU.
- CPU set immutable while configured.
- Admission rejects repeated CPUs, CPU 0, offline CPUs, multiple NUMA nodes,
  incomplete SMT sibling sets, and configurations without a usable housekeeping
  CPU.
- Static linear pipeline with bounded SPSC queues.
- Queue capacity is a power of two from 1 to 1024 slots.
- Release/acquire ordering; no routine accelerator IPI for queue coordination.

### Speaker notes

Phase 10 is deliberately static. The first stage runs independent workers with
no cross-worker communication. The second stage adds bounded SPSC queues and a
linear pipeline. This sequencing separates basic multi-CPU ownership from
communication effects.

The queue contract is intentionally narrow. Worker `i` produces to queue `i`,
and worker `i+1` consumes from queue `i`. Producers and consumers spin without
overwriting a full slot or consuming an empty slot. Publication and consumption
use release/acquire ordering. A producer-done bit terminates the drain only
after the queue is empty.

The pipeline is not a general threading or messaging system. It is a bounded,
static communication topology suitable for proving that multiple owned CPUs can
coordinate through shared memory without routine IPIs.

**Source links:**

- [SMP execution model](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/phase10-smp-design.md#L8-L35)
- [Bounded queue contract](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/phase10-smp-design.md#L149-L165)
- [Driver topology and queue validation](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/drivers/cpu_accel/cpu_accel.c#L3169-L3340)
- [UAPI queue configuration object](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/include/uapi/linux/cpu_accel.h#L398-L446)

## Backing slide 4 — Transactional lifecycle and recovery

### On-slide content

- Validate every selected CPU and prepare every worker.
- Reserve workqueue and IRQ resources for every selected CPU.
- Enter every worker.
- Publish `RUNNING` only after every worker reports active.
- Failure stops or unwinds every started worker and restores Linux ownership.
- Worker fault, watchdog expiry, emergency stop, and manager death are
  domain-wide and fail closed.

### Speaker notes

The all-or-nothing lifecycle is central to the safety argument. A partially
active domain would make reasoning about CPU ownership and workload progress
much harder. Instead, the driver prepares all workers, reserves all resources,
enters all workers, and publishes success only after every worker reports
active.

Failure is also domain-wide. Per-worker diagnostics remain available, but the
domain does not continue with a subset of workers. Manager death requests
lifecycle exit and waits for all workers to reach a terminal state before
releasing resources.

ABI 25 adds a worker-indexed escape command. On x86, the kernel can send a
local-APIC NMI to a target worker; the registered handler redirects a
prevalidated user-mode frame to a prevalidated escape entry and stack, and the
worker reaches `USER_EXIT`. This is an experimental recovery mechanism for this
prototype, not a general interrupt-safe user ABI.

**Source links:**

- [Lifecycle design](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/phase10-smp-design.md#L36-L65)
- [Domain configuration and start implementation](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/drivers/cpu_accel/cpu_accel.c#L3383-L3686)
- [User worker join implementation](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/drivers/cpu_accel/cpu_accel.c#L3747-L3847)
- [Ioctl command dispatch and ABI negotiation](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/drivers/cpu_accel/cpu_accel.c#L3849-L3895)

## Backing slide 5 — x86 ownership and direct entry

### On-slide content

- Per-CPU owner state and generation tracking.
- Admission rejects active owners, exiting owners, in-flight stop requests,
  maintenance, KGDB breakpoints, live watchdogs, and pending TLB flushes.
- Direct entry uses a dedicated accelerator vector.
- Reschedule and call-function IPIs are deferred while an owner is active.
- TLB shootdowns targeting the owned `mm` are recorded and reconciled.

### Speaker notes

The x86 backend maintains per-CPU request state protected by a raw spinlock.
Before an owner enters, the code checks that the CPU is online, no maintenance
or debug rendezvous is active, no TLB flush is pending, no owner is active, and
no stop request is in flight. It records the owner generation, mm, stop callback,
and pending TLB state.

Kernel-backed lifecycle entry uses a direct accelerator vector. The entry
callback is published before the target can take the vector, and the initiating
CPU waits until the lifecycle callback reports done.

While a user owner is active, ordinary reschedule and call-function requests are
deferred and counted rather than delivered to the owned CPU. TLB shootdowns are
not ignored: the backend records the relevant generation or unscoped flush and
reconciles it when ownership ends. This is why the memory and TLB work became a
major subsystem rather than a simple CPU mask operation.

**Source links:**

- [x86 owner admission and state setup](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/arch/x86/kernel/accel.c#L139-L242)
- [x86 direct entry path](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/arch/x86/kernel/accel.c#L891-L916)
- [Deferred reschedule and call-function handling](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/arch/x86/kernel/accel.c#L918-L978)
- [TLB shootdown recording](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/arch/x86/kernel/accel.c#L980-L1008)

## Backing slide 6 — Workqueue and IRQ quarantine

### On-slide content

- Workqueue reservation redirects eligible unbound work away from owned CPUs.
- Per-CPU work remains associated with its target and is deferred until Linux
  resumes.
- IRQ quarantine reserves target CPUs against new normal affinity assignment.
- Existing IRQ affinity is snapshotted; migratable IRQs are moved.
- In-flight handlers are waited for; unsafe movement fails admission.

### Speaker notes

The kernel integration has two broad classes: workqueue selection and IRQ
affinity. Workqueue reservation prevents unbound work from being selected on an
owned CPU. Per-CPU work is not arbitrarily moved; it remains associated with
the target and is handled after ownership ends.

IRQ quarantine is an admission step. It reserves the target against new normal
IRQ affinity assignments, snapshots existing affinity, moves migratable IRQs,
waits for in-flight handlers, and fails closed if an IRQ cannot be moved safely.
Rollback restores the original affinity while filtering other
accelerator-reserved CPUs from restored masks.

These mechanisms are why the project could not stop at “take the CPU out of the
scheduler.” Real Linux paths interact through workqueue selection, IRQ routing,
TLB maintenance, and synchronous IPI waits.

**Source links:**

- [Key mechanisms report](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-report.html#L477-L510)
- [Single-CPU workqueue and IRQ reservation path](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/drivers/cpu_accel/cpu_accel.c#L2937-L3065)

## Backing slide 7 — Blast-radius reduction

### On-slide content

| Measure | Before reduction | After reduction |
|---|---:|---:|
| Non-module files | 1,625 | 309 |
| Insertions | 8,807 | 3,215 |
| Deletions | 3,437 | 872 |
| Wrapper-family paths | 1,539 | 219 |

- Full current tracked diff: 438 files, 41,126 insertions, 876 deletions.
- Retained wrapper call sites: 218 files and 526 calls.
- Internal call-site audit: 218/218 files pass.
- Internal semantic audit: 90/90 files pass.
- External maintainer review: pending.

### Speaker notes

The initial broad-wrapper design was a reasonable defensive response to the
audit finding that many ordinary kernel paths can synchronously wait for work on
an owned CPU. Its cost was an enormous review surface: more than 1,500
wrapper-family paths and a parallel naming convention across the workqueue API.

The reduction asked a sharper question: which operations actually need an
explicit accelerator-aware public API? For work-item waits, the answer was none.
The internal workqueue completion path can perform owner coordination directly,
and ordinary APIs such as `flush_work()` and `cancel_work_sync()` invoke that
path. For whole-workqueue flush, drain, and destroy, the answer was three
explicit wrappers because those operations can wait for arbitrary per-CPU work
and need an all-possible-CPU reservation.

The reduction preserved producer, teardown, cleanup, and deadlock-sensitive
call-site semantics. It removed conversions where asynchronous-work completion
was not required for accelerator correctness. Internal audits closed all
retained call-site and semantic files, but external subsystem review remains
pending.

**Source links:**

- [Reduction outcome](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-blast-radius-report.html#L120-L146)
- [Wrapper implementation review](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/phase10-blast-radius-wrapper-api-audit.md#L15-L83)
- [Call-site audit method](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/phase10-blast-radius-call-site-audit.md#L16-L33)
- [`workqueue_sync_wait_begin()`](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/kernel/workqueue.c#L4353-L4374)
- [`cpu_accel_destroy_workqueue()`](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/kernel/workqueue.c#L6382-L6397)

## Backing slide 8 — Validation methodology

### On-slide content

- Host: Simon.
- Kernel: `7.3.0-rc3-accel-tlbfix-00522-gda8c5e8e7e62`.
- Tuned profile: `cpu-partitioning`.
- Housekeeping CPUs: 0–3.
- Isolated CPUs: 4–11.
- cpu-accel workers: 5, 7, 9, 11.
- rt-tests workers: 4, 6, 8, 10.
- Launchers constrained to housekeeping CPUs.
- Both tools ran concurrently for 86,400 seconds.

### Speaker notes

Explain the placement carefully. The host uses a cpu-partitioning profile with
housekeeping CPUs 0–3 and isolated CPUs 4–11. The cpu-accel workers use
5, 7, 9, and 11, while rt-tests `oslat` uses 4, 6, 8, and 10. The control
processes and launchers are constrained to the housekeeping CPUs so test control
does not run on isolated CPUs.

The run is exact-current: it used the post-reduction module and control tool,
not an earlier binary. Both tools ran concurrently, so the comparison includes
the effect of running another busy low-latency workload on adjacent isolated
CPUs.

Scheduler-class observations were captured separately because the long-run
process capture could otherwise perturb the measured interval. The cpu-accel
workers were CFS tasks before entering the accelerator domain; the rt-tests
workers were `SCHED_FIFO 99`.

**Source link:** [Test environment and placement](https://github.com/k-rister/linux/blob/cee53c1374a5/Documentation/accel/v7.3-rebase-4cpu-user-oslat-and-rt-tests-oslat-2026-11-26.md#L1-L18)

## Backing slide 9 — Detailed v7.3-final 24-hour results

### On-slide content

| Tool | Samples | Weighted mean | Linear-rank p50/p90/p99/p999 | Maximum | Samples > 1 µs |
|---|---:|---:|---|---:|---:|
| cpu-accel `user-oslat` | 17,146,540,420,604 | 20.155598190 ns | 20/20/30/30 ns | 380 ns | 0 |
| rt-tests `oslat` | 15,240,596,761,793 | 22.676174219 ns | 20/30/30/30 ns | 9,232 ns | 345,581,176 |

Per-worker cpu-accel maxima:

| CPU | Maximum |
|---:|---:|
| 5 | 380 ns |
| 7 | 280 ns |
| 9 | 270 ns |
| 11 | 330 ns |

### Speaker notes

The user-oslat aggregate maximum is 380 ns, and every per-worker maximum is at
or below that value. All four workers report zero context switches, zero
migrations, and zero escapes. Architecture IPIs are zero; one architecture IRQ
was recorded on CPU 5, and the other workers recorded none.

The rt-tests maximum is 9,232 ns, with approximately 345.6 million samples
above 1 µs. Its tool-reported internal measurement duration is 86,329.680
seconds per thread despite the 86,400-second requested wall duration. The
report uses that tool-reported duration when calculating rt-tests sample rates
and 86,400 seconds for user-oslat.

This comparison is evidence that the post-reduction implementation sustained
the intended execution model under a long, concurrent, real-hardware run. It is
not a formal worst-case latency proof.

**Source link:** [Detailed user-oslat and rt-tests results](https://github.com/k-rister/linux/blob/cee53c1374a5/Documentation/accel/v7.3-rebase-4cpu-user-oslat-and-rt-tests-oslat-2026-11-26.md#L20-L77)

## Backing slide 10 — Run health and artifact integrity

### On-slide content

- Window: 2026-11-25 02:51:26 UTC to 2026-11-26 02:51:27 UTC.
- `user-oslat` exit: `0`.
- rt-tests `oslat` exit: `0`.
- Module refcount before and after: `0`.
- New NVMe QID 2 timeouts: none.
- New `cpu_accel` runtime errors: none.
- `sha256sum -c` passed for all raw artifacts.

### Speaker notes

Do not present latency in isolation. A soak is useful only if lifecycle and
health invariants also hold. The module was loaded before the run and returned
to refcount zero afterward. Both measurement tools exited successfully. The
kernel log showed no new NVMe QID 2 timeout and no new cpu-accel runtime error.
The raw evidence directory contains the tool outputs, scheduler observations,
pre/post kernel logs, run metadata, and a checksum manifest. All raw artifacts
passed verification.

Raw evidence remains on Simon under:
`/root/accel-measure/v7.3-rebase-4cpu-user-oslat-and-rt-tests-oslat-24h-20261125T025126Z`.

**Source link:** [Run health and artifact integrity](https://github.com/k-rister/linux/blob/cee53c1374a5/Documentation/accel/v7.3-rebase-4cpu-user-oslat-and-rt-tests-oslat-2026-11-26.md#L79-L118)

## Backing slide 11 — Review, limitations, and next work

### On-slide content

- Completed: CPU ownership, protected execution, static SMP, bounded queues,
  recovery, ABI compatibility, blast-radius reduction, internal audits, the
  v7.3-final rebase, and increasing-duration soak ladder.
- Not completed: device/DMA ownership, VFIO/IOMMUFD, BAR/MMIO, device
  interrupts, DMA quiesce/reset, DPDK/PMD adaptation, networking, formal bound,
  non-x86 backend, external review.
- Phase 11 starts with device and static-DMA ownership.

### Speaker notes

Close the detailed section by separating implementation state from review state.
The internal implementation and audit work is closed for the tested scope. The
full tracked diff is still large—438 files, 41,126 insertions, and 876
deletions—so external review should focus on x86 ownership, workqueue semantics,
IRQ affinity, MMU/TLB/reclaim, SMP and CPU transitions, and the required
driver-specific ordering changes.

Phase 11 should not begin by adding networking directly. It should first define
device ownership: VFIO/IOMMUFD integration, domain-bound lifecycle, fixed DMA
mappings, BAR/MMIO exposure, interrupt masking and routing, quiesce/reset, and
fault injection. Only after that boundary is stable should PMD or DPDK
adaptation and networking workloads be considered.

**Source links:**

- [Current boundary and unfinished work](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-report.html#L619-L660)
- [External review priorities](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-blast-radius-report.html#L184-L196)

---

# Anticipated questions

## Why is this not just CPU isolation?

CPU isolation reduces ordinary scheduler and IRQ work, but the CPU remains part
of the ordinary Linux execution model. Workqueues, synchronous IPIs, TLB
shootdowns, stop-machine, hotplug, debug/tracing, and IRQ affinity can still
interact with the target. CPU-accel adds an explicit ownership and coordination
boundary.

## Why keep Linux as the single OS?

Linux already provides resource accounting, device management, observability,
authentication, lifecycle control, and recovery. The experiment is whether a
coarse-grained CPU lease can provide deterministic execution without introducing
a second kernel or hypervisor.

## Why remove the broad `*_with_sync_wait()` family?

The broad family duplicated the same ownership concern across many public API
names. The reduced design performs per-work owner coordination inside ordinary
workqueue completion paths and retains explicit wrappers only for whole-queue
flush, drain, and destroy operations.

## Did the reduction sacrifice functionality?

No required accelerator functionality was intentionally sacrificed. The broad
exported API and unnecessary call-site conversions were removed. Producer,
teardown, cleanup, and deadlock-sensitive semantics remain coordinated.

## Is the 24-hour result a hard latency bound?

No. It is exact-current empirical evidence for the tested host, kernel,
configuration, workload, and duration. A formal hard bound would require a
stronger system model and qualification of firmware, hardware, and platform
events outside the current control plane.

## Why compare user-oslat with rt-tests oslat?

Both are cyclic timestamp workloads, but they are not identical implementations.
The value is a concurrent, same-host, exact-current comparison under the same
isolation profile. It should not be presented as a universal benchmark victory.

## What is the biggest remaining risk?

Device and DMA ownership are unimplemented, and external subsystem review is
pending. The kernel changes outside the accelerator module remain the primary
review risk even after the substantial blast-radius reduction.

---

# Source traceability

| Claim or area | Source |
|---|---|
| Original goals and non-goals | [Design report](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-report.html#L293-L355) |
| Control plane, memory, static SMP, and queues | [Architecture report](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-report.html#L357-L448) |
| Transactional lifecycle and recovery | [Phase 10 design](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/phase10-smp-design.md#L36-L65) |
| ABI 25 objects and commands | [UAPI header](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/include/uapi/linux/cpu_accel.h#L294-L446) |
| Domain configuration, start, and worker join | [cpu-accel driver](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/drivers/cpu_accel/cpu_accel.c#L3383-L3847) |
| x86 owner admission and direct entry | [x86 accelerator backend](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/arch/x86/kernel/accel.c#L139-L916) |
| Deferred IPI and TLB recording | [x86 accelerator backend](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/arch/x86/kernel/accel.c#L918-L1008) |
| Workqueue owner coordination | [workqueue core](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/kernel/workqueue.c#L4353-L4481) |
| Per-work and destroy-time coordination | [workqueue core](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/kernel/workqueue.c#L4614-L6397) |
| Retained wrapper API and fallbacks | [cpu-accel workqueue header](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/include/linux/cpu_accel_workqueue.h#L10-L39) |
| Blast-radius reduction metrics | [Blast-radius report](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-blast-radius-report.html#L78-L146) |
| Wrapper semantic audit | [Wrapper API audit](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/phase10-blast-radius-wrapper-api-audit.md#L15-L119) |
| Call-site audit closure | [Call-site audit](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/phase10-blast-radius-call-site-audit.md#L1-L104) |
| v7.3-final results and health | [v7.3-final rebase validation report](https://github.com/k-rister/linux/blob/cee53c1374a5/Documentation/accel/v7.3-rebase-4cpu-user-oslat-and-rt-tests-oslat-2026-11-26.md#L1-L118) |
| Current boundary and unfinished work | [Design report](https://github.com/k-rister/linux/blob/e4bcafcc86d44b6e8e800f54a9394dbc9ff507d/Documentation/accel/cpu-accel-report.html#L619-L660) |
