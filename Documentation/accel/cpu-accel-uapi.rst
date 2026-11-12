.. SPDX-License-Identifier: GPL-2.0

========================
CPU accelerator UAPI
========================

This document is the normative contract for the CPU accelerator control UAPI.
The current ABI is 25 and the minimum supported ABI is 23. ABI 23 and ABI 24
are frozen compatibility surfaces. The x86 ring-3 workload, image-admission,
and ABI 25 static-domain extensions remain experimental and are not part of
the frozen control-plane contract.

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

ABI 25 adds commands ``0x11`` through ``0x16`` for domain configuration,
domain start, user-worker join, worker-targeted user escape, domain status,
and bounded SPSC queue configuration. It also defines the per-worker telemetry
mapping tokens returned by domain configuration. ABI 25 does not change
``struct cpu_accel_config``, ``struct cpu_accel_shared``, or any ABI 23/24
command encoding.

An ABI 25 domain contains a fixed set of up to ``CPU_ACCEL_MAX_WORKERS``
CPUs. Admission rejects repeated CPUs, CPU 0, an offline CPU, multiple NUMA
nodes, incomplete SMT sibling sets, and a set that leaves no online Linux
housekeeping CPU. The CPU set is immutable while configured. Normal stop,
worker failure, watchdog expiry, manager death, and activation rollback are
domain-wide. No partially active or mixed Linux/accelerator domain is a
success state.

For a user workload, the manager configures common domain parameters and
creates one attached workload region per worker. Each worker process receives
the descriptor, is pinned to its selected CPU, maps its per-worker telemetry
token and region, and calls ``CPU_ACCEL_IOC_DOMAIN_WORKER_JOIN`` from its own
sealed memory map. The manager never remotely enters another task's address
space. The legacy ``START`` and ``CONFIG`` commands are rejected after ABI 25
is selected; ABI 25 callers use the new domain commands. The ABI 25 escape
command targets one joined worker by index. The ABI 23 ``USER_ESCAPE``
command continues to target worker 0 and preserves its original encoding.

``CPU_ACCEL_IOC_DOMAIN_QUEUE_CONFIG`` is accepted only after ABI 25 domain
configuration and before domain start, and only when the selected workload is
``CPU_ACCEL_WORKLOAD_USER_PIPELINE``. It configures a static linear pipeline:
worker ``i`` produces to worker ``i + 1``. The command requires exactly
``cpu_count - 1`` unique queue regions, supports 2 to 16 workers, and uses one
uniform power-of-two capacity from 1 through 1024 slots for all queues.

ABI 25 region creation uses ``region_type`` in the field that was previously
reserved in the experimental object. ``CPU_ACCEL_REGION_TYPE_WORKLOAD`` is zero
and is used by ordinary workload regions. ``CPU_ACCEL_REGION_TYPE_SPSC_QUEUE``
selects a queue region. A queue region must be attached and bidirectionally
mapped, have exactly one manager mapping before configuration, and contain the
queue magic, ABI version, capacity, topology, size, epoch, and mapping token
expected by the driver.

An SPSC queue is a 256-byte fixed header followed by 16-byte slot records.
The header contains an immutable descriptor, one producer-owned 64-byte cache
line, one consumer-owned 64-byte cache line, and a 64-byte alignment pad. Slots
begin at offset 256. The queue is full when
``producer.head - consumer.tail == capacity`` and empty when the two indices
are equal. Producers and consumers spin without overwriting or consuming an
invalid slot.

The producer writes a slot and then release-stores ``producer.head``. The
consumer acquire-loads ``producer.head``, reads the slot, and release-stores
``consumer.tail``. Before reusing a slot, the producer acquire-loads
``consumer.tail``. The queue header's owner-ready word contains producer-ready,
consumer-ready, and producer-done bits. The producer release-sets the done bit
after its final enqueue; a consumer acquire-loads it while the queue is empty
so it drains all published records and then exits. Queue coordination is
shared-memory polling and does not use routine accelerator IPIs. Each queue
region has exactly two owner mappings: its producer and consumer. Worker VMA
admission accepts only the worker's workload region and its expected input and
output queue regions.

Architecture telemetry is a separate extension object. The x86 image-admission
object contains numeric virtual addresses and is architecture-specific. It is
not portable and is not part of the frozen control-plane contract. A production
portable workload interface must use opaque registered objects before it can be
frozen.

State and result contract
=========================

For ABI 23 and ABI 24, the shared control mapping is authoritative for state,
terminal result fields, and workload telemetry. For ABI 25, the legacy shared
mapping is a domain summary and each worker has a separate read-only telemetry
mapping. ``CPU_ACCEL_IOC_DOMAIN_STATUS`` reports joined, active, terminal, and
per-worker state and result counts. ``poll()`` is a control-plane optimization
only. It reports ``EPOLLIN | EPOLLRDNORM`` after the domain summary reaches a
terminal state and never replaces shared-memory state. Applications may
instead poll the mapping directly.

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
   * - ABI 23 client on ABI 25 kernel
     - Uses only ABI 23 commands; observes selected ABI 23 and unchanged
       mapping layout, structure size, and fingerprint.
     - ``tools/cpu_accel/test-uapi-abi23-compat.c``
   * - ABI 24 SDK on ABI 23-only kernel
     - Sees ``ENOTTY`` from ABI info, falls back to ABI 23, and maps
       successfully.
     - ``tools/cpu_accel/test-uapi-sdk-fallback.c`` with the test ioctl shim
   * - ABI 23 forced SDK on ABI 25 kernel
     - Selects ABI 23 and completes the workload.
     - Phase 9 freeze gate
   * - ABI 24 SDK on ABI 25 kernel
     - Selects ABI 24 and completes the workload with no latency regression.
     - Phase 9 freeze gate
   * - ABI 25 SDK on ABI 25 kernel
     - Selects ABI 25, configures a multi-CPU static domain, and completes
       one independent user-oslat worker per selected CPU.
     - Phase 10 SMP gate

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
