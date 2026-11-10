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

The driver creates ``/dev/cpu_accel`` with a fixed-size control/status mapping
and a small ioctl interface:

* ``CONFIG`` selects an online CPU other than CPU 0 and supplies a duration
  and sample period.
* ``START`` explicitly enters the lifecycle-backed accelerator workload.
  ``CPU_ACCEL_FLAG_PERSISTENT`` keeps ownership until STOP or the duration
  watchdog; the SDK tool exposes this as ``--persistent``.
* ``STOP`` requests an orderly exit at the next sample boundary.
* ``EXIT`` waits for the target CPU to return from the accelerator entry
  function and completes the transition back to Linux.
* ``RESET`` returns the device to its initial state after a completed run.
* ``GET_CAPABILITIES`` reports the runtime region count, per-region, and
  total-size limits plus the supported permission combinations.
* ``REGION_CREATE``, ``REGION_ATTACH``, ``REGION_DETACH``,
  ``REGION_DESTROY``, and ``REGION_GET_INFO`` manage variable-size,
  workload-owned shared regions.

The control mapping contains the state, explicit Linux/accelerator transition
mode, selected workload, run timestamps, region-set metadata, and kernel-owned
isolation telemetry.  ABI version 16 also
reports lifecycle entry/exit timestamps, interrupt and softirq deltas,
timer, hrtimer, RCU, and scheduler softirq deltas, current-task
context-switch deltas, CPU-entry/exit identity, migration detection, pending
scheduler and softirq state, and preemption state.  On x86 it additionally
reports architecture interrupt, IPI, TLB, deferred-reschedule,
deferred-call-function, NMI, machine-check exception, and machine-check
poll counter deltas, plus native x86 TLB flush target batches directed at
the CPU during accelerator ownership, including the ring-3 process-mm
window;
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

The initial workload selector supports ``timestamp``, ``memmove``, and the
ABI 22 ``user-region`` proof.  The
memmove workload allocates and touches two bounded kernel buffers before the
accelerator entry point, then copies between them once per timing period.  It
is an oslat-like non-networked workload for measuring execution-source
isolation while avoiding page faults and system calls in the accelerator
interval.  The buffers are kernel-owned; this workload does not yet provide a
protected user address space or a user-supplied accelerator binary.

ABI version 22 supports the x86-only ``user-oslat`` workload.  The companion
forks
a worker so the accelerator task has a distinct ``mm_struct``, pins that task
to the target CPU, and supplies page-aligned executable-image and private-stack
ranges.  The kernel validates the VMAs, prefaults and pins their pages, holds
the worker address space's write-side mapping lock for the active epoch, and
returns the worker to ring 3 with the saved user register frame and IF clear.
The driver retains the opener's ``mm_struct`` as the companion identity and
rejects admission if the worker still shares that address space.
The image polls the read-only control mapping for the stop request, records
TSC-derived samples and histograms in an attached shared region, and uses
``CPU_ACCEL_IOC_USER_EXIT`` as its only terminal system call.  The kernel
restores the original ``START`` return frame, releases the address-space
admission lock, and then restores Linux-owned IRQ/workqueue state.

A kernel-initiated maintenance stop can make the image reach ``USER_EXIT``
before the companion's deadline ``STOP``.  The companion accepts that stop's
``EALREADY`` result only after confirming that the worker has already exited,
so a cooperative maintenance stop is not misreported as a controller failure.

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
The module rejects Xen PV and HVM guests at initialization: this backend's
staged IPI vectors and native TLB ownership hooks do not use Xen's IPI and
TLB-flush paths.
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

ABI version 22 replaces that fixed workload-data mapping with explicit shared
regions.  The companion creates a zeroed, page-aligned ``vmalloc_user`` region,
maps it by opaque token, attaches it while the device is idle, and snapshots
the attached set during ``CONFIG``.  Manager and owner mappings are exact-size,
role-specific, non-executable ``MAP_SHARED`` mappings.  Region contents are
opaque workload bytes; ``user-region``, ``user-cyclictest``, and
``user-oslat`` use a private header and completion sequence in those bytes.
Region epochs change on attach, detach, reset, and reuse so the workload can
reject stale mappings or results.  See
:doc:`cpu-accel-memory` for the permission, lifetime, and isolation model.

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
``irq_quarantine_blockers`` reports blockers from a rejected start. This does
not suppress IPIs, local timers, NMIs, or firmware activity. Generic internal
affinity updates filter reserved CPUs, and public forced-affinity requests
reject them. Quarantine restore may reapply a saved mask only during failed
admission rollback or after the owner has stopped. Managed IRQs with no
available target remain shut down and restart when ownership is released.
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
``cpu-accel-testctl`` provides a minimal command-line exerciser::

  make -C tools/cpu_accel
  sudo make -C tools/cpu_accel test
  CPU_ACCEL_REPEATS=5 CPU_ACCEL_LOAD_CPUS=2-7 \
    sudo make -C tools/cpu_accel test
  sudo insmod drivers/cpu_accel/cpu_accel.ko
  sudo tools/cpu_accel/cpu-accel-testctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --persistent
  sudo tools/cpu_accel/cpu-accel-testctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --workload memmove --work-bytes 4096
  sudo tools/cpu_accel/cpu-accel-testctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --workload user-region --work-bytes 4096
  sudo tools/cpu_accel/cpu-accel-testctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --workload user-oslat
  sudo tools/cpu_accel/cpu-accel-testctl run --cpu 1 --duration-ms 5000 \
    --period-us 1000 --workload user-hang --escape-after-ms 20
  sudo tools/cpu_accel/cpu-accel-testctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --quarantine-irqs

The focused function-graph teardown test can also overlap BPF JIT/seccomp
filter allocation and teardown with an active owner. Build the bounded churn
helper and set ``CPU_ACCEL_SECCOMP_CHURN`` to enable this additional workload::

  make -C tools/cpu_accel test-seccomp-jit-churn
  sudo env CPU_ACCEL_SECCOMP_CHURN=tools/cpu_accel/test-seccomp-jit-churn \
    CPU_ACCEL_SECCOMP_CPUS=3-7 sh tools/cpu_accel/test-ftrace-maintenance.sh

The repeated hotplug race harness supports CPU-up and CPU-down modes. It runs
the matching focused owner test multiple times and can add one busy-loop worker
per comma-separated load CPU. It records each run's result, owner-active
interval, and CPU-down wait in a summary TSV::

  sudo make -C tools/cpu_accel test-hotplug-race
  sudo make -C tools/cpu_accel test-hotplug-down-race
  sudo env CPU_ACCEL_REPEAT_COUNT=8 CPU_ACCEL_LOAD_CPUS=3,4,5 \
    make -C tools/cpu_accel test-hotplug-race

The global TLB-flush test can load a module already installed for the running
kernel. This is useful when a matching probe module cannot be built locally.
Choose a module that is safe to load and unload; module parameters are passed
after the module name::

  sudo env CPU_ACCEL_TESTCTL=/tmp/cpu-accel-testctl \
    sh tools/cpu_accel/test-tlb-flush.sh --modprobe dummy numdummies=0

Without ``--modprobe``, the test continues to load its matching no-op probe
module with ``insmod``.

To validate synchronous call-function IPIs during ring-3 ownership, run the
prewarmed seccomp/JIT overlap test with a small two-CPU worker set::

  sudo env CPU_ACCEL_TESTCTL=/tmp/cpu-accel-testctl \
    CPU_ACCEL_SECCOMP_CHURN=/tmp/test-seccomp-jit-churn \
    sh tools/cpu_accel/test-call-function-owner-stop.sh

The synchronous per-CPU work test also covers SLUB cache shrink, the RCU
sheaf drain used when a cache is destroyed, and network backlog flushing.
The network case creates a temporary veth pair, directs receive-side scaling
to an online housekeeping CPU accepted by the kernel's RPS filter, floods it
while the owner is active, and unregisters the device. This can be a different
CPU from the one used by the other cases because RPS excludes isolated CPUs.
Set ``CPU_ACCEL_NET_CPU`` to the RPS housekeeping CPU and
``CPU_ACCEL_WORK_CPU`` to a different online CPU when the owner CPU is
isolated; on a host where only CPU 0 is RPS-eligible, keep CPU 0 as
``CPU_ACCEL_NET_CPU`` and choose another online CPU for the work CPU.
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

The resctrl owner-stop test mounts resctrl if necessary, creates a temporary
control group, assigns the target CPU while a bounded ``user-oslat`` owner is
active, and removes the group afterward.  It requires a kernel and platform
with resctrl CPU-domain updates::

  sudo make -C tools/cpu_accel test-resctrl-owner-stop

The RFS owner-stop test creates a temporary veth pair across a network
namespace, enables a non-threaded backlog path, primes one UDP flow on the
target CPU, and then sends packets on that same flow while a bounded
``user-oslat`` owner is active.  It requires ``iproute2``, the built-in RFS
flow helper, and the ``veth`` module; the helper binds its sender to the veth
device so local-address routing cannot silently use loopback.  It restores
the global RFS flow-table count to zero afterward::

  sudo make -C tools/cpu_accel test-rfs-owner-stop

The veth teardown owner test uses the same temporary RFS flow setup, queues
packets while the selected CPU is owned, and deletes the temporary veth pair
from the work CPU while the flow sockets remain open.  It requires the same
helper and modules and verifies that network-device teardown completes without
requesting owner stop::

  sudo make -C tools/cpu_accel test-veth-teardown-owner

The veth newlink fault test starts an owner and attempts to create a temporary
veth whose peer name collides with loopback.  Function-graph tracing is
filtered to ``veth_newlink`` and ``register_netdevice`` to prove that the
caller's ``-EEXIST`` unwind path runs on the work CPU; the failed creation must
leave no partial device and must pair the generic owner-stop request with a
cooperative owner exit::

  sudo make -C tools/cpu_accel test-veth-newlink-fault-owner

The netdevsim reload owner test creates a temporary one-port ``netdevsim``
device, fails a ``devlink`` driver reload while the owner is active, and then
reloads successfully after ownership ends. It traces the netdevsim reload and
netdevice registration/unregistration paths on the work CPU and verifies that
the owner completes without an escape, context switch, migration, or kernel
error::

  sudo make -C tools/cpu_accel test-netdevsim-reload-owner

The netdevsim teardown owner test creates a temporary one-port ``netdevsim``
device and deletes its bus device while the owner is active. It traces
``device_unregister()``, ``nsim_drv_remove()``, and ``unregister_netdevice()``
on the work CPU and verifies that the bounded owner completes without an
escape, context switch, task migration, or kernel error::

  sudo make -C tools/cpu_accel test-netdevsim-teardown-owner

The netdevsim rebind owner test unbinds and rebinds the temporary device's
driver while the owner is active. It traces the driver-core attach/detach
entry points and the netdevsim probe/remove and netdevice registration paths
on the work CPU, then verifies that the device is restored and the bounded
owner completes without an escape, context switch, migration, or kernel
error::

  sudo make -C tools/cpu_accel test-netdevsim-rebind-owner

The netdevsim SR-IOV owner test switches the temporary device to switchdev
mode, creates and removes one VF, and returns to legacy mode while the owner
is active. It traces the e-switch mode changes, VF port add/remove paths, and
netdevice registration/unregistration on the work CPU, then verifies that the
device and its PF port remain intact and the bounded owner completes without
an escape, context switch, migration, or kernel error::

  sudo make -C tools/cpu_accel test-netdevsim-sriov-owner

The dirty-writeback owner test writes a bounded temporary file from a
housekeeping CPU while a bounded ``user-oslat`` owner is active, flushes the
containing filesystem, and requires inode-matched dirty-folio and
writeback-completion traces before owner exit.  It requires the writeback
tracepoints and a writable regular filesystem at ``/var/tmp``::

  sudo make -C tools/cpu_accel test-dirty-writeback-owner

The migration stress owner test moves one private anonymous page back and
forth between two NUMA memory nodes while a reader on a third online CPU
keeps touching it and a bounded ``user-oslat`` owner remains active.  It
requires two NUMA nodes with memory, the migration tracepoints, and distinct
owner, work, and reader CPUs; set ``CPU_ACCEL_MIGRATION_WORK_CPU`` and
``CPU_ACCEL_MIGRATION_READER_CPU`` to override the defaults.  The owner must
complete without an escape, context switch, task migration, or kernel error::

  sudo make -C tools/cpu_accel test-migration-owner

The amd-pstate shared-memory owner test switches a per-CPU EPP policy while
that CPU is owned and verifies that the policy write completes immediately,
does not request owner stop, and leaves the owner running.  It is specific to
platforms whose ``amd-pstate-epp`` driver uses shared-memory CPPC rather than
the x86 per-CPU MSR path::

  sudo make -C tools/cpu_accel test-amd-pstate-shmem-owner

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
  sudo tools/cpu_accel/test-call-function-single-owner-stop.sh irq-work

The ``irq-work`` mode requires a CPU-cycles PMU event on the target CPU. The
probe enables the event before owner entry and arms its NMI handler during
ring-3 ownership; the NMI queues local IRQ work, the probe checks that its
callback count stays zero, then a synchronous call stops the owner and the
probe checks callback completion.

Fault injection uses the ``user_escape_drop_count`` module parameter, which
suppresses the NMI of that many upcoming user-escape attempts on the sending
side. A suppressed attempt fails its one-second deadline exactly like a
genuinely lost NMI, so the controller's retry loop is what recovers the run.
Consuming the drop on the sender keeps unrelated NMIs from eating a drop
slot; an earlier receiver-side design let an external NMI arriving during
the request window consume a drop nondeterministically. The focused test
runs a bounded ``user-hang`` owner, an independent NMI rescue helper as a
five-second backstop, and asserts the exact retry accounting::

  sudo sh tools/cpu_accel/test-user-escape-fault.sh
  sudo env CPU_ACCEL_ESCAPE_DROP_COUNT=2 \
    sh tools/cpu_accel/test-user-escape-fault.sh

``CPU_ACCEL_ESCAPE_DROP_COUNT`` accepts 0 through 3; the test issues
``drop + 1`` escape attempts and requires ``recovery_attempts`` equal to
``drop + 1``, ``user_escape_count`` of exactly one, no unclaimed-NMI
diagnostics on the target CPU, and full consumption of the drop parameter.
The test temporarily selects ``never`` in transparent hugepage's ``enabled``
control and restores the previous mode after the run: ``khugepaged``'s
``lru_add_drain_all()`` stops every owned CPU on its scan period and would
otherwise end the run before the injected window completes. On 2026-10-29,
drop counts 0, 1, and 2 each passed on
``7.3.0-rc3-accel-tlbfix-02150-gde9255a04ef7`` with the rebuilt driver,
including a ten-run drop-2 stability loop with no kernel-error match.

The owner-death test uses the test-only ``user-death`` workload as a safe
stand-in for a fatal user-mode machine check. It waits for the ring-3 owner
to become active, kills the worker, and requires the
``sched_process_exit`` death work to clear the image, restore Linux mode,
record ``-ESRCH``, and leave the device in ``ERROR`` without an escape NMI::

  sudo make -C tools/cpu_accel test-user-death-owner

The fault-injection coverage by required event is: a lost or suppressed
escape NMI (this test, drop counts 0 through 3); the timed escape fallback
(every bounded owner-stop test reaches its timed escape); admission
rejection under an enabled lockup detector (``test-watchdog-owner-admission.sh``);
and the ``stop_machine()`` CPU-up rendezvous returning ``-EBUSY`` while its
caller continues (return-probe evidence on candidate build ``#65``). A
kernel-initiated stop during a fault-injection run still outranks the
injected window: the owner-stop contract stops the owner first, the run
reports the stop path's accounting instead of the retry loop's, and the test
treats such a run as failed rather than passing it. General exception and
NMI containment beyond the escape path, and machine-check injection, remain
open: the in-tree MCE injector's software mode only decodes a synthetic
record, while hardware mode requires a disposable recovery-capable system.
The source-level classification and owner-death test cover the user-mode
``#MC`` task-death path; actual hardware machine-check injection remains
deferred.

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
       It does not cover IPIs, local timers, NMIs, or firmware. Generic
       internal updates filter reserved CPUs, and public forced-affinity
       requests reject them; forced quarantine restore is limited to rollback
       or owner-stopped teardown. Managed IRQs with no available target remain
       shut down until ownership is released.
     - ``test-cpu-accel.sh`` has an optional ``--quarantine-irqs`` path;
       it checks moved IRQs, zero blockers, and zero measured ``irq_count`` for
       kernel, shared-memory, and ring-3 workloads. IRQ KUnit coverage checks
       default startup/internal updates and managed shutdown/restart around an
       active reservation. ``test-irq-storm.sh`` applies bounded ping-flood
       traffic to a selected NIC while a persistent timestamp workload owns
       the target CPU; it checks effective IRQ affinities and per-vector
       delivery deltas as well as the accelerator IRQ counters. Run it via
       ``make -C tools/cpu_accel test-irq-storm`` with
       ``CPU_ACCEL_CPU``, ``CPU_ACCEL_IRQ_INTERFACE``, and
       ``CPU_ACCEL_IRQ_STORM_HOST`` set.
       On ``accelerator-dev``, kernel
       ``7.3.0-rc3-accel-tlbfix-phase8-irq-d08dba1-02098-gd08dba1bad4f``
       booted through the one-time GRUB entry. Because
       ``CONFIG_KUNIT_DEFAULT_ENABLED`` is unset, ``kunit.enable=1`` was
       required: ``irq_test_cases`` passed 6/6, including both accelerator
       affinity/startup cases, and ``refcount_interrupt`` passed 4/4.
       ``test-irq-storm.sh`` passed on ``ens18``/CPU 1 with a 1,000-packet
       ping flood: 1,000 replies, 2,002 MSI deliveries, zero delivered to
       CPU 1, ``irq_count=0``, five IRQs quarantined, zero blockers, and clean
       STOP (state 4, mode 0). Maximum measured lateness was 11,606 ns. IRQ 38
       was temporarily placed on CPU 1 to exercise the NIC output vector and
       restored to CPU 3 after the run. A trial on CPU 3 was safely rejected
       because managed virtio-blk IRQ 43 blocked quarantine; affinity rollback
       succeeded. No soft lockup, hung-task, or RCU-stall report appeared in
       the candidate boot.
     - Software IRQ injection is not used because the x86 injection path can
       prematurely complete an affinity change.
   * - Local timers and timer activity
     - **Deferred only while maskable interrupts are disabled.** There is no
       general timer ownership or housekeeping-routing policy for an active
       CPU. Timer migration maintenance requests owner stop.
     - On 2026-10-28, the ``test-sync-work-owner-stop.sh``
       ``timer_migration`` case passed on
       ``7.3.0-rc3-accel-tlbfix-phase8-irq-d08dba1-02098-gd08dba1bad4f``.
       The CPU1 owner-stop request came from a cpuset partition transition;
       its triggered stack contained ``housekeeping_update()`` and
       ``cpuset_partition_write()``, followed by the matching owner-exit event.
       The trigger completed in 17 ms. The result reported
       ``state=7``, ``recovery_state=2``, ``user_active_ns=15253680``,
       ``timer_softirq_count=0``, and ``hrtimer_softirq_count=0``. The run's
       dmesg and kernel-error deltas were empty.
       ``make -C tools/cpu_accel test-timerlat-owner`` pairs a 500 ms
       CPU1 timerlat baseline with a ring-3 owner interval on the same kernel.
       On 2026-10-28 it passed on
       ``7.3.0-rc3-accel-tlbfix-phase8-irq-d08dba1-02098-gd08dba1bad4f``.
       The baseline had 503 IRQ and 503 thread samples at a 1 ms period; IRQ
       p50/p95/p99/max were 2,160/21,420/27,065/34,946 ns, and thread
       p50/p95/p99/max were 6,280/38,868/46,572/51,524 ns. During the
       253,144,053 ns owner interval, one IRQ sample measured 252,562,339 ns
       of latency; the maximum IRQ gap in the owner trace was 253,559,000 ns.
       The maximum thread latency was 252,583,299 ns and was recorded just
       after the active interval. The owner escaped once and recovered with
       ``recovery_state=2`` and one recovery attempt; no kernel warning or
       error appeared. The userspace controller is pinned to CPU0 so its timed
       escape remains runnable while CPU1 is owned.
       On 2026-10-28, the timerlat harness also passed on candidate build
       ``#66`` (``e406bf98cb01``) with a 100 us period on owned CPU 2. The
       500 ms baseline recorded 5,036 IRQ and 5,036 thread samples; IRQ
       p50/p95/p99/max were 1,404/19,652/27,125/44,033 ns, and thread
       p50/p95/p99/max were 4,505/35,994/41,646/63,794 ns. During the
       252,899,414 ns owner interval, one timerlat IRQ sample was observed;
       its maximum latency was 252,811,508 ns and the maximum IRQ gap was
       252,911,000 ns. The accelerator reported zero timer and hrtimer
       softirq-vector dispatches.
     - This is a high-rate periodic timerlat probe,
       not a timer-callback storm or a latency bound; the softirq counters do
       not count individual timer callbacks.
   * - RCU callbacks
     - **Enforced policy.** x86 admission requires
       the target CPU to be both RCU-offloaded and outside
       ``HK_TYPE_RCU`` housekeeping CPUs. The per-CPU nocb callback kthread and
       its grace-period kthread are bound to the RCU housekeeping mask with
       ``kthread_bind_mask()``, preventing userspace affinity and cpuset
       changes from moving them onto an isolated owner CPU. The isolation
       policy comes from the boot-time ``nohz_full=`` mask; admission returns
       ``-EOPNOTSUPP`` if the target remains an RCU housekeeping CPU.
     - On candidate ``7.3.0-rc3-accel-tlbfix-02104-ge406bf98cb01-dirty``
       build ``#67``, ``test-rcu-nocb-admission.sh`` passed: CPU 1 was rejected
       because it lacks callback offload, while CPU 2 completed a 100 ms
       ring-3 run with 100 samples and no escape. Every ``rcuop/*`` and
       ``rcuog/*`` thread had ``Cpus_allowed_list=0-1,3-7``; a ``taskset``
       attempt to move a callback worker to CPU 2 was rejected because the
       kthread has ``PF_NO_SETAFFINITY`` set.
       On the same build, ``test-rcu-callback-routing.sh`` passed with 64 of
       64 callbacks completed on CPU 6 while the CPU 2 owner was active;
       ``target_callbacks=0``. The first callback completed 990 ms after the
       owner interval began, and the owner completed its 4.762 s interval with
       2,048 samples, no user escape, and ``recovery_state=0``. The harness
       waits for module-init reclamation to drain before holding its RCU reader:
       that cleanup uses ``execmem_free()`` and a global kernel TLB flush, which
       correctly stops an active owner if the test delays its grace period.
       Candidate build ``#68`` (``7.3.0-rc3-accel-tlbfix-02114-gb36ad836e944``)
       passed ``test-rcu-nocb-admission.sh``: CPU 1 was rejected for lacking
       callback offload and CPU 2 completed a 100 ms run with 100 samples and
       no escape. The 64-callback routing test also passed: all callbacks ran
       on CPU 0, ``target_callbacks=0``, and the first callback completed
       993 ms after owner entry. The owner completed 4.762 s with 2,048
       samples, ``user_escape_count=0``, and ``recovery_state=0``. Its first
       attempt ended early because the one-second module-init drain delay was
       insufficient: the outstanding ``execmem_free()`` TLB flush escaped the
       owner after the probe reader was released. Increasing that test delay to
       three seconds allowed the rerun to pass.
       The maximum 256-callback batch then passed three trials on build ``#68``
       with all callbacks completing during CPU 2 ownership and
       ``target_callbacks=0``. Two runs completed the 4.762 s owner interval;
       callbacks ran on CPUs 1 and 0, respectively, and both produced 2,048
       samples. Their observed ``max_lateness_ns`` values were 16,537 ns and
       569,976 ns (the latter with tracepoints and a CPU 4 busy loop). In the
       third run, all callbacks completed on CPU 3 by 20.3 ms after owner entry.
       ``khugepaged`` then requested a generic owner stop from
       ``__lru_add_drain_all`` (``tlb_flush_id=0``); the trace recorded owner
       exit 61 us later and recovery to Linux mode. A separate full-duration
       trace recorded three ``tlb_flush_wait`` begin/complete pairs while one
       owner was active, with no owner-stop request.
     - These bounded batches do not cover a sustained callback storm or
       establish a latency bound. ``rcu_softirq_count`` counts softirq-vector
       dispatches, not callbacks.
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
       this selective call-function path.
   * - Local IRQ work callbacks
     - **Deferred by interrupt masking.** The owner interval keeps IF clear,
       so the local ``IRQ_WORK_VECTOR`` cannot run until ownership exits.
       Remote ``irq_work_queue_on()`` requests use the call-function queue and
       follow its deferral policy. Ring-3 exit retires the mm owner and
       reconciles TLB state before enabling interrupts.
     - ``test-call-function-single-owner-stop.sh irq-work`` enables a
       CPU-cycles PMU event before owner entry, then arms its NMI handler during
       ring-3 ownership. The target NMI queues local IRQ work; the probe checks
       that its callback count stays zero, then a synchronous call stops the
       owner and the probe checks callback completion. Source review of
       ``irq_work_queue()``, ``arch_irq_work_raise()``,
       ``sysvec_irq_work()``, and accelerator exit ordering identifies the
       local-vector policy. The aggregate ``arch_irq_count`` includes
       ``IRQ_COUNT_IRQ_WORK`` deliveries.
       On 2026-10-28, one initial run returned ``-EIO`` without failure
       counters. After adding failure-only probe diagnostics, 16 consecutive
       bounded runs on
       ``7.3.0-rc3-accel-tlbfix-02114-gb36ad836e944`` passed. Each queued one
       item; callback count stayed zero during ownership and reached one after
       exit. Runs observed 4-37 PMU overflows and 104-115 ms probe completion,
       with no soft lockup, hung task, RCU stall, BUG, Oops, or panic. The
       initial failure remains unexplained.
     - No callback-storm, sustained-queue, or cross-class contention evidence;
       these runs do not establish a latency bound. ``arch_irq_count`` does
       not identify the callback or whether work remained pending at exit.
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
       A cross-file source pass compared per-CPU queue allocations with
       direct queue-wide flush, drain, and destroy calls. Reviewed textual
       overlaps were separate queue objects or already used owner-aware waits;
       no additional direct same-queue raw wait was identified.
       A cross-file per-item scan also matched the spelling ``&dma->work``
       between ddbridge and NetUP. Source review confirmed distinct item types
       and queues: ddbridge's ``struct ddb_dma`` uses the per-CPU ``ddb_wq``
       and owner-aware cancellation, while NetUP's ``struct netup_dma`` uses
       its own single-thread queue. This was a textual false positive, not a
       raw wait on ddbridge's per-CPU queue.
       A cross-file ``system_percpu_wq`` item-name scan returned four textual
       hits. The NVMe host and target ``ka_work`` items belong to distinct
       control types. The mt76 raw waits belong to the separate mt7915 and
       mt7996 device types; mt792x teardown already uses owner-aware
       cancellation. Source review found no additional raw wait on
       ``system_percpu_wq``.
       A producer-lifecycle review of LiquidIO's indirect per-CPU queue fields
       found that NIC_INFO link-status work is queued while holding
       ``link_status_lock``; teardown clears the interface pointer under the
       same lock before draining that work. The shared NIC_INFO dispatch
       callback can remain registered until device cleanup: taking the lock
       waits for an in-flight producer, and clearing the interface pointer
       prevents later callbacks from queueing for it. Synchronous delayed-work
       cancellation then completes any queued or running callback before the
       netdev is freed. RX OOM polling is queued from
       receive processing and may requeue itself; normal interface stop
       disables NAPI before the queue is drained. Channel reconfiguration
       previously ignored ``ndo_stop()`` errors and could proceed to queue
       teardown without that guarantee. ``lio_ethtool_set_channels()`` now
       aborts the reset and clears ``LIO_IFSTATE_RESETTING`` if the stop
       fails. The ``lio_ethtool.o`` object builds on this x86 host, but no
       LiquidIO hardware is available for runtime validation.
       A follow-up reviewed Vivid's global HDMI and S-Video control queues:
       per-device release cancels both work items, platform driver/device
       unregister precedes owner-aware queue destruction, and init unwind
       unregisters the device before draining. KASAN's workqueue UAF test
       intentionally queues its test item and already uses
       ``destroy_workqueue_with_sync_wait()``.
       ARM FF-A notification teardown also quiesces its producers before the
       owner-aware drain: ``cpuhp_remove_state()`` invokes the per-CPU disable
       callback for online CPUs, disabling both notification IRQs before the
       workqueue is destroyed. Notifier dispatch now holds SRCU protection
       through the client callback, so ``notify_relinquish()`` waits for
       in-flight callbacks before the client can destroy its own queue. The
       IRQ registrations and per-CPU data are released afterward. This is a
       source-only audit; no FF-A hardware is available for runtime
       validation.
       OP-TEE's SMC notification teardown requests asynchronous notification
       stop, removes the CPU-hotplug IRQ state, drains its owner-aware
       workqueue,
       and only then frees the per-CPU IRQ registration and data. Its FF-A
       teardown relinquishes the notifier before destroying the client
       workqueue; the FF-A SRCU barrier above now makes that unregister wait
       for callbacks already selected for dispatch. This audit is source-only;
       no SMC secure-world or FF-A runtime test was available.
       IDXD's event-log IRQ thread can queue fault work onto a user WQ's
       private per-CPU queue. WQ teardown now serializes with event-log
       processing, marks remaining entries for that WQ consumed, and drains
       the private queue before destroying it. The IDXD cdev object builds on
       this x86 host; no IDXD hardware is available for runtime validation.
       OcteonTX2 MCS IRQs come from a separate PCI function, but their
       enqueue path holds an RCU read section while using ``mcs->rvu``.
       RVU teardown unpublishes those pointers and waits for a grace period
       before draining and destroying the per-CPU queue. Later MCS IRQs see
       no RVU pointer and cannot enqueue. This is source-only; no MCS hardware
       is available for runtime validation.
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
       On 2026-10-28, ``test-perf-owner-stop.sh`` passed on
       ``7.3.0-rc3-accel-tlbfix-phase8-irq-d08dba1-02098-gd08dba1bad4f``
       with target CPU 2 and a 3-second escape delay. The per-CPU
       ``cycles:u`` helper completed in 253 ms and read 380 cycles. The trace
       paired owner 7's stop request from
       ``__smp_call_function_single`` with its exit 75 us later. The run
       reported ``state=7``, ``recovery_state=2``, ``user_escape_count=1``,
       ``arch_call_function_deferred=1``, and ``migration_detected=0``; the
       captured kernel-error and new-dmesg logs were empty. The test also
       passed on candidate build ``#67``
       (``7.3.0-rc3-accel-tlbfix-02104-ge406bf98cb01-dirty``). The helper
       completed in 254 ms and read 211 cycles; the trace paired owner 10's
       stop request from ``__smp_call_function_single`` with its exit 79 us
       later. The run reported ``state=7``, ``recovery_state=2``,
       ``user_escape_count=1``, ``arch_call_function_deferred=1``, and
       ``migration_detected=0``, with no kernel-error records. Three
       additional serial runs passed, each taking 253--254 ms and reading
       192--219 cycles. Every run verified the owner-stop and exit trace pair
       and an empty kernel-error log.
     - The four bounded runs cover one hardware-event install/close path only;
       they do not cover high-volume or concurrent PMU churn, task-scoped
       events, PMI delivery during ownership, or a comprehensive
       tracing-operation matrix.
   * - Resctrl changes
     - **Owner-stop for synchronous remote CPU callbacks.** The x86 domain
       programming paths use ``smp_call_function_any(..., 1)`` and
       ``on_each_cpu_mask(..., 1)``; the generic synchronous-call hooks reserve
       each remote CPU and request owner exit before waiting for its callback.
       Task-group changes use ``smp_call_function_single()`` on the moved
       task's current CPU and follow the same generic owner-stop policy.
     - Source review covers the resctrl callback sites and their path through
       ``arch_smp_sync_wait_begin/stop/end``. The call-function owner-stop
       tests cover that generic callback machinery. On 2026-11-09,
       ``test-resctrl-owner-stop.sh`` passed on simon.lab
       (``7.3.0-rc3-accel-tlbfix-gd16a5ac600cc-dirty``, ABI 22): assigning
       CPU 3 to a temporary resctrl group completed in 14 ms while the owner
       was active.  The trace paired an owner-stop request from
       ``smp_call_function_many_cond()`` with owner exit 24 us later; the
       owner reported ``state=3``, no escape, no context switch, no migration,
       one deferred call-function IPI, and no matched kernel error.
       On 2026-11-09, ``test-resctrl-task-owner-stop.sh`` passed on
       simon.lab: moving the isolated owner task into a temporary resctrl
       control group from CPU 0 completed in 14 ms while CPU 3 was owned.
       Function-graph tracing recorded ``_update_task_closid_rmid()`` on
       CPU 3.  The task-group write used
       ``__smp_call_function_single()``, requested owner stop, and owner
       exit followed 22 us later.  The owner completed with ``state=3``,
       777288 samples, a 140 ns maximum, no escape, context switch,
       migration, or architecture IPI, and an empty kernel-error log.
       On 2026-11-09, ``test-resctrl-monitor-owner-stop.sh`` passed on
       simon.lab: reading ``llc_occupancy`` for a CPU-3-scoped monitor group
       from CPU 0 completed in 2 ms while CPU 3 was owned.  Function-graph
       tracing recorded ``mon_event_count()`` on CPU 0.  The monitor read used
       ``smp_call_function_many_cond()``, requested owner stop, and owner exit
       followed 22 us later.  The owner completed with ``state=3``,
       49075687 samples, a 200 ns maximum, no escape, context switch,
       migration, or architecture IPI, and an empty kernel-error log.
     - The tests cover the CPU-assignment/domain-update and task-group move
       and monitor-read paths. Other resctrl operations remain untested, and
       there is no resctrl-specific counter for those paths.
   * - CPU hotplug attempts
     - **Owner-stop before online or offline transitions.** The current source
       closes owner admission and drains active owners before taking the
       CPU-hotplug write lock. CPU-up carries a target-CPU reservation through
       its inactive-CPU cache rendezvous.
     - ``test-cpu-accel.sh`` starts an active owner, requests offlining from
       CPU 0, and checks that the owner exits before the CPU goes offline.
       ``test-stop-machine-inactive-owner.sh`` now checks the CPU-up gate,
       owner-exit ordering, and successful inactive-CPU rendezvous.
       On candidate build ``#65`` (``d08dba1``), the CPU-up test reproduced a
       safety failure: the inactive stop-machine try gate returned ``-EBUSY``,
       but the sysfs write returned success and CPU 1 became online while CPU
       2 still owned the accelerator. On candidate build ``#66``
       (``e406bf98cb01``), one run of the updated test passed: the trace showed
       the ring-3 owner stop request and exit before the CPU-up maintenance
       gate completed, followed by a successful inactive-CPU rendezvous and
       CPU 1 online. The owner returned through the expected NMI escape path
       with recovery successful and 31 ms of active time; the timed escape
       fallback was not reached. Candidate build ``#67`` passed two repeat
       runs: with a 3 s configured run the owner exited after 35.658 ms; with
       a 5 s configured run it exited after 30.457 ms. Recovery succeeded in
       both runs and CPU 1 came online. Candidate build ``#68`` passed one
       3 s run:
       owner active time was 34.544 ms, recovery succeeded, and CPU 1 came
       online after owner exit.
       On 2026-10-28, four traced 3 s runs passed on kernel build ``#02114``
       (``b36ad836e944``), with a busy loop pinned to CPU 4. Each trace showed
       ``begin < owner_stop_request < owner_exit_complete < begin_done < try
       < stop``, a successful inactive-CPU reservation and stop-machine
       return, and CPU 1 online after cleanup. All four runs recorded the NMI
       escape with Linux mode restored and no recovery attempt. Owner active
       time ranged from 29.071 to 37.605 ms; the observed stop-request-to-owner
       exit interval ranged from 11 to 16 us, and maintenance begin-to-return
       ranged from 25 to 29 us. No soft-lockup, hung-task, or RCU-stall alert
       appeared. These four samples are observations, not a worst-case bound.
       A prior attempt with a stale controller reached its timed escape before
       the test observed an active marker; it never launched CPU-up and is not
       included in this count.
     - ``test-hotplug-race.sh`` provides bounded repeated CPU-up and CPU-down
       harnesses with optional concurrent load. Runtime reports for both modes
       and a run-specific latency-tail report remain open.
   * - ``stop_machine()`` requests
     - **Owner-stop on sleepable paths; reject unscoped inactive rendezvous.**
       The public ``stop_machine()`` wrapper drains owners before taking
       CPU-hotplug locks. Each in-tree x86 caller of
       ``stop_machine_cpuslocked()`` has a caller-side maintenance gate before
       its locks and rendezvous. The cache-CPU-online rendezvous can proceed
       inside the matching CPU-up reservation after owners have exited; an
       unscoped inactive-CPU call still uses the nonblocking try gate. The
       cache-CPU-online caller skips that rendezvous when cache-control setup
       is disabled or delayed AP initialization remains active. No in-tree
       x86 caller of ``stop_core_cpuslocked()`` was found.
     - The callsite review covers CPU hotplug, MTRR updates, late microcode,
       TDX module installation, and cache initialization. Existing CPU
       hotplug and ftrace tests cover selected owner-stop paths. On candidate
       build ``#65``, return probes confirmed that the CPU-up cache rendezvous
       returned ``-EBUSY`` while its caller nevertheless brought the CPU
       online. On candidate build ``#66`` the updated trace-order test passed:
       ``begin < owner_stop_request < owner_exit_complete < begin_done < try
       < stop``, with the reservation returning true and stop-machine returning
       success. Candidate build ``#67`` repeated that trace order, with the
       inactive-CPU reservation returning true and stop-machine returning
       success. Candidate build ``#68`` passed one more traced CPU-up run with
       the same ordering, reservation true, and inactive stop-machine return
       value zero. Kernel build ``#02114`` passed four additional loaded runs
       with that trace order and successful inactive-CPU rendezvous; the
       maximum observed owner-stop-request-to-exit interval was 16 us.
     - Higher-volume hotplug, caller lock-order fault injection, a general
       ``stop_machine()`` fault injector, and disposition of out-of-tree direct
       ``stop_machine_cpuslocked()`` callers remain open.
   * - Frequency, idle, thermal, power-policy, and watchdog changes
     - **Watchdog admission is rejected when an enabled lockup detector
       covers the target CPU; in-tree x86 per-CPU MSR policy writes are
       waited call-function callbacks subject to the sync-callback gap;
       cpuidle driver maintenance requests owner exit and coupled-idle
       pokes are deferred; autonomous hardware frequency and thermal
       behavior is outside the kernel policy envelope.** Soft-lockup
       watchdog reconfiguration
       uses ``smp_call_on_cpu()`` for per-CPU timer work. A remote call
       requests owner exit and waits for that work to complete. The default
       watchdog mask uses timer-housekeeping CPUs, but the privileged
       ``watchdog_cpumask`` sysctl can add a possible CPU. The x86 owner-entry
       check rejects a CPU covered by an enabled hard- or soft-lockup detector.
       The hard-lockup perf-NMI callback can report a lockup with printk,
       backtraces, and optional ``hardlockup_panic``; an active detector cannot
       share an accelerator CPU.
     - Intel P-state policy updates can write a target CPU MSR through
       ``wrmsrq_on_cpu()``, which uses a waited ``smp_call_function_single()``.
       The x86 sync-wait hook requests owner exit after queueing the callback.
       A syscall from the accelerated user image enables interrupts before
       dispatch, so the queued IPI callback can run before
       ``CPU_ACCEL_IOC_USER_EXIT`` reconciles TLB state and completes owner
        exit. Waiting for the callback therefore does not prove that the policy
        operation runs after ownership ends; an owner-exit-before-callback
        handshake is required and is implemented and verified below.
        On 2026-10-29, one bounded run of
       ``test-call-function-single-owner-stop.sh irq-work`` confirmed that
       gap on kernel
       ``7.3.0-rc3-accel-tlbfix-02141-g1c9eb2e109a0``. With a 5 s user-hang
       owner on CPU 2, one 100 ms PMU NMI window, and one synchronous
       call-function request from CPU 3, the deferred IRQ-work callback ran
       after the synchronous wait returned and observed
       ``x86_cpu_accel_any_active()`` set: the probe's post-sync line
       reported ``irq_work_queued=1
       irq_work_callback_count_after_sync=1
       irq_work_callback_owner_active_count=1 callback_count=1``. By the
       happens-before chain recorded in
       ``phase8-handoff-2026-10-28.md``, the waited CSD callback therefore
       also ran before the owner's exit, including TLB reconciliation,
       completed. The run reported ``state=7``, ``recovery_state=2``,
       ``user_escape_count=1``, the paired owner-stop and owner-exit trace
       events, an empty kernel-error log, and clean module and owner
        cleanup; it was recorded as gap confirmation, and the
        owner-exit-before-callback handshake design is the required
        follow-up.
        On 2026-10-29, that handshake (Option C in
        ``phase8-owner-exit-handshake-design.md``) was implemented in
        ``de9255a04ef7``: the stop NMI handler completes the owner's
        remaining exit steps (``x86_cpu_accel_owner_exit()``, the
        local-branch ``x86_cpu_accel_tlb_reconcile()``, and
        ``x86_cpu_accel_owner_exit_finish()``) through the new
        ``x86_cpu_accel_user_stop_exit()`` before redirecting to the escape
        tail, so no vector can be serviced before the exit publishes; the
        release path stays idempotent and prefers the NMI-captured
        ``tlb_shootdown_targets`` snapshot for telemetry. On candidate
        kernel ``7.3.0-rc3-accel-tlbfix-02150-gde9255a04ef7``, four bounded
        ``irq-work`` probe runs each reported
        ``irq_work_callback_owner_active_count=0 callback_count=1`` with
        ``state=7``, ``recovery_state=2``, ``user_escape_count=1``, the
        paired owner-stop and owner-exit trace events, and empty
        kernel-error logs. The regression modes ``sync``,
        ``smp-call-on-cpu``, and ``work-on-cpu`` each passed with empty
        kernel-error logs, ``test-watchdog-owner-admission.sh`` passed
        (admission rejected with ``-EBUSY`` while the lockup detector
        covered CPU 2, mask restored on exit), and
        ``test-perf-owner-stop.sh`` passed with the per-CPU perf event
        stopping the owner before the timed escape. The sync-callback
         ordering gap is closed for the waited call-function path on this
         build.
         On 2026-10-30, the same four probe modes re-validated on
         ``7.3.0-rc3-accel-tlbfix-gb381f8577d1a-dirty`` (build #80): the
         ``irq-work`` mode reported
         ``irq_work_callback_count_while_owner=0
         irq_work_callback_owner_active_count=0 callback_count=1`` with
         ``state=7``, ``recovery_state=2``, and an empty kernel-error log;
         the ``sync``, ``smp-call-on-cpu``, and ``work-on-cpu`` regression
         modes each passed with ``callback_count=1`` and empty kernel-error
         logs. The sync-callback ordering gap remains closed on this build.
         A source review of the in-tree x86 per-CPU MSR paths found the same
       waited ``smp_call_function_single()`` mechanism (``wrmsrq_on_cpu()``
       and ``rdmsrq_on_cpu()`` in ``arch/x86/lib/msr-smp.c``) for the
       remaining power-policy writers: ``amd-pstate`` (CPPC_REQ, CPPC_REQ2,
       and PERF_CTL), ``p4-clockmod`` (THERM_CONTROL),
       ``speedstep-centrino`` (PERF_CTL), ``intel_epb``
       (``arch/x86/kernel/cpu/intel_epb.c``, ENERGY_PERF_BIAS),
       ``x86_pkg_temp_thermal`` (PACKAGE_THERM_INTERRUPT),
       ``intel-uncore-frequency`` (UNCORE_RATIO_LIMIT), and AMD HFI
       (WORKLOAD_CLASS_CONFIG and WORKLOAD_HRST). The read-side paths of
       ``acpi-cpufreq``, ``amd_freq_sensitivity``, ``coretemp``, and the
       drivers above use the waited ``rdmsrq_on_cpu()``. The user-facing
       MSR driver (``arch/x86/kernel/msr.c``) writes any online CPU's MSR
       through ``wrmsrq_safe_on_cpu()`` and carries the same
       queue-before-stop gap. CPU-idle driver register and unregister run
       ``cpuidle_setup_broadcast_timer()`` over the driver cpumask through a
       waited ``on_each_cpu_mask()``, so the generic sync hooks reserve each
       CPU and request owner exit before the callback; the coupled-idle poke
       instead queues ``smp_call_function_single_async()`` with no stop
       request and follows the deferred call-function policy.
       ``intel_powerclamp`` performs local residency-MSR reads only in this
       tree, and the legacy ACPI processor throttling driver is absent.
       CPU-frequency cooling updates a frequency-QoS request and may reach
       this driver path. Scheduler idle entry is reached through the idle
       task after its reschedule check; this does not classify idle
       policy changes or autonomous hardware frequency and thermal changes,
       which do not pass through these kernel callbacks.
       On 2026-11-09, ``test-amd-pstate-shmem-owner.sh`` passed on simon.lab:
       writing CPU 3's ``amd-pstate-epp`` governor from ``performance`` to
       ``powersave`` while CPU 3 was owned completed in 3 ms, produced an
       ``amd_pstate_epp_perf`` trace, and did not request owner stop.  The
       5 s owner completed with ``state=3``, no escape, no context switch,
       no migration, and an empty kernel-error log.  This validates only the
       shared-memory CPPC path on that host; the x86 per-CPU MSR policy
       writers above remain source-classified.
       On 2026-11-09, ``test-thermal-cooling-owner.sh`` passed on simon.lab:
       changing CPU 3's Processor cooling state from 0 to 2 while CPU 3 was
       owned completed in 15 ms and lowered ``scaling_max_freq`` from
       4,954,565 Hz to 2,972,739 Hz.  Function-graph tracing showed
       ``cpufreq_set_cur_state()`` and ``freq_qos_update_request()`` on the
       work CPU.  The frequency-QoS update requested the owner stop through
       ``smp_call_function_many_cond()``; owner exit followed 21 us later.
       The 5 s owner completed cooperatively with ``state=3``, 846451
       samples, a 140 ns maximum, no escape, context switch, migration, or
       architecture IPI, and an empty kernel-error log.  The cooling state
       and frequency ceiling were restored after the run.  This validates
       the thermal cpufreq-policy path; the x86 per-CPU MSR policy writers
       above remain source-classified.
       On 2026-11-09, ``test-cpuidle-state-owner.sh`` passed on simon.lab:
       disabling CPU 3's ``C1`` state while CPU 3 was owned completed in
       1 ms.  Function-graph tracing showed ``store_state_disable()`` on
       the work CPU.  The state change used the waited
       ``smp_call_function_many_cond()`` path: it requested the owner stop,
       and owner exit followed 21 us later.  The state was restored after
       the run.  The owner completed with ``state=3``, 953279 samples, a
       140 ns maximum, no escape, context switch, migration, or
       architecture IPI, and an empty kernel-error log.  This covers the
       runtime idle state-disable path; driver register/unregister and
       coupled-idle pokes remain source-classified.
      - ``test-watchdog-owner-admission.sh`` changes the watchdog mask to the
        selected CPU and expects owner entry to fail with ``-EBUSY``; it restores
        the prior mask on exit. The x86 object builds, and one bounded run passed
        on candidate kernel
        ``7.3.0-rc3-accel-tlbfix-02141-g1c9eb2e109a0`` and a repeated bounded
        run passed on
        ``7.3.0-rc3-accel-tlbfix-02150-gde9255a04ef7``. The power-policy MSR
        review is source-only, and the bounded irq-work runs above are
        observations, not a stress envelope or a latency bound.
       Runtime frequency tests, idle driver register/unregister, broader
       event counters, fault injection, and stress-envelope latency remain
       open.
    * - Microcode updates
      - **The complete update transaction is gated: runtime reload drains
        owners before the stop-machine rendezvous; early and hotplug paths are
        out of scope or caller-local; resume applies on the BSP only.**
      - 2026-10-29 source audit of all x86 microcode update paths (built-in,
        no module-load trigger). Runtime reload through
        ``/sys/devices/system/cpu/microcode/reload`` is the only cross-CPU
        update entry point: ``reload_store()`` in
        ``arch/x86/kernel/cpu/microcode/core.c`` takes the
        ``x86_cpu_accel_maintenance_begin()`` owner-drain gate before
        ``cpus_read_lock()`` and ``load_late_locked()`` reaches the
        ``stop_machine_cpuslocked()`` rendezvous on ``cpu_online_mask``
        (``core.c``), already audited under the ``stop_machine()`` row; the
        Intel NMI rendezvous variant self-NMIs each stopped CPU and the AMD
        variant invokes the update handler directly in the stopped context.
        Early boot loading is per-CPU before SMP, when no ownership exists.
        The CPU-hotplug callback only collects CPU information and manages
        the sysfs group; it performs no update and is synchronized with the
        update path by ``cpus_read_lock()``. The suspend-resume syscore
        callback applies cached firmware on the BSP only, inside the
        owner-drain span of the suspend row. No microcode update was run
        during Phase 8 validation.
      - No runtime update run is recorded; failure injection, event counters,
        and run-specific latency evidence remain open.
   * - Suspend and hibernate
     - **Owner-drain gate spans CPU freeze through thaw.** The gate nests
       with each CPU-hotplug transition and prevents admission on the retained
       primary CPU.
     - Source review covers the suspend, hibernate, and recovery call paths.
       No Phase 8 suspend or hibernate lifecycle run is recorded.
     - Failure injection and external continuity evidence remain open.
   * - Kexec and kexec-jump
     - **Normal kexec holds the owner-drain gate through its one-way
       transfer.** Kexec-jump uses the suspend CPU freeze path and therefore
       holds the gate until secondary CPUs are thawed.
     - Source review covers both paths. No Phase 8 kexec lifecycle run is
       recorded.
     - Failure injection and external continuity evidence remain open.
   * - Reboot and panic
     - **Terminal CPU-stop paths; no sleepable owner-drain gate.** Normal
       reboot sends the reboot vector and falls back to NMI; forced reboot can
       skip stopping other CPUs. Panic uses the crash CPU-stop path.
     - Source review covers the x86 stop paths. No Phase 8 reboot or panic
       lifecycle run is recorded.
     - Deterministic owner termination and external continuity evidence
       remain open.
   * - Memory pressure and reclaim
     - **Safe only on audited completion paths; otherwise synchronous.** A
       clean file-page reclaim probe waits for owner reconciliation. Dirty or
       writeback reclaim and migration remain synchronous.
      - ``test-tlb-generation.c`` exercises ``MADV_PAGEOUT``; sync-work tests
        cover LRU/SLUB drains and vmalloc purge. On 2026-10-30, a sustained
        5 s ``user-oslat`` run on
        ``7.3.0-rc3-accel-tlbfix-gb381f8577d1a-dirty`` with concurrent
        ``drop_caches`` and 256 MB page-allocation churn (three iterations)
        completed with ``state=3``, 5000 samples, p50=8 ns, p90=16 ns,
        p99=8192 ns, max=169377 ns, ``arch_tlb_shootdown_targets=3``,
        ``user_escape_count=0``, and an empty kernel-error log. The three
        TLB shootdowns from reclaim were absorbed without an escape.
        On 2026-11-09, a traced ABI-22 run on simon.lab used one 256 MB
        page-cache churn and ``drop_caches`` cycle while CPU 3 was owned.
        ``__lru_add_drain_all()`` requested the owner stop after 439.6 ms;
        owner exit followed 16 us later.  The owner completed cooperatively
        with no escape, context switch, migration, or arch IPI, and the worst
        observed sample was 290 ns.
        On 2026-11-09, ``test-dirty-writeback-owner.sh`` passed on simon.lab:
        a 64 MiB XFS temporary file was written and flushed from CPU 0 while
        CPU 3 was owned.  Inode-matched ``writeback_dirty_folio`` and
        ``writeback_single_inode`` traces recorded 16,384 written pages in
        121 ms, before owner exit.  The 10 s owner run completed with
        ``state=3``, ``stop_requested=0``, no escape, context switch, or
        migration, and an empty kernel-error log.
      - ``test-migration-owner.sh`` now provides the focused migration-stress
        harness. Modeled on the upstream selftest's ``move_pages()`` loop, it
        moves one private anonymous page between two NUMA nodes while a pinned
        reader touches it and a bounded owner remains active. The harness
        requires two memory NUMA nodes and a third online CPU; its runtime
        report remains pending because simon.lab has only NUMA node 0.
    * - Device-management operations
      - **Caller-local management is safe by construction; device-specific
        steering is rejected or routed to housekeeping at configuration time;
        the one residual steering path to an owned CPU is deferred.**
      - 2026-10-29 source audit. Management operations (driver-core
        probe/remove/bind/unbind, PCI/FLR and reset-control resets, sysfs
        handlers, netdev register/unregister) run in caller context and never
        execute code on the target CPU. RPS configuration is filtered by
        ``rps_cpumask_housekeeping()`` (``net/core/net-sysfs.c``), which
        intersects ``rps_cpus`` and ``rps_default_mask`` with the housekeeping
        mask and rejects an empty result, so RPS cannot steer to an isolated
        CPU; the veth/network-backlog case of
        ``test-sync-work-owner-stop.sh`` exercises this routed path. XPS
        selects the transmit queue on the current CPU only, so it performs no
        cross-CPU execution. NAPI and tasklets run on the current or IRQ CPU,
        which the IRQ-affinity quarantine excludes. The RFS flow table targets
        the CPU where the socket last received data and is not filtered at
        configuration time, so a flow recorded on an owned CPU keeps targeting
        it while owned; delivery then rides the general mechanisms, either
        ``smp_call_function_single_async()`` of the non-threaded backlog RPS
        IPI (``net/core/dev.c``) or a ``wake_up_process()`` of the per-CPU
        ``backlog_napi`` thread in threaded mode, both of which leave the work
        pending until the owner exits. Device operations that use waited or
        deferred call-function, workqueues, stop machine, or CPU hotplug
        inherit the corresponding row's disposition.
       - On 2026-10-30, a 3 s ``user-oslat`` run on
         ``7.3.0-rc3-accel-tlbfix-gb381f8577d1a-dirty`` with a concurrent
         NIC down/up cycle (``ip link set ens18 down`` / ``up``) and an ARP
         flush completed with ``state=7``, ``recovery_state=2``,
         ``user_escape_count=1``, ``arch_tlb_shootdown_targets=1``, 1583
         samples before the escape, and an empty kernel-error log. The NIC
         stop path requested an owner stop through the generic
         call-function mechanism; the operation completed and the owner
         recovered. On 2026-11-09, ``test-rfs-owner-stop.sh`` passed on
         simon.lab with the same ABI-22 kernel: one UDP flow was primed on
         owned CPU 3, and 32 packets on that flow were sent while a 5 s
         ``user-oslat`` owner was active. None arrived during a 500 ms
         observation window; all 32 arrived after owner exit. The owner
         reported ``state=3``, one deferred call-function IPI, no escape,
         no context switch, and no migration. The trace ordered the veth
         transmit and receive events before an owner exit with
         ``stop_requested=0``, and the kernel-error log was empty.
         On 2026-11-09, ``test-veth-teardown-owner.sh`` passed on simon.lab:
         after 32 queued packets remained undelivered for 500 ms during
         ownership, deleting the temporary veth pair from CPU 0 while the
         flow sockets remained open completed in 24 ms. The owner finished
         normally with ``state=3``,
         ``stop_requested=0``, no owner-stop request, no escape, no context
         switch, no migration, and an empty kernel-error log. This covers one
         bounded caller-local veth unregister/teardown case; the broader
         device-reset and teardown fault matrix remains open.
         On 2026-11-09, ``test-veth-newlink-fault-owner.sh`` passed on
         simon.lab: creating a veth whose peer name collided with loopback
         failed on CPU 0 in 15 ms with ``-EEXIST``, left no partial device, and
         traced both ``veth_newlink`` and ``register_netdevice`` on CPU 0.
         The ``register_netdevice`` path reached a waited
         ``smp_call_function_many_cond()`` owner-stop request.  The CPU 3 owner
         exited cooperatively 22 us after the stop request with ``state=3``,
         ``stop_requested=1``, no escape, no context switch, no migration, and
         an empty kernel-error log. This covers one bounded device-creation
         failure unwind; the netdevsim runtime report and broader device
         reset/teardown fault matrix remain open.
         ``test-netdevsim-reload-owner.sh`` provides the next focused reset
         path: it creates a temporary one-port netdevsim device, fails a
         ``devlink`` driver reload while an owner is active, and reloads
         successfully after ownership ends. Function-graph tracing records
         ``nsim_dev_reload_down()``, ``nsim_dev_reload_up()``,
         ``unregister_netdevice()``, and ``register_netdevice()`` on the work
         CPU; the owner must complete without an escape, context switch,
         migration, or kernel error. Its runtime report remains pending.
         ``test-netdevsim-teardown-owner.sh`` adds a distinct device-removal
         path: it deletes the temporary netdevsim bus device while the owner
         is active and traces ``device_unregister()``,
         ``nsim_drv_remove()``, and ``unregister_netdevice()`` on the work
         CPU. Its runtime report also remains pending.
         ``test-netdevsim-rebind-owner.sh`` adds a driver-bind lifecycle
         path: it unbinds and rebinds the temporary netdevsim driver while
         the owner is active and traces ``device_driver_detach()``,
         ``device_driver_attach()``, ``nsim_drv_remove()``,
         ``nsim_drv_probe()``, ``unregister_netdevice()``, and
         ``register_netdevice()`` on the work CPU. Its runtime report also
         remains pending.
         ``test-netdevsim-sriov-owner.sh`` adds an SR-IOV configuration
         path: it switches the temporary netdevsim device to switchdev mode,
         creates and removes one VF, and returns to legacy mode while the
         owner is active. It traces
         ``nsim_devlink_eswitch_mode_set()``,
         ``nsim_esw_switchdev_enable()``, ``nsim_esw_legacy_enable()``,
         ``nsim_drv_configure_vfs()``, ``__nsim_dev_port_add()``,
         ``__nsim_dev_port_del()``, ``register_netdevice()``, and
         ``unregister_netdevice()`` on the work CPU. Its runtime report also
         remains pending.
   * - Exceptions, NMIs, and machine checks
      - **NMI escape is owner-terminating recovery for the tested ring-3
        workload; user-mode ``#MC`` death is reconciled through task exit;
        kernel-mode ``#MC`` is domain-terminating; general events are not
        contained.** Recovery state, error, attempt
        count, timestamps, and successful escape count use fixed shared ABI
        fields. MCE injection is deferred on this recovery-sensitive VM:
        the in-tree injector's software mode only decodes a synthetic record,
        while hardware mode invokes ``#MC`` and may panic.
      - ``user_escape_count`` and recovery fields report the owner-termination
        experiment. ABI 23 adds separate x86 NMI, MCE-exception, and MCE-poll
        counter deltas, and ``test-user-escape-fault.sh`` now requires the
        successful escape to correlate with at least one NMI entry. On
        2026-10-29,
        ``test-user-escape-fault.sh`` passed on
        ``7.3.0-rc3-accel-tlbfix-02150-gde9255a04ef7`` with the sender-side
        drop parameter for counts 0, 1, and 2, including a ten-run drop-2
        stability loop: each run reported
        ``recovery_attempts`` equal to ``drop + 1``,
        ``user_escape_count=1``, no unknown-NMI diagnostics, no kernel-error
        match, and full drop consumption, with the transparent hugepage mode
        restored after each run. This validates the retry path for up to three
        deliberately suppressed escape NMIs. The earlier 2026-10-28
        receiver-side drop on
        ``7.3.0-rc3-accel-tlbfix-phase8-irq-d08dba1-02098-gd08dba1bad4f``
        demonstrated that an unrelated NMI can consume a drop slot and was
        replaced by sender-side suppression.
       - **MCE source classification (injection itself deferred).** A
         user-mode ``#MC`` on the owner (for example the user-reclaim workload
         reading a poisoned page) is dispatched by ``do_machine_check()``
         through ``queue_task_work()`` (``task_work_add(TWA_RESUME)``) to
         ``kill_me_maybe()`` or ``kill_me_now()``: the owner dies from
         ``memory_failure()`` page isolation plus SIGBUS, or a direct SIGBUS,
         at the next resume to user mode. The driver registers both
         ``sched_process_exit_early`` and ``sched_process_exit`` tracepoints:
         the early hook balances the accelerated owner's preempt/IRQ state and
         mm lock, while the death work clears ``user_image.active``, releases
         the pinned image, restores IRQ/workqueue/region state, records
         ``-ESRCH``, and transitions the device to ``ERROR`` in Linux mode.
         The NMI handler also checks ``current`` against the admitted owner,
         so a stale active flag cannot retarget an unrelated user task. A
         kernel-mode ``#MC`` (kernel owner
         or the owner's kernel entry/exit window) reaches ``mce_panic()`` and
         is domain-terminating for the whole system, so no owner
         reconciliation is needed there. Corrected machine checks (CMCI) are
         logged by the per-CPU ``mce_timer`` poll pass
         (``machine_check_poll()``): bounded timer and softirq activity on
         the owned CPU that does not change owner state and is counted by the
         IRQ and softirq telemetry.
       - The ``user-death`` workload is the focused test for this
         reconciliation path: it must report ``state=4 mode=0``,
         ``recovery_state=5 recovery_error=-3``, ``user_escape_count=0``, and
         ``migration_detected=0`` with no kernel-error match. ABI 23's
         exception counters are not yet runtime-validated on the rebuilt
         kernel; general NMI containment evidence and actual MCE injection
         remain uncovered. Hardware MCE injection requires a
         disposable recovery-capable test system. A kernel-initiated stop
        (for example ``khugepaged``'s ``lru_add_drain_all()``) outranks an
        injected drop window and ends the run early; the focused test
        disables transparent hugepages during the run to keep that source out
        of the window.
    * - No ordinary task or unbounded path; latency
      - **Measured envelope per bounded stress class; no hard bound.**
        Existing counters do not prove absence of every ordinary Linux or
        unbounded kernel path.
      - ABI 18 adds a 64-bin log2 lateness histogram to the shared struct;
        both owner-side sampling paths fill it and ``cpu-accel-testctl`` reports
        the non-zero bins plus p50/p90/p99/p999 upper-edge estimates.
        ``test-wcl-report.sh`` runs one bounded stress class at a time
        (transparent hugepages disabled for the matrix to keep
	``khugepaged`` out of the window) and records the distribution and
	counter correlation per class, including the ABI 23 NMI and MCE deltas.
        The default set is portable; selecting ``all`` or an individual class
        through ``CPU_ACCEL_WCL_CLASSES`` also permits the topology-specific
        device-lifecycle, ``migration``, ``net_backlog``,
        ``timer_migration``, and ``irq_storm`` rows.
	On 2026-10-29 on
        ``7.3.0-rc3-accel-tlbfix-02150-gde9255a04ef7``, the 5 s
        ``user-oslat`` baseline and five-busy-CPU load runs each produced
        5000 samples with p50=8 ns, p90=16 ns, p999=16384 ns, and maxima
        of 15137 ns and 16350 ns; the vmstat, lru, slub_shrink,
        ring_resize, call_function, and irq_work owner-stop runs each
        completed with their stop/exit trace pair and the expected
        deferred-work and IPI counters; and the timerlat baseline on CPU 2
        measured p99 of 7874 ns (irq) / 13000 ns (thread) with a 253 ms
        max-active timer IRQ during the owner interval, bounded by the
        owner stop. On 2026-11-09, the new ``memory_pressure`` class on
        simon.lab repeatedly wrote and read a 128 MiB tmpfs file from CPU 0
        while CPU 3 ran a 5 s ``user-cyclictest`` owner.  It produced 5003
        samples with p50=16 ns, p90/p99/p999=32 ns, max=19 ns, no IRQ,
        softirq, workqueue, architecture-IPI, TLB, context-switch, or
        migration event, and completed with ``state=3``.  The full report is
        ``Documentation/accel/phase8-wcl-report.md``.
        On 2026-11-09, the sync-work harness was fixed to keep trace events and
        ``function_graph`` in separate instances and to enable an owner-stop
        stack trace.  The vmstat case passed on simon.lab: writing
        ``vm.stat_refresh`` from CPU 0 stopped the CPU 3 owner in 3 ms, and the
        stack proved the request came through ``schedule_on_each_cpu()``.  The
        stop and owner-exit tracepoints were 10 us apart; the owner escaped and
        recovered with no context switch, migration, or matched kernel error.
        The LRU case also passed: ``drop_caches`` from CPU 0 stopped the CPU 3
        owner in 17 ms, the stack proved the request came through
        ``__lru_add_drain_all()``, and the stop and owner-exit tracepoints were
        9 us apart with the same clean owner telemetry.
        The SLUB-shrink and trace-ring-resize cases passed with 4 ms and 9 ms
        helpers; their stacks proved ``flush_all_cpus_locked()`` and
        ``ring_buffer_resize()``, respectively, and each stop-to-exit interval
        was 9 us.  The timer-migration case also passed on this
        ``rcu_nocbs``-covered boot: isolating CPU 1 stopped its owner in 11 ms,
        the stack proved ``housekeeping_update()``, the stop-to-exit interval
        was 9 us, and the cgroup and transparent-hugepage state were restored.
      - No hard bound; log2-bin percentiles are factor-of-two ceilings.
        The ``net_backlog`` class is deferred to a boot with an RPS-eligible
        owner CPU covered by ``rcu_nocbs`` and a separate trigger CPU; on
        simon.lab, ``nohz_full=1-11`` leaves CPU 0 as the only RPS-eligible
        housekeeping CPU, and the test reserves it for the trigger.  The
        attempted temporary-veth run therefore failed before an owner run and
        cleaned up.  ``irq_storm`` needs a test NIC.

The plan's disposition vocabulary is safe, routed to housekeeping, deferred,
rejected, or domain-terminating. Several rows remain unclassified or have only
selective policies; those are open audit findings, not implied rejections.
Phase 8 remains open until each activity has an enforced disposition, the
fault-injection suite and counters cover the required events, and a
run-specific worst-case latency report is recorded.
