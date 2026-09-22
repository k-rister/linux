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

The shared mapping contains the state, explicit Linux/accelerator transition
mode, selected workload, run timestamps, aggregate lateness, and up to
``CPU_ACCEL_MAX_SAMPLES`` timestamp samples.  ABI version 6 also
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

The watchdog is capped at five seconds and ``STOP`` is cooperative.  It can
recover this prototype's timestamp loop, but it cannot rescue arbitrary code
that fails to observe the shared stop state.  A malfunctioning kernel
implementation is not assumed to be recoverable without reverting to the
known-good kernel.  This lifecycle is the first step toward that model, but
the synchronous SMP dispatch still uses the normal IPI entry path and does
not suppress Linux-generated IPIs while the target is running.  The next
phase should validate the bounded memmove workload under loaded conditions,
then add a protected accelerator address-space model and explicit
shared-memory ownership before replacing the staged x86 handoff with direct
APIC ownership and explicit pending-IPI/TLB policy.  A stronger latency claim
must wait for those controls and for a defined recovery contract.
