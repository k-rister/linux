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
execution; no CPU offline/online transition is required.  The dispatch is
currently implemented with ``smp_call_function_single()`` as a stepping stone
for a future architecture-specific direct entry/exit backend.

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
``CPU_ACCEL_MAX_SAMPLES`` timestamp samples.  ABI version 10 also
reports lifecycle entry/exit timestamps, interrupt and softirq deltas,
timer, hrtimer, RCU, and scheduler softirq deltas, current-task
context-switch deltas, CPU-entry/exit identity, migration detection, pending
scheduler and softirq state, and preemption state.  On x86 it additionally
reports architecture interrupt, IPI, and TLB counter deltas;
``arch_counters_valid`` identifies whether those counters are available.
The x86 staged backend is reported as ``CPU_ACCEL_BACKEND_X86_STAGED_IPI``;
it uses one normal IPI for entry and does not yet provide direct APIC entry.
Workqueue queue and execution tracepoints report activity targeted at the
accelerator CPU while it is running.
These are observations made by the prototype, not suppression or admission
controls for the corresponding activity.

The initial workload selector supports ``timestamp`` and ``memmove``.  The
memmove workload allocates and touches two bounded kernel buffers before the
accelerator entry point, then copies between them once per timing period.  It
is an oslat-like non-networked workload for measuring execution-source
isolation while avoiding page faults and system calls in the accelerator
interval.  The buffers are kernel-owned; this workload does not yet provide a
protected user address space or a user-supplied accelerator binary.

ABI version 10 adds the x86-only ``user-oslat`` workload.  The companion forks
a worker so the accelerator task has a distinct ``mm_struct``, pins that task
to the target CPU, and supplies page-aligned executable-image and private-stack
ranges.  The kernel validates the VMAs, prefaults and pins their pages, holds
the worker address space's write-side mapping lock for the active epoch, and
returns the worker to ring 3 with the saved user register frame and IF clear.
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

ABI version 10 retains the fixed-size shared-region mapping at
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
  sudo tools/cpu_accel/cpu-accelctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --quarantine-irqs

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
is the first step toward that model, but the synchronous SMP dispatch still
uses the normal IPI entry path and does not suppress Linux-generated IPIs
while the target is running.  The next phase should validate the bounded
memmove workload under loaded conditions and then measure successful and
failed ring-3 escape cases.
The proposed protected address-space and shared-memory contract is documented
in :doc:`cpu-accel-memory`; the internal region/epoch model and the first
prefaulted shared entry are now in place.  The x86 cooperative ring-3 image is
the first protected-address-space proof.  The next implementation steps are
to measure its entry/exit and active-mm behavior, define an escape/recovery
contract, and only then add direct APIC ownership or IOMMU-backed networking.
A stronger latency claim must wait for those controls and for a defined
recovery contract.
