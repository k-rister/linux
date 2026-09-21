.. SPDX-License-Identifier: GPL-2.0

====================================
Single-CPU accelerator prototype
====================================

The ``CPU_ACCEL`` driver is the first foundation prototype for a Linux
dataplane accelerator environment.  It is an experimental control plane for
one online, non-boot CPU and is intended to make the control and measurement
interfaces concrete before implementing full CPU mode transitions.

This version is deliberately not a complete isolated accelerator CPU.  It
does not offline a CPU, migrate or mask its interrupt sources, suppress TLB
shootdowns, provide a memory-protection domain, or provide a hard latency
bound.  During the bounded timestamp workload it disables preemption and
local maskable interrupts on the target CPU.  NMIs, SMIs, machine checks,
pending IPIs, firmware activity, and hardware execution effects remain
outside this prototype's control.

Interface
=========

The driver creates ``/dev/cpu_accel`` with a fixed-size shared mapping and a
small ioctl interface:

* ``CONFIG`` selects an online CPU other than CPU 0 and supplies a duration
  and sample period.
* ``START`` explicitly enters the bounded accelerator workload.
* ``STOP`` requests an orderly exit at the next sample boundary.
* ``RESET`` returns the device to its initial state after a completed run.

The shared mapping contains the state, run timestamps, aggregate lateness,
and up to ``CPU_ACCEL_MAX_SAMPLES`` timestamp samples.  The first ABI supports
only ``CPU_ACCEL_FLAG_IRQS_OFF``.

The companion SDK in ``tools/cpu_accel`` wraps the device and
``cpu-accelctl`` provides a minimal command-line exerciser::

  make -C tools/cpu_accel
  sudo insmod drivers/cpu_accel/cpu_accel.ko
  sudo tools/cpu_accel/cpu-accelctl run --cpu 1 --duration-ms 100 --period-us 1000

The tool prints the shared result, including the maximum observed lateness.
The control process should run on a non-target CPU.  CPU 0 is reserved for
control-plane work by this prototype and is rejected as a target.

Recovery and next steps
=======================

The workload is capped at five seconds and ``STOP`` is cooperative.  A
malfunctioning kernel implementation is not assumed to be recoverable
without reverting to the known-good kernel.  The next implementation phase
must add an explicit Linux-to-accelerator and accelerator-to-Linux CPU
lifecycle, with CPU-hotplug-style quiescence and validation of interrupt,
workqueue, RCU, timer, and TLB activity before attempting a stronger latency
claim.
