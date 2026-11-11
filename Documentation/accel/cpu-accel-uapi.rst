.. SPDX-License-Identifier: GPL-2.0

========================
CPU accelerator UAPI
========================

This document is the normative contract for the CPU accelerator control UAPI.
The current ABI is 24 and the minimum supported ABI is 23. The ABI 24
control plane is frozen. The x86 ring-3 workload and image-admission
extension remains experimental and is not part of the frozen control-plane
contract.

Object model
============

``/dev/cpu_accel`` exposes one singleton administrative domain for the
prototype. Opening the device requires ``CAP_SYS_ADMIN`` and acquires exclusive
ownership. A descriptor may be passed to another process with ``SCM_RIGHTS``.
The process that opened the descriptor is the manager and owns the control
mapping; a worker with a different memory map may map owner regions.

Closing the last descriptor requests an orderly exit, tears down the lifecycle
thread when it is no longer active, releases regions and locked-memory
accounting, and clears session configuration. If the manager dies during an
active run, a replacement manager may reopen the device and read the terminal
state and mode before issuing ``CPU_ACCEL_IOC_RESET``.

Fork without ``execve()`` does not create a second domain. Descriptor passing
and deliberate inheritance are allowed, but the singleton-open gate remains
owned by the original descriptor. ``O_CLOEXEC`` is the SDK default; callers
must clear it deliberately when a worker needs the descriptor across
``execve()``.

ABI negotiation
===============

``CPU_ACCEL_IOC_GET_ABI_INFO`` returns the kernel's current, minimum, maximum,
and selected ABI versions. A newly opened descriptor selects ABI 23 by default.
``CPU_ACCEL_IOC_SET_ABI_VERSION`` selects an ABI in the supported range. The
selection is local to that descriptor.

ABI 23 clients use the core commands and do not issue the negotiation ioctls.
An ABI 24 SDK detects an ABI 23-only kernel when
``CPU_ACCEL_IOC_GET_ABI_INFO`` returns ``ENOTTY`` and falls back to ABI 23.
An explicitly requested ABI outside the supported range fails with ``ENOTSUP``.

Extension policy
================

Existing command numbers, argument types, and encoded sizes are immutable.
Reserved words and unknown flag bits must be zero and are rejected with
``EINVAL``. Unknown commands return ``ENOTTY``. New fields require a new
command and a new argument object; existing objects must not grow by appending
fields.

Core ABI 23 commands ``0x00`` through ``0x0c`` and the control mapping layout
are compatibility-critical. ABI 24 adds commands ``0x0d`` through ``0x10``:
ABI negotiation, architecture telemetry, and x86 image admission. The mapping
layout and fingerprint are unchanged from ABI 23.

Architecture telemetry is a separate extension object. The x86 image-admission
object contains numeric virtual addresses and is architecture-specific. It is
not portable and is not part of the frozen control-plane contract. A production
portable workload interface must use opaque registered objects before it can be
frozen.

State and result contract
=========================

The shared control mapping is authoritative for state, terminal result fields,
and workload telemetry. ``poll()`` is a control-plane optimization only. It
reports ``EPOLLIN | EPOLLRDNORM`` after a terminal state is published and never
replaces shared-memory state. Applications may instead poll the mapping
directly.

Terminal states are ``CPU_ACCEL_STATE_COMPLETE``, ``STOPPED``, ``ERROR``,
``WATCHDOG``, and ``ESCAPED``. A controller may retry after ``ERROR``,
``WATCHDOG``, or ``ESCAPED`` only after returning to Linux mode and issuing a
lifecycle ``EXIT`` followed by ``RESET``. Manager death performs the lifecycle
exit during file release, so a replacement manager may reset directly after
observing a terminal state. A controller must not treat a nonterminal state as
safe.

Compatibility matrix
====================

.. list-table::
   :header-rows: 1
   :widths: 30 40 30

   * - Combination
     - Required behavior
     - Evidence
   * - ABI 23 client on ABI 24 kernel
     - Uses only ABI 23 commands; observes selected ABI 23 and unchanged
       mapping layout, structure size, and fingerprint.
     - ``tools/cpu_accel/test-uapi-abi23-compat.c``
   * - ABI 24 SDK on ABI 23-only kernel
     - Sees ``ENOTTY`` from ABI info, falls back to ABI 23, and maps
       successfully.
     - ``tools/cpu_accel/test-uapi-sdk-fallback.c`` with the test ioctl shim
   * - ABI 23 forced SDK on ABI 24 kernel
     - Selects ABI 23 and completes the workload.
     - Phase 9 freeze gate
   * - ABI 24 SDK on ABI 24 kernel
     - Selects ABI 24 and completes the workload with no latency regression.
     - Phase 9 freeze gate

The direct ABI 23 client was runtime-tested against the ABI 24 kernel. The ABI
24 SDK fallback path was validated with a test-only ``LD_PRELOAD`` ioctl shim
that makes ``CPU_ACCEL_IOC_GET_ABI_INFO`` return ``ENOTTY``, the observable
behavior of an ABI 23-only kernel. No physical ABI 23-only kernel boot was
performed.

Error policy
============

Invalid flags, reserved words, unsupported workloads, or malformed
configuration return ``EINVAL``. Unsupported ABI requests return ``ENOTSUP``.
Unknown ioctls return ``ENOTTY``. Missing privilege returns ``EPERM``. Busy
device, session, or lifecycle states return ``EBUSY``. User-memory copy
failures return ``EFAULT``. Resource exhaustion returns ``ENOMEM`` or
``ENOENT`` as appropriate. Backend failures are reported through terminal
state and recovery fields.
