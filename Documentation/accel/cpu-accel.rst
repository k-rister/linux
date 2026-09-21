.. SPDX-License-Identifier: GPL-2.0

====================================
Single-CPU accelerator prototype
====================================

The ``CPU_ACCEL`` driver is the first foundation prototype for a Linux
dataplane accelerator environment.  It is an experimental control plane for
one hotpluggable, non-boot CPU and is intended to make the control and
measurement interfaces concrete for a persistent accelerator ownership mode.

This version is deliberately not a complete isolated accelerator CPU.  It
uses a CPU-hotplug teardown callback as the transition boundary.  In
persistent mode the target CPU remains in the accelerator callback, with
preemption and local maskable interrupts disabled, until ``STOP`` or the
configured watchdog.  Normal Linux hotplug then completes taking it offline;
``EXIT`` brings it back online.  It does not yet provide a dedicated
architecture-independent persistent CPU state, a memory-protection domain,
explicit interrupt source ownership, TLB-shootdown suppression, or a hard
latency bound.  NMIs, SMIs, machine checks, pending IPIs, firmware activity,
and hardware execution effects remain outside this prototype's control.

Interface
=========

The driver creates ``/dev/cpu_accel`` with a fixed-size shared mapping and a
small ioctl interface:

* ``CONFIG`` selects an online CPU other than CPU 0 and supplies a duration
  and sample period.
* ``START`` explicitly enters the CPU-hotplug-backed accelerator workload.
  ``CPU_ACCEL_FLAG_PERSISTENT`` keeps ownership until STOP or the duration
  watchdog; the SDK tool exposes this as ``--persistent``.
* ``STOP`` requests an orderly exit at the next sample boundary.
* ``EXIT`` explicitly brings the target CPU back into Linux after the
  workload has stopped and hotplug has completed.
* ``RESET`` returns the device to its initial state after a completed run.

The shared mapping contains the state, run timestamps, aggregate lateness,
and up to ``CPU_ACCEL_MAX_SAMPLES`` timestamp samples.  The first ABI supports
``CPU_ACCEL_FLAG_IRQS_OFF`` and ``CPU_ACCEL_FLAG_PERSISTENT``.  A watchdog
termination is reported as ``CPU_ACCEL_STATE_WATCHDOG``.

The companion SDK in ``tools/cpu_accel`` wraps the device and
``cpu-accelctl`` provides a minimal command-line exerciser::

  make -C tools/cpu_accel
  sudo make -C tools/cpu_accel test
  sudo insmod drivers/cpu_accel/cpu_accel.ko
  sudo tools/cpu_accel/cpu-accelctl run --cpu 1 --duration-ms 100 \
    --period-us 1000 --persistent

The tool prints the shared result, including the maximum observed lateness.
The tool pins its control process to CPU 0.  CPU 0 is reserved for
control-plane work by this prototype and is rejected as a target.

Recovery and next steps
=======================

The watchdog is capped at five seconds and ``STOP`` is cooperative.  It can
recover this prototype's timestamp loop, but it cannot rescue arbitrary code
that fails to observe the shared stop state.  A malfunctioning kernel
implementation is not assumed to be recoverable without reverting to the
known-good kernel.  The next implementation phase should replace the
hotplug-callback ownership mechanism with a dedicated architecture-neutral
accelerator CPU lifecycle, plus validation of interrupt, workqueue, RCU,
timer, and TLB activity before attempting a stronger latency claim.
