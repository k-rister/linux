.. SPDX-License-Identifier: GPL-2.0

========================================
CPU accelerator memory model (design)
========================================

Status and scope
================

This document defines the memory model for the CPU accelerator prototype. It
is a design contract, not an ABI specification or an implementation claim.
The current prototype only has kernel-owned workload buffers and a shared
control/telemetry mapping. In particular, the current ``memmove`` workload
does not execute user code and does not provide a protected accelerator
address space.

The kernel prototype now tracks an internal owner and generation epoch for
the workload region. Private kernel-owned buffers return to Linux ownership
after lifecycle exit. Shared entries remain in ``COMPLETE`` or ``ERROR``
ownership until the companion explicitly reclaims them. This is lifecycle
bookkeeping, not yet protection against an untrusted accelerator or a device
DMA engine.

The model is intended to support a non-networked protected workload first and
then a NIC-backed dataplane. It must preserve the single-OS model: Linux,
the companion process, and one or more accelerator CPUs remain in the same
kernel. Entering accelerator mode is a controlled ownership transition, not
a second kernel or a guest environment.

Memory classes
==============

Every mapping visible to an accelerator is assigned one explicit class:

``CONTROL``
    Linux-owned control and telemetry pages. The companion may map these
    pages, but accelerator code does not use them as general data memory.
    State transitions are published with release/acquire operations.

``ACCEL_PRIVATE``
    Pages mapped only in the accelerator address space. Linux allocates and
    initializes them before entry and does not reclaim, fault in, migrate, or
    change their permissions while the accelerator is active. These pages
    hold code, stacks, private state, and scratch data.

``SHARED``
    Pages mapped into both the companion address space and the accelerator
    address space. They are the only intended bidirectional communication
    path during accelerator execution. Each shared region has an ownership
    protocol; a writable mapping alone is not an ownership protocol.

``DMA``
    A shared or private region additionally mapped into a device IOMMU
    domain. The device is allowed to DMA only to explicitly registered pages.
    The IOMMU mapping is established before accelerator entry and is not
    changed during the dataplane interval.

The control mapping and data mappings must use different handles and offsets.
The existing fixed shared mapping should not grow into an implicit general
purpose data channel: doing so would make it difficult to audit which pages
the accelerator and a device may modify.

Protected address-space contract
=================================

The first protected workload should execute at ring 3 in a dedicated
accelerator ``mm_struct``. The address space contains only:

* a prevalidated accelerator image and read-only code;
* preallocated, prefaulted private data and stack pages;
* explicitly registered shared windows; and
* optional device-DMA pages with matching IOMMU permissions.

The accelerator must not run in the companion process's ``mm`` and must not
be given arbitrary user virtual addresses. The control plane passes opaque
region identifiers and bounded offsets; the kernel resolves those identifiers
to pinned pages and the accelerator address-space layout.

Before entry, the admission path must:

* pin or otherwise make every required page resident;
* populate all required page-table levels and remove lazy allocation,
  copy-on-write, migration, and reclaim from the active set;
* validate code, stack, private, shared, and DMA permissions;
* install guard pages where the architecture supports them; and
* freeze address-space mutation for the active epoch.

No page fault, allocation, filesystem access, system call, signal delivery,
or dynamic mapping operation is part of the accelerator contract. A fault is
an admission or execution failure, not a supported slow path. The companion
may prepare the next buffer or process completed work, but it must not alter
the active accelerator address space or its page contents outside the shared
region ownership protocol.

The active ``mm`` must not be modified while it is running. This is the
memory-side rule that prevents Linux from needing to invalidate translations
on the accelerator CPU for ordinary unmap, mprotect, COW, migration, or
reclaim activity. Enter and exit may pay the architecture-specific address
space transition and TLB costs; the dataplane interval may not.

Shared-region ownership
=======================

Shared memory is divided into independently owned regions or ring elements.
The initial SDK should expose a small state machine:

``LINUX``
    Linux or the companion owns the contents and may prepare them.

``READY``
    The producer has finished initialization and publishes the element with
    a release store. The consumer may claim it with an acquire load.

``ACCELERATOR``
    Accelerator code owns the element. The companion must treat it as
    read-only from its point of view until ownership is returned.

``COMPLETE``
    Accelerator code has finished and publishes the result with a release
    store. Linux or the companion may reclaim it with an acquire load.

``ERROR``
    The element requires control-plane recovery and must not be reused until
    the kernel has completed the recovery epoch.

The hot path uses cache-line-aligned atomic sequence numbers and
release/acquire ordering. It must not use a syscall, mutex, page fault,
allocation, or scheduler wakeup. Polling is the baseline communication
mechanism; optional notifications are control-plane hints and are never
required for correctness. A region may have one producer and one consumer
initially. Multi-producer or multi-consumer ownership should be added only
after the ordering and cache-coherency costs are measured.

The current ABI 8 proof uses a fixed 64 KiB shared-data mapping containing two
8 KiB entries. The kernel validates the selected entry and length, prefaults
the backing allocation at module initialization, and exposes only bounded
entry data through the mapping. ``CPU_ACCEL_IOC_SHARED_READY`` transfers a
Linux-prepared entry to the accelerator with its current epoch;
``CPU_ACCEL_IOC_SHARED_RECLAIM`` returns a terminal entry to Linux. The
mapping is intentionally small and fixed so it can be audited and is page
size compatible with common 4 KiB, 16 KiB, and 64 KiB Linux targets. It does
not yet establish a protected ring-3 mapping or IOMMU domain.

The kernel must reject a start if a shared region is still owned by Linux or
has an incomplete handoff. On orderly completion, stop, or watchdog, the
kernel retains ``COMPLETE`` ownership until reclaim. On execution error, it
retains ``ERROR`` ownership until recovery. This prevents a late accelerator
store from corrupting a buffer that Linux has already recycled.

Proposed control-plane objects
==============================

The eventual ABI should use opaque handles rather than user pointers:

``address-space``
    Owns the accelerator ``mm`` and its immutable mapping table.

``region``
    Describes class, length, permissions, alignment, and ownership epoch.

``image``
    Describes prevalidated code and entry metadata. Loading or replacing an
    image is allowed only while the accelerator is in Linux mode.

``binding``
    Binds an address space, image, region set, and accelerator CPU or CPU
    group for one run.

The first implementation should keep these objects private to one open file
and one accelerator instance. Exporting globally visible handles before
multi-tenant isolation exists would create an avoidable lifetime and access
control problem. The ABI should report region ownership, active epoch,
mapped length, and DMA/IOMMU status in the control mapping, but should not
expose kernel virtual or physical addresses.

IOMMU and networking extension
===============================

For the networking prototype, the NIC and accelerator must share an IOMMU
domain whose mappings are limited to registered ``DMA`` regions. Descriptor
rings, packet buffers, and completion metadata need separate permissions and
ownership rules. A packet buffer returned to Linux or the companion must be
removed from the accelerator/device-owned set before it is reused. The
control-plane transition may be slow; the packet path must not perform IOMMU
map/unmap operations.

This design does not require the CPU accelerator to become a PCI device. A
custom driver is still useful as the authority that binds the CPU lifecycle,
address space, region ownership, and IOMMU device ownership into one
recoverable transaction. DPDK can remain the userspace dataplane library if
the eventual region and DMA handles can be integrated without copying.

Recovery and hard-bound implications
=====================================

Memory protection does not by itself make arbitrary accelerator code
recoverable. A noncooperating ring-3 loop with interrupts disabled can still
prevent a normal control thread from regaining the CPU. The prototype must
therefore separate:

* cooperative stop, which is suitable for the initial oslat-like image;
* an architecture-specific fault/escape path for a protected ring-3 image;
  and
* machine-level failures such as NMI, SMI, machine check, or firmware
  activity, which remain outside a formal Linux-only bound.

The current x86 prototype exercises the second case with
``CPU_ACCEL_IOC_USER_ESCAPE``.  The controller requests a local-APIC NMI, and
the NMI handler redirects only a user-mode frame belonging to the active
prevalidated image to a pinned escape trampoline.  The trampoline performs the
terminal ``CPU_ACCEL_IOC_USER_EXIT`` operation.  This mechanism is deliberately
limited: it does not recover kernel-mode execution, an NMI/SMI/machine-check
handler, a failed or disabled local APIC, a host/hypervisor fault, or a
corrupted/self-modifying image.  It is therefore a recovery experiment and
not evidence of a formal packet-latency bound.

ABI 12 adds a deliberately noncooperative ``user-hang`` fixture that never
polls the control mapping.  Its only supported termination path is the x86
escape operation, and successful recovery is reported as
``CPU_ACCEL_STATE_ESCAPED``.  This makes the recovery test distinguishable
from a cooperative ``COMPLETE`` result without treating the fixture as a
general-purpose user workload.

The first protected prototype should make a bounded, cooperative image and
its fault behavior measurable before attempting arbitrary user code. A hard
packet round-trip bound can be claimed only after the active-mm freeze,
translation invalidation policy, interrupt/IPI policy, device-IOMMU policy,
and recovery behavior are specified for the target architecture.

Implementation sequence
=======================

The implementation checkpoints are:

1. [completed] Add an internal region/epoch model without exposing physical
   addresses or allowing user code to run. Exercise it with the existing
   kernel-owned memmove workload.
2. [completed] Add a prefaulted ``SHARED`` region and a companion SDK ring
   with explicit ownership transitions. Verify that concurrent misuse is
   rejected and that the accelerator never accesses the region outside its
   active epoch.
3. [completed] Add the x86 cooperative, prevalidated ring-3 oslat-like
   image in a forked task's dedicated address space. Measure entry/exit and
   active-interval TLB behavior; this first slice now pins the image and stack
   and holds the active ``mmap_lock`` write side.
4. [completed for x86 prototype] Add architecture-specific escape/recovery
   handling and document which failures remain unrecoverable without reboot.
   The ABI 12 NMI escape path is a bounded recovery experiment for the
   prevalidated ring-3 image; it is not a hard guarantee.
5. [completed] Define the architecture-neutral recovery capability and result
   contract.  ABI 12 distinguishes successful escape from unsupported,
   timed-out, or failed attempts and exercises retry behavior with a debug-only
   dropped-NMI fixture.
6. [completed for x86 prototype] Add a dedicated APIC entry vector for kernel
   workloads.  ABI 13 removes the generic scheduler/function-call IPI handoff,
   but it does not yet suppress other IPIs or TLB shootdowns.
7. Define APIC ownership and address-space/TLB policy, then add IOMMU-backed
   ``DMA`` regions and userspace/NIC integration only after the non-networked
   memory contract is stable.
