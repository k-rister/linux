.. SPDX-License-Identifier: GPL-2.0

====================================
Single-CPU accelerator prototype
====================================

The ``CPU_ACCEL`` driver is the first foundation prototype for a Linux
dataplane accelerator environment.  It is an experimental control plane for
one online CPU and is intended to make the control and measurement interfaces
concrete for a persistent accelerator ownership mode.

This version is deliberately not a complete isolated accelerator CPU.  Its
architecture-neutral lifecycle boundary synchronously dispatches an entry
function to the target CPU while a controller thread holds the CPU-hotplug
read lock.  The target remains online, but preemption and local maskable
interrupts are disabled until ``STOP``, completion, or the configured
watchdog.  Returning from the entry function returns the CPU to normal Linux
execution; no CPU offline/online transition is required.  On x86 with a local
APIC, kernel workloads use a dedicated APIC vector for entry; other
architectures retain the generic SMP fallback.

It does not yet provide a memory-protection domain, explicit interrupt source
ownership, TLB-shootdown suppression, or a hard latency bound.  NMIs, SMIs,
machine checks, pending IPIs, firmware activity, and hardware execution
effects remain outside this prototype's control.

Interface
=========

The driver creates ``/dev/cpu_accel`` with a fixed-size shared mapping and a
small ioctl interface:

* ``CONFIG`` selects an online CPU other than CPU 0 and supplies a duration
  and sample period.
* ``START`` explicitly enters the lifecycle-backed accelerator workload.
  ``CPU_ACCEL_FLAG_PERSISTENT`` keeps ownership until STOP or the duration
  watchdog; the SDK tool exposes this as ``--persistent``.
* ``STOP`` requests an orderly exit at the next sample boundary.
* ``EXIT`` waits for the target CPU to return from the accelerator entry
  function and completes the transition back to Linux.
* ``RESET`` returns the device to its initial state after a completed run.

The control mapping contains the state, explicit Linux/accelerator transition
mode, selected workload, run timestamps, aggregate lateness, and up to
``CPU_ACCEL_MAX_SAMPLES`` timestamp samples.  ABI version 16 also
reports lifecycle entry/exit timestamps, interrupt and softirq deltas,
timer, hrtimer, RCU, and scheduler softirq deltas, current-task
context-switch deltas, CPU-entry/exit identity, migration detection, pending
scheduler and softirq state, and preemption state.  On x86 it additionally
reports architecture interrupt, IPI, TLB, deferred-reschedule, and
deferred-call-function counter deltas, plus native x86 TLB flush target
batches directed at the CPU during accelerator ownership, including the
ring-3 process-mm window;
``arch_counters_valid`` identifies whether those counters are available.
The x86 direct backend is reported as
``CPU_ACCEL_BACKEND_X86_DIRECT_APIC``; it uses one dedicated APIC vector for
entry and avoids the generic scheduler/function-call IPI path.  This is a
direct-entry prototype, not full APIC ownership: the backend defers remote
reschedule and call-function IPIs while it owns the target and replays them
after exit, but Linux can still generate other IPIs or TLB shootdowns for the
target CPU.
Workqueue queue and execution tracepoints report activity targeted at the
accelerator CPU while it is running.
These are observations made by the prototype, not suppression or admission
controls for the corresponding activity.

``backend_flags`` reports backend/workload capabilities.  The
``CPU_ACCEL_BACKEND_FLAG_USER_ESCAPE`` bit means that the configured workload
has a supported recovery operation.  ``recovery_state``,
``recovery_error``, ``recovery_attempts``, and the recovery timestamps form a
common result contract independent of how an architecture performs the
escape.  A request is reported as ``REQUESTED`` while in flight,
``SUCCEEDED`` after the protected image reaches its terminal exit,
``UNSUPPORTED`` when the backend cannot provide the operation, and
``TIMEOUT`` or ``FAILED`` when the attempt did not complete.  A failed or
timed-out attempt does not pretend that the CPU has returned to Linux; the
shared ``mode`` and ``state`` remain nonterminal until a later recovery or
cooperative exit succeeds.

The ``CPU_ACCEL_BACKEND_FLAG_RESCHEDULE_DEFER`` bit means that the x86 direct
backend defers remote scheduler reschedule IPIs during the accelerator
interval.  ``arch_reschedule_deferred`` reports how many such requests were
deferred during the run.  This is intentionally selective: function-call
IPIs, TLB shootdowns, local timer interrupts, NMIs, and other interrupt paths
are covered independently.  The
``CPU_ACCEL_BACKEND_FLAG_CALL_FUNCTION_DEFER`` bit means that call-function
IPIs are deferred and replayed after exit; ``arch_call_function_deferred``
reports how many were deferred. Asynchronous callbacks remain deferred without
stopping the owner. A synchronous waiter reserves the target against new
ownership before queueing its callback, then asks an active registered owner
to stop before it waits. This also covers a caller blocked while reusing a CSD
for earlier asynchronous work. TLB shootdowns and other interrupt paths are
not covered by either capability as an explicit policy. On the native x86
path, remote TLB flush callbacks use call-function work, so this generic
deferral also delays those callbacks until after exit; synchronous flush
senders can request the active owner to stop. ABI 16's
``arch_tlb_shootdown_targets`` counts native x86 flush batches that targeted
the CPU while accelerator ownership was active, including the ring-3
process-mm window through image unlock. For that worker, native x86 hooks also
record the highest generation targeting its ``mm`` and whether an
address-space-unscoped flush, such as a kernel/global flush, is pending. The
owner retires ownership and locally reconciles pending TLB state while the
image mm remains write-locked, before releasing pinned image pages. Remote
callback completion remains governed by the native flush path. The target
counter does not measure completion or provide an isolation or latency
guarantee, and paravirtual TLB-flush paths may use different mechanisms.

The initial workload selector supports ``timestamp`` and ``memmove``.  The
memmove workload allocates and touches two bounded kernel buffers before the
accelerator entry point, then copies between them once per timing period.  It
is an oslat-like non-networked workload for measuring execution-source
isolation while avoiding page faults and system calls in the accelerator
interval.  The buffers are kernel-owned; this workload does not yet provide a
protected user address space or a user-supplied accelerator binary.

ABI version 13 retains the x86-only ``user-oslat`` workload.  The companion forks
a worker so the accelerator task has a distinct ``mm_struct``, pins that task
to the target CPU, and supplies page-aligned executable-image and private-stack
ranges.  The kernel validates the VMAs, prefaults and pins their pages, holds
the worker address space's write-side mapping lock for the active epoch, and
returns the worker to ring 3 with the saved user register frame and IF clear.
The driver retains the opener's ``mm_struct`` as the companion identity and
rejects admission if the worker still shares that address space.
The image polls the existing control mapping, records TSC-derived samples, and
uses ``CPU_ACCEL_IOC_USER_EXIT`` as its only terminal system call.  The kernel
restores the original ``START`` return frame, releases the address-space
admission lock, and then restores Linux-owned IRQ/workqueue state.

The shared result now reports ``user_active_start_ns``,
``user_active_end_ns``, and ``user_escape_count``.  On x86, the companion may
issue ``CPU_ACCEL_IOC_USER_ESCAPE`` while this image is active.  The kernel
sends a local-APIC NMI to the target CPU; the registered NMI handler redirects
only a user-mode frame for the active, prevalidated image to its prevalidated
escape entry and stack.  That entry performs the existing terminal
``CPU_ACCEL_IOC_USER_EXIT`` syscall.  This is an experimental recovery path
for the prototype, not a general interrupt-safe user ABI.  The image and
stack remain pinned and the address-space mapping lock remains held while the
escape is possible.

These timestamps bound the interval beginning at ring-3 image entry and
ending at entry to the terminal ``USER_EXIT`` syscall.  The generic
``lifecycle_entry_ns``/``lifecycle_exit_ns`` counters include kernel handoff
and post-exit cleanup, so they must not be used as a direct measurement of the
protected user interval.

This is a cooperative ring-3 proof, not a general user-program ABI.  The
image must not fault, make ordinary system calls, return normally, create
threads, or modify its address space while active.  The x86 escape path only
handles a user-mode frame on a live local APIC; it does not recover a CPU
stuck in kernel mode, an NMI/SMI/machine-check path, a disabled or failed
local APIC, a host or hypervisor fault, or a corrupted/self-modifying image.
The image and stack are pinned only for the active epoch, and the current
prototype still does not provide an IOMMU domain or a formal hard-latency
bound.

ABI version 12 adds the x86-only ``user-hang`` workload for recovery testing.
It deliberately spins in ring 3 without polling the control mapping and must
be started with the companion's ``--escape-after-ms`` option.  A successful
forced escape terminates with ``CPU_ACCEL_STATE_ESCAPED`` rather than
``CPU_ACCEL_STATE_COMPLETE``.  This workload is a fault-injection fixture,
not an application contract.  The debug-only module parameter
``user_escape_drop_count=N`` drops the next ``N`` x86 escape NMIs; the
companion's ``--escape-retries`` option exercises the retry contract after a
reported timeout.

ABI version 13 retains the fixed-size shared-region mapping at
``CPU_ACCEL_SHARED_MAP_OFFSET``.  It contains two bounded entries, each with
an owner, epoch, length, and data area.  The ``shared-memmove`` workload uses
one selected entry and copies between its two halves.  The companion
initializes the entry and issues ``CPU_ACCEL_IOC_SHARED_READY`` before
``START``.  The kernel publishes ``ACCELERATOR`` while the workload owns the
entry, then retains ``COMPLETE`` or ``ERROR`` ownership after the lifecycle
returns.  The companion must issue ``CPU_ACCEL_IOC_SHARED_RECLAIM`` after
observing the terminal result before reusing the entry.  The kernel rejects
stale epochs, wrong lengths, active-entry reuse, and ownership races.
This is a prefaulted, kernel-backed shared-memory proof of the ownership
protocol; it is not yet an IOMMU-protected user mapping.

The mode reports ``LINUX``, ``ENTERING``, ``ACCELERATOR``, ``EXITING``, or
``RECOVERY``.  It makes the lifecycle transition explicit for the control
plane, but does not yet claim that Linux has removed every scheduler,
interrupt, RCU, workqueue, or TLB responsibility from the target CPU.

While the target is entering or in accelerator mode, the workqueue core
reserves it for unbound-work selection and redirects eligible unbound work to
another online CPU.  Per-CPU work remains associated with its target and is
deferred until Linux mode resumes.  Together with the opt-in IRQ quarantine
below, this is the first active execution-source quarantine mechanism; it
does not yet cover local timers, RCU callbacks, or TLB shootdowns.

The first ABI supports ``CPU_ACCEL_FLAG_IRQS_OFF`` and
``CPU_ACCEL_FLAG_PERSISTENT``.  ``CPU_ACCEL_FLAG_IRQ_QUARANTINE`` is an
explicit opt-in admission step that reserves the target against new normal
IRQ affinity assignments, snapshots active IRQ affinity, moves migratable
IRQs to other online CPUs, and waits for in-flight handlers.  Architecture
specific deferred moves are completed on the CPU currently owning the IRQ
vector during this transition; this transition-time cross-CPU activity is
not part of the accelerator interval.  It fails closed and rolls back if an
active IRQ is per-CPU, non-balancable, has no affinity setter, or cannot be
moved.  ``irq_quarantined`` reports the number moved;
``irq_quarantine_blockers`` reports blockers from a rejected start.  This
does not suppress IPIs, local timers, NMIs, firmware activity, or later IRQ
affinity changes made through internal paths.
``CPU_ACCEL_FLAG_REQUIRE_QUIESCENT`` adds an
entry admission check that rejects a target with a pending reschedule or
softirq request and reports ``RECOVERY`` mode.  It is a precondition check,
not a mechanism for draining or suppressing those sources.  A watchdog
termination is reported as
``CPU_ACCEL_STATE_WATCHDOG``.
Successful forced ring-3 recovery is reported as
``CPU_ACCEL_STATE_ESCAPED``; this distinguishes recovery from cooperative
completion.

The companion SDK in ``tools/cpu_accel`` wraps the device and
``cpu-accelctl`` provides a minimal command-line exerciser::

  make -C tools/cpu_accel
  sudo make -C tools/cpu_accel test
  CPU_ACCEL_REPEATS=5 CPU_ACCEL_LOAD_CPUS=2-7 \
    sudo make -C tools/cpu_accel test
  sudo insmod drivers/cpu_accel/cpu_accel.ko
  sudo tools/cpu_accel/cpu-accelctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --persistent
  sudo tools/cpu_accel/cpu-accelctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --workload memmove --work-bytes 4096
  sudo tools/cpu_accel/cpu-accelctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --workload shared-memmove --work-bytes 4096
  sudo tools/cpu_accel/cpu-accelctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --workload user-oslat
  sudo tools/cpu_accel/cpu-accelctl run --cpu 1 --duration-ms 5000 \
    --period-us 1000 --workload user-hang --escape-after-ms 20
  sudo tools/cpu_accel/cpu-accelctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --quarantine-irqs

The focused function-graph teardown test can also overlap BPF JIT/seccomp
filter allocation and teardown with an active owner. Build the bounded churn
helper and set ``CPU_ACCEL_SECCOMP_CHURN`` to enable this additional workload::

  make -C tools/cpu_accel test-seccomp-jit-churn
  sudo env CPU_ACCEL_SECCOMP_CHURN=tools/cpu_accel/test-seccomp-jit-churn \
    CPU_ACCEL_SECCOMP_CPUS=3-7 sh tools/cpu_accel/test-ftrace-maintenance.sh

The global TLB-flush test can load a module already installed for the running
kernel. This is useful when a matching probe module cannot be built locally.
Choose a module that is safe to load and unload; module parameters are passed
after the module name::

  sudo env CPU_ACCELCTL=/tmp/cpu-accelctl \
    sh tools/cpu_accel/test-tlb-flush.sh --modprobe dummy numdummies=0

Without ``--modprobe``, the test continues to load its matching no-op probe
module with ``insmod``.

To validate synchronous call-function IPIs during ring-3 ownership, run the
prewarmed seccomp/JIT overlap test with a small two-CPU worker set::

  sudo env CPU_ACCELCTL=/tmp/cpu-accelctl \
    CPU_ACCEL_SECCOMP_CHURN=/tmp/test-seccomp-jit-churn \
    sh tools/cpu_accel/test-call-function-owner-stop.sh

The synchronous per-CPU work test also covers SLUB cache shrink, the RCU
sheaf drain used when a cache is destroyed, and network backlog flushing.
The network case creates a temporary veth pair, directs receive-side scaling
to an online housekeeping CPU accepted by the kernel's RPS filter, floods it
while the owner is active, and unregisters the device. This can be a different
CPU from the one used by the other cases because RPS excludes isolated CPUs.
The timer migration case temporarily creates an isolated cpuset partition and
requires the cgroup v2 cpuset controller; the script enables it only when
needed and restores the prior controller state after the case. The worker
enters the test partition while its control process stays on a housekeeping
CPU to deliver the timed escape. The script temporarily selects ``never`` in
transparent hugepage's ``enabled`` control so ``khugepaged``'s unrelated
``lru_add_drain_all()`` cannot stop the owner before the timer hook; cleanup
restores the previous mode. Set ``CPU_ACCEL_TEST_CASE=timer_migration`` to run
only this case while debugging; the default runs the complete suite. The
owner's fallback escape defaults to 4.9 seconds, leaving margin after the
trace-entry poll, while the timer migration operation must still finish in
under 2 seconds.
Build the matching probe module with the other CPU accelerator probes before
running the test; the VM also needs ``iproute2``, ``iputils``, and the ``veth``
module::

  make -C /lib/modules/$(uname -r)/build \
    M="$PWD/tools/cpu_accel/tlb-flush-probe" modules
  sudo make -C tools/cpu_accel test-sync-work-owner-stop

The same kernel changes also gate per-CPU callback work drained by
``cleanup_srcu_struct()`` and online per-node workers used by vmalloc purge.
The current script has no focused SRCU-cleanup trigger. The vmalloc worker path
requires multiple NUMA purge nodes and enough lazy areas to select a helper;
the single-node test VM does not meet that condition, so this path has only
been build- and source-validated there.

The focused single-call probe covers synchronous IPI delivery, reuse of a CSD
whose earlier asynchronous callback is still pending, and the remote
``smp_call_on_cpu()`` and ``work_on_cpu()`` workqueue waits. Build its module
against the running kernel, then run any of the modes below (the default
target and work CPUs are 2 and 3)::

  make -C /lib/modules/$(uname -r)/build \
    M="$PWD/tools/cpu_accel/tlb-flush-probe" modules
  sudo tools/cpu_accel/test-call-function-single-owner-stop.sh sync
  sudo tools/cpu_accel/test-call-function-single-owner-stop.sh async-reuse
  sudo tools/cpu_accel/test-call-function-single-owner-stop.sh smp-call-on-cpu
  sudo tools/cpu_accel/test-call-function-single-owner-stop.sh work-on-cpu

The opt-in ``flush_work_with_sync_wait()`` API can be checked with a probe
that queues work on the active owner CPU, then flushes it from another CPU.
``flush_workqueue_with_sync_wait()`` covers per-CPU queues whose flush may
include work run by a rescuer away from its originating pool. It reserves every
possible CPU for the duration of that flush, so use it only when that broad
owner-stop scope is acceptable. The queue-wide API retains the standard warning
against flushing system-wide workqueues. Build the probe against the candidate
kernel and run either focused case::

  make -C /lib/modules/$(uname -r)/build \
    M="$PWD/tools/cpu_accel/tlb-flush-probe" modules
  sudo env CPU_ACCEL_TEST_CASE=sync_work_flush \
    make -C tools/cpu_accel test-sync-work-owner-stop
  sudo env CPU_ACCEL_TEST_CASE=sync_workqueue_flush \
    make -C tools/cpu_accel test-sync-work-owner-stop

Each test requires an owner-stop/exit trace pair and execution of the same
work item on the target CPU. The queue-flush case uses a private ``WQ_PERCPU``
workqueue.

The tool prints the shared result, including the maximum observed lateness,
selected workload, work size, completed work iterations, and lifecycle
telemetry.  The generic interrupt and context-switch deltas
are sampled around the target callback; the context-switch value is the
current task's switch-counter delta.  x86 IPI/TLB counters are read from the
per-CPU architecture interrupt statistics.  ``CPU_ACCEL_REPEATS`` repeats the
normal run, while ``CPU_ACCEL_LOAD_CPUS`` starts a busy loop on the listed
non-target CPUs for a loaded measurement.  Workqueue counters depend on
tracepoint exports from the core workqueue implementation and count only
queueing requested for, or execution occurring on, the target CPU during the
accelerator interval.
The tool pins its control process to CPU 0.  CPU 0 is reserved for
control-plane work by this prototype and is rejected as a target.

Lifecycle and next steps
========================

The watchdog is capped at five seconds and ``STOP`` is cooperative.
``CPU_ACCEL_IOC_USER_ESCAPE`` provides a separate x86 recovery experiment for
the ring-3 workload, but it is bounded by a one-second controller wait and is
not a hard guarantee.  A malfunctioning kernel implementation is not assumed
to be recoverable without reverting to the known-good kernel.  This lifecycle
is the first step toward that model.  ABI version 15 added selective
reschedule- and call-function-IPI deferral and replay while retaining the
ABI12 recovery capability and debug-only dropped-NMI fixture. ABI 16 adds TLB
target telemetry, and implementation checkpoint 10 now defines the x86
address-space/TLB ownership policy, including completion before eligible
page reuse and conservative synchronous paths for other cases. Same-mm range
flushes and page-table release remain synchronous; runtime ``INVLPGB``
validation awaits AMD EPYC 7003-or-later hardware. The current roadmap focus is
Phase 8 adversarial isolation validation. A controller may retry after
``TIMEOUT`` or ``FAILED``; it must not treat those states as a return to Linux.
The proposed protected address-space and shared-memory contract is documented
in :doc:`cpu-accel-memory`; the internal region/epoch model and the first
prefaulted shared entry are now in place.  The x86 cooperative ring-3 image is
the first protected-address-space proof, and the ABI 12 NMI escape is the
first x86 recovery experiment. The common recovery contract, direct APIC entry,
and checkpoint 10 TLB policy are in place for the tested x86 prototype. IOMMU-
backed memory and networking remain gated on the adversarial validation phase.
A stronger latency claim must wait for those controls and for a defined
recovery contract.

Phase 8 isolation matrix
========================

This matrix maps the Phase 8 activity classes to the current x86 prototype's
policy and evidence.  A listed test is a test entry point, not a claim that it
passed on the current kernel.  The matrix records source and harness coverage;
run-specific results must be recorded with the kernel build and test logs.

.. list-table:: Phase 8 activity disposition and evidence
   :header-rows: 1
   :widths: 17 39 25 20

   * - Activity
     - Disposition and policy
     - Evidence in tree
     - Missing evidence or limitation
   * - Normal IRQ affinity and IRQ storms
     - **Routed/rejected, opt-in.** IRQ quarantine moves migratable normal
       IRQs to other online CPUs and rejects admission if a blocker remains.
       It does not cover IPIs, local timers, NMIs, firmware, or internal IRQ
       affinity changes after admission.
     - ``test-cpu-accel.sh`` has an optional ``--quarantine-irqs`` path;
       shared results report moved IRQs and blockers.
     - No IRQ-storm fault injector or post-admission affinity-change test.
   * - Local timers and timer activity
     - **Deferred only while maskable interrupts are disabled.** There is no
       general timer ownership or housekeeping-routing policy for an active
       CPU. Timer migration maintenance requests owner stop.
     - ``test-sync-work-owner-stop.sh`` has a ``timer_migration`` case.
       Timer and hrtimer softirq deltas are reported.
     - No local-timer storm test; softirq counts do not count individual
       timer callbacks or bound their latency.
   * - RCU callbacks
     - **Owner-stop for selected synchronous waits; otherwise unclassified.**
       The IRQ quarantine is not an RCU callback-routing mechanism.
     - The sync-work test covers selected RCU sheaf and SRCU cleanup paths;
       ``rcu_softirq_count`` reports the softirq vector.
     - No RCU callback count, callback storm, or general RCU-routing proof.
   * - Scheduler and ordinary task placement
     - **Deferred selectively.** Reschedule IPIs are counted and replayed
       after owner exit. The prototype reserves the target for selected
       execution sources; this is not a proof for every Linux kernel path.
     - ``arch_reschedule_deferred``, context-switch and migration fields;
       baseline and loaded oslat runs check selected invariants.
     - No exhaustive task-placement or unbounded-kernel-path detector.
   * - Asynchronous call-function work
     - **Deferred.** The request remains queued and is replayed after owner
       exit.
     - ``arch_call_function_deferred`` and call-function owner-stop tests.
     - Other IPI classes and arbitrary callback workloads are not covered by
       this selective path.
   * - Synchronous CPU calls and workqueue waits
     - **Owner-stop, then synchronous completion.** A synchronous waiter
       reserves the target, requests owner exit, and retains the reservation
       until its callback completes. Unbound work is **routed** away from the
       owner; per-CPU work is **deferred** until Linux mode resumes.
     - ``test-call-function-owner-stop.sh``,
       ``test-call-function-single-owner-stop.sh``, workqueue counters, and
       ``test-sync-work-owner-stop.sh`` cover selected callbacks and waits,
       including vmstat, LRU/SLUB drains, network backlog, vmalloc purge, and
       focused flush probes. The source audit also converted default-bound
       notification teardown in the AOA GPIO Feature and PMF backends; those
       PowerPC-only objects were not build-tested on this x86 host.
     - The scripts cover selected producers and APIs, not every queue,
       callback, or synchronous wait site under load.
   * - ``mmap``/unmap and page-table changes on other CPUs
     - **Deferred with completion before eligible page reuse; synchronous for
       remaining paths.** A different-mm ring-3 owner can carry a pending
       generation and flush locally before reusing that ``mm``. Same-mm VMA
       teardown cannot overlap the current sealed owner lifecycle.
     - ``test-tlb-generation.c`` covers remote-mm unmap, COW, data/PTE-page
       reuse, and a clean-file-page reclaim probe.
     - Generic batch disposition and same-mm teardown during ownership are
       outside the current lifecycle contract.
   * - TLB shootdown pressure
     - **Deferred selectively or owner-stop/synchronous.** The x86 path
       filters only eligible different-mm ring-3 targets; same-mm range
       flushes and kernel-mode owners remain synchronous. Eligible data-page
       release waits for reconciliation; page-table release remains
       synchronous.
     - TLB target telemetry and flush-wait traces;
       ``test-tlb-flush.sh`` and ``test-tlb-generation.c`` provide focused
       pressure and reuse cases.
     - Runtime ``INVLPGB`` validation requires AMD EPYC 7003-or-later
       hardware, which is unavailable in this environment.
   * - Module load/unload and executable-code maintenance
     - **Owner-stop before audited maintenance.** The text and ftrace paths
       with a reservation drain active owners before mutation.
     - ``test-tlb-flush.sh`` loads a probe while the owner runs;
       sync-work probes exercise selected ``rmmod`` cleanup. The ftrace test
       tears down function-graph tracing and optionally runs BPF JIT churn.
     - These are focused cases, not an exhaustive test of module lifecycle or
       all executable-memory writers.
   * - Perf and tracing operations
     - **Owner-stop for the audited ftrace maintenance path and per-CPU PMU
       event installation; otherwise unclassified.** Perf's per-CPU event
       installation uses a synchronous CPU callback, which enters the generic
       owner-stop hook before it can complete on the target CPU.
     - ``test-ftrace-maintenance.sh`` tests function-graph teardown and
       optional BPF JIT churn. ``test-perf-owner-stop.sh`` opens a per-CPU
       ``cycles:u`` event while a ring-3 owner is active and checks the paired
       owner-stop and exit tracepoints.
     - The perf probe has not run against the accelerator kernel in the
       current VM, and covers one hardware event install/close path only.
       Repeated PMU churn, task-scoped events, PMI delivery during ownership,
       and a comprehensive tracing-operation matrix remain open.
   * - Resctrl changes
     - **Owner-stop for synchronous remote CPU callbacks.** The x86 domain
       programming paths use ``smp_call_function_any(..., 1)`` and
       ``on_each_cpu_mask(..., 1)``; the generic synchronous-call hooks reserve
       each remote CPU and request owner exit before waiting for its callback.
       Task-group changes and other resctrl activity have no subsystem-specific
       policy established here.
     - Source review covers the resctrl callback sites and their path through
       ``arch_smp_sync_wait_begin/stop/end``. The call-function owner-stop
       tests cover that generic callback machinery.
     - No resctrl operation has been fault-injected while an owner is active,
       and there is no resctrl-specific counter or policy for the remaining
       task-group and monitoring paths.
   * - CPU hotplug attempts
     - **Owner-stop before offlining.** The offlining request waits until the
       active owner exits, then removes the CPU.
     - ``test-cpu-accel.sh`` starts an active owner, requests offlining from
       CPU 0, and checks that the owner exits before the CPU goes offline.
     - Repeated and loaded hotplug races are not represented by this case.
   * - ``stop_machine()`` requests
     - **Owner-stop on sleepable paths; conditional reject on inactive-CPU
       rendezvous.** The public ``stop_machine()`` wrapper drains owners before
       taking CPU-hotplug locks. Each in-tree x86 caller of
       ``stop_machine_cpuslocked()`` has a caller-side maintenance gate before
       its locks and rendezvous. When called,
       ``stop_machine_from_inactive_cpu()`` uses a nonblocking reservation and
       returns ``-EBUSY`` if an owner prevents it. The x86 cache-CPU-online
       caller can skip that rendezvous when cache-control setup is disabled or
       delayed AP initialization remains active. No in-tree x86 caller of
       ``stop_core_cpuslocked()`` was found.
     - The callsite review covers CPU hotplug, MTRR updates, late microcode,
       TDX module installation, and cache initialization. Existing CPU
       hotplug and ftrace tests cover selected owner-stop paths. A bounded VM
       probe confirmed the ring-3 owner was active before bringing CPU 3
       online, but CPU 3 still came online. The test did not establish whether
       the conditional cache rendezvous ran, so this is not a busy-reject
       result.
     - No general ``stop_machine()`` fault injector or counter proves which
       conditional path ran. The active-owner CPU-up probe is inconclusive;
       caller lock-order testing and disposition of out-of-tree direct
       ``stop_machine_cpuslocked()`` callers also remain open.
   * - Memory pressure and reclaim
     - **Safe only on audited completion paths; otherwise synchronous.** A
       clean file-page reclaim probe waits for owner reconciliation. Dirty or
       writeback reclaim and migration remain synchronous.
     - ``test-tlb-generation.c`` exercises ``MADV_PAGEOUT``; sync-work tests
       cover LRU/SLUB drains and vmalloc purge.
     - No sustained pressure envelope or dirty/writeback/migration stress
       report.
   * - Device-management operations
     - **Unclassified beyond tested network backlog work.** The backlog flush
       test does not establish a general device ownership or reset policy.
     - ``test-sync-work-owner-stop.sh`` has a veth/network-backlog case.
     - No device reset, teardown, or management-operation fault matrix.
   * - Exceptions, NMIs, and machine checks
     - **NMI escape is owner-terminating recovery for the tested ring-3
       workload; general events are not contained.** NMI and MCE counts are
       folded into ``arch_irq_count``; exceptions have no dedicated count.
     - ``user_escape_count`` reports the escape experiment; the aggregate x86
       IRQ counter includes NMI and MCE statistics.
     - No separate event counters, exception injection matrix, or safe MCE
       injection evidence.
   * - No ordinary task or unbounded path; latency
     - **Selected invariants measured; no hard bound.** Existing counters do
       not prove absence of every ordinary Linux or unbounded kernel path.
     - Context-switch, migration, reschedule, IRQ/IPI/TLB, workqueue, and
       workload-lateness fields; tests report observed maxima for selected
       workloads.
     - No full Phase 8 worst-case latency report, percentile distribution, or
       complete event correlation across the listed stress classes.

The plan's disposition vocabulary is safe, routed to housekeeping, deferred,
rejected, or domain-terminating. Several rows remain unclassified or have only
selective policies; those are open audit findings, not implied rejections.
Phase 8 remains open until each activity has an enforced disposition, the
fault-injection suite and counters cover the required events, and a
run-specific worst-case latency report is recorded.
