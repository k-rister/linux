.. SPDX-License-Identifier: GPL-2.0

========================================
CPU accelerator memory model (design)
========================================

Status and scope
================

This document defines the memory model for the CPU accelerator prototype. It
is a design contract, not an ABI specification or an implementation claim.
The kernel-mode timestamp and ``memmove`` workloads use kernel-owned buffers
and the shared control/telemetry mapping; the current ``memmove`` workload does
not execute user code. The x86 ring-3 path is a separate sealed-process-``mm``
prototype described below, not the final protected address-space model.

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

The active ``mm`` must not undergo VMA or permission changes while it is
running. The write lock enforces that rule for the current prototype. Reclaim
can still remove a PTE despite that lock; page pins prevent backing reuse, and
the batched-unmap generation policy below keeps the owner CPU from using an
affected address space after the backing can be released. Other PTE-changing
paths must be excluded unless they provide an equivalent lifetime guarantee.
Enter and exit may pay the architecture-specific address-space transition and
TLB costs; the dataplane interval may not.

Address-space and TLB ownership policy
=======================================

The current x86 ring-3 workload is a sealed-process-``mm`` prototype, not yet
the final address-space ownership model. On device open, the driver retains
the opener's ``mm`` as the companion address space. User-workload admission
rejects that same ``mm`` during configuration and start, and requires a
single-threaded worker with a distinct ``mm``. The companion opens and maps the
control interface, then forks a worker that execs a fresh worker image, passing
the device file descriptor and preserving standard streams. Other inherited
file descriptors are closed. The new process maps its own control pages, so it
does not retain the companion's copy-on-write mappings.

Before START, the worker switches to its admitted stack and unmaps every
removable VMA except the executable image, private stack, control mapping,
shared-data mapping, and the page containing the worker's registered RSEQ
area, when present. The test-only ``user-reclaim`` workload additionally
retains exactly one read-only, shared, regular-file mapping. That page is
deliberately left unpinned so the focused reclaim test can ask reclaim to
remove it. The validator recognizes this read-only mapping with
``VM_MAYSHARE``: Linux does not set ``VM_SHARED`` for a shared mapping made
from a read-only file descriptor. The focused ``test-tlb-generation`` test
can run this case alone with ``--reclaim-only``. It requests ``MADV_PAGEOUT``
while the ring-3 owner is active, schedules owner exit after one second, and
retries pageout after the owner releases its ``mm`` if the page remains
resident. It verifies non-residency and the file contents after refault. The
kernel may update the RSEQ area on the user-return
slowpath after a deferred reschedule, so removing it would turn normal
accelerator exit into a SIGSEGV. The driver validates the RSEQ page as private,
readable, writable, and non-executable. The legacy x86 ``[vsyscall]`` VMA is
fixed at ``VSYSCALL_ADDR`` and rejects ``munmap``; the worker leaves it mapped,
and the driver accepts only that exact executable-only architecture mapping. The
image must be file-backed private RX, the stack anonymous private RW without
execute permission, and the device maps RW shared at their exact offsets and
sizes (including a single VMA if the adjacent maps are coalesced). The driver
validates the complete remaining VMA set, pins the image and stack, and holds
the worker ``mm``'s ``mmap_lock`` for write during the active epoch. The
user fixtures, seal/start trampoline, exit syscall helper, and escape
trampoline are emitted into one page-aligned executable section, and the CLI
passes that section's page-rounded bounds as the admitted image. This avoids
deriving the image from the distance between functions whose placement can
change with linker layout.

This path seals the worker's process ``mm`` at VMA level; it does not create an
``mm`` independently of a task. The architecture-owned ``[vsyscall]`` mapping
is not a workload entry point and is not pinned as part of the accelerator
image.

ABI 15 defers and replays call-function IPIs while the native x86 direct
backend owns a CPU. Native x86 remote TLB flushes use call-function work, so
their callbacks remain queued until the ownership interval ends. A synchronous
flush sender can therefore wait for the accelerator to exit. This is an
incidental consequence of call-function deferral. For the ring-3 process-mm
backend, ``flush_tlb_multi()`` removes active ring-3 owners from an
mm-scoped flush mask only when they are running a different ``mm``. That
address space's TLB generation is advanced, and ``switch_mm()`` performs the
needed local flush before the owner CPU can use it again. A range flush for
the owner's own ``mm`` remains synchronous: the owner is asked to stop before
the flush dispatches, and the caller waits for local TLB reconciliation
before it can release any gathered pages. ``mmu_gather`` does not yet transfer
its queued data and page-table pages to a completion owner; adding that
completion ownership is required before same-``mm`` owners can be filtered.
The owned worker ``mm`` is write-locked for the active interval, which
excludes ordinary VMA changes; reclaim has its own completion path for
eligible clean folios. This avoids making a synchronous flush wait for an
interrupt-disabled ring-3 owner that is running a different address space,
without treating an unacknowledged same-``mm`` generation as permission to
reuse pages.
ABI 16 reports
``arch_tlb_shootdown_targets`` for x86 flush batches that target the CPU while
accelerator ownership is active, including the native IPI and INVLPGB paths.
The ring-3 process-mm window uses the same per-CPU ownership state as the
direct backend. Native x86 flush hooks track the highest TLB generation that
targets the owned ``mm``, separately from a pending address-space-unscoped
flush such as a kernel/global flush. The owner retires ownership and snapshots
that state while the image mm remains write-locked, then flushes the local TLB
if either kind of invalidation is pending. Only then does the driver unlock the
mm and release the pinned image pages. Remote callback completion remains
governed by the native flush path; there is no separate remote acknowledgment
queue. A kernel-address-range flush on x86 uses INVLPGB and its system-wide
completion barrier when available and no accelerator CPU is owned. During
ownership it uses the synchronous kernel-only IPI path and waits for the owner
to exit; this preserves user translations while keeping stale kernel
translations from outliving the flush. Before dispatching those IPIs, the
``kernel_tlb_flush_range()`` and ``kernel_tlb_flush_all()`` IPI fallbacks
request each registered owner's nonblocking stop callback while the target
CPUs remain reserved against new admission.
Owner exit waits for any callback already in flight before dropping its
registration.
Standalone full and all-nonglobal flushes retain the synchronous IPI path
during ownership because they would evict active accelerator user translations.
The task-local ``arch_tlbbatch_flush()`` path has a narrower exception: it may
defer a ring-3 owner only when every affected ``mm`` is different from the
owner's sealed ``mm``. Its per-``mm``
generations are reconciled by ``switch_mm()`` before that owner CPU can use an
affected address space. An own-``mm`` unmap and a kernel-mode owner retain the
synchronous path. New ownership is barred on the selected target CPUs until
each flush completes, so it cannot race a new owner after choosing its target
mask.
Global-ASID INVLPGB broadcasts reserve all online CPUs through
``TLBSYNC`` for the same reason. An attempted entry on a reserved target CPU
returns ``-EBUSY``. The worker still uses its process ``mm``. Kernel mapping
changes affecting code or the exception/recovery path still need a separate
maintenance policy; TLB invalidation alone does not establish safety.
Do not infer TLB isolation or a latency bound from a zero target count or TLB
counter delta.

The policy for a protected accelerator address space is:

* The accelerator runs only in its sealed ``mm``. Before entry, the CPU must
  leave any Linux task ``mm`` and be removed from that ``mm``'s active CPU set
  using the architecture's normal address-space-switch rules.
* The active accelerator ``mm`` is prefaulted and held against VMA and
  permission changes. The current prototype pins its admitted pages. Reclaim
  may still remove a PTE, but it must not release or migrate pinned backing;
  an own-``mm`` batched unmap stays in the synchronous TLB target set until
  owner exit. Other PTE-changing paths remain unsupported until they provide
  an equivalent lifetime guarantee.
* For the owned ``mm``, record a targeted invalidation and flush locally before
  releasing its pages or unlocking the ``mm``. For another ``mm``, omit the
  owner from the immediate target mask only while it cannot use that address
  space; its advanced TLB generation must force a local flush before
  ``switch_mm()`` lets the CPU use it again. Deferral must never permit stale
  translations to be used after a page is freed or reused.
* Entry and exit perform the required address-space switch and local
  invalidations. Kernel-global mapping changes need a separate policy because
  they are not limited to the accelerator ``mm``; they must either quiesce the
  owner before acknowledgement or be proven irrelevant to all code executed
  during the active interval and its recovery path.

Flush completion and kernel maintenance
---------------------------------------

The current x86 implementation keeps ``arch_tlbbatch_flush()`` synchronous.
This hook belongs to the task-local batched-unmap path: generic
``try_to_unmap_flush()`` passes it ``current->tlb_ubc.arch`` and clears the
batch only after the hook returns. Reclaim and migration callers rely on that
completion before proceeding. In particular, a dirty unmapped folio must be
flushed before I/O starts, or a stale writable translation could modify it
during writeback.

The x86 batch contains a union of target CPUs, not the ``mm`` list or folios
that produced it. Each ``arch_tlbbatch_add_pending()`` nevertheless advances
the affected ``mm``'s TLB generation. The accelerator records when an unmap
targets the active ring-3 owner's own ``mm``. At batch flush, x86 may omit a
ring-3 owner only when its current ``mm`` has no pending generation and no
unscoped invalidation. That owner cannot use translations belonging to another
``mm`` while it remains in the sealed address space; ``switch_mm()`` must
reconcile the advanced generation before that ``mm`` can run again.

Generation bookkeeping alone is not an admission interlock: the unmap records
the generation after clearing the PTE, while owner entry initializes its
pending generation as it becomes active. Batched reverse-map unmaps now
bracket PTE clearing and generation publication with
``arch_tlbbatch_unmap_begin()`` and ``arch_tlbbatch_unmap_end()``. On x86, the
per-``mm`` in-flight count is serialized with owner admission. A new owner
waits for that update to finish and locally flushes its TLB before it can use
the address space. An owner already active when the update starts is recorded
by the generation bookkeeping. Without a reclaim completion, it remains in the
synchronous target set; the vmscan completion path may omit it only after
registering an acknowledgement for the affected generation.

This closes the admission race for batched reverse-map unmaps. The generic
``arch_tlbbatch_flush()`` path and migration callers remain synchronous. Vmscan
has a separate bounded path for eligible clean folios: it reserves completion
storage before PTE removal and retains each folio until matching owners
acknowledge. This does not change the generic batch contract, and
``mmu_gather`` remains a separate path.

Without a reclaim completion, an owner whose own ``mm`` was changed remains
in the synchronous target set. The write lock excludes VMA changes, and pins
keep admitted folios from being released while the owner uses them. Vmscan
checks for known DMA pins before demotion, swap allocation, or unmapping,
avoiding work on folios that must stay resident. Its post-unmap pin check
remains necessary for a pin acquired during the unmap race. For eligible
reclaim, x86 filters an own-``mm`` owner only after registering it against the
reserved completion; the folio stays held until that owner's local TLB flush
is acknowledged. Missing completion storage and all other callers use the
synchronous path. Kernel-mode owners have no sealed user ``mm`` and are not
filtered, so their synchronous flush can still wait without a bound.

The owner can exit between the PTE clear and generation publication. The
synchronous path remains safe in that ordering: an owner still active when the
generation is recorded reconciles it on exit; an owner that has already exited
is no longer filtered from the ordinary batch flush. The completion path
preserves this handoff: it registers only owners still active at the recorded
generation, and a registered owner acknowledges only after its local TLB
reconciliation. If the task switches away before a synchronous flush, the
advanced ``mm`` generation is checked before the task can use the address
space again. Inactive state alone is not an acknowledgement that stale
translations have been invalidated.

The reclaim call sites impose different completion obligations. In
``try_to_unmap_one()``, the PTE is cleared and rmap state is updated before the
task-local batch is flushed. In ``shrink_folio_list()``, a dirty folio must
complete ``try_to_unmap_flush_dirty()`` before ``pageout()`` starts writeback;
reclaim also flushes before ``free_unref_folios()`` releases reclaimed folios.
Migration flushes its task-local batch before it copies or moves folios. The
vmscan completion path below transfers an eligible folio to a completion-managed
list only after reclaim has selected it for release. The dirty-reclaim and
migration ordering also protects data operations: a stale owner must not write
the folio while writeback reads it or while migration copies it. Deferring
those flushes therefore requires transferring the writeback or migration
operation itself to work that starts after owner acknowledgement, while
retaining the folio locks, references, and caller accounting. Holding the folio
while the original caller proceeds is not sufficient. Other callers continue
to wait for the synchronous flush; returning with only a pending generation
would let them proceed as if invalidation had completed.

This path is separate from ``mmu_gather``. The x86 architecture's
``arch_tlbflush_unmap_batch`` stores only a CPU mask and an
``unmapped_pages`` flag; it does not own the folios whose mappings were
removed. Returning after only recording a pending flush would let the generic
caller proceed to I/O or release a folio before the owner handles it. A
nonblocking ``mmu_gather`` version therefore needs its own completion-managed
page-table and data-page lists. Changing the architecture hook alone cannot
defer those lifetimes.

The first vmscan reclaim slice now reserves a completion object from a
preallocated pool before clearing any PTE. Each object can record up to 16
distinct ``mm`` generations and 16 owner acknowledgements; the pool holds 16
objects. It is used only for clean, non-hugetlb folios without buffer-release
work. Rmap records each affected ``mm`` generation in the reserved object.
When that folio is otherwise ready to be freed, x86 registers each matching
active ring-3 owner against its pending generation, then flushes ordinary CPU
targets synchronously. The flush filter omits only owners registered to that
specific completion. After registration succeeds, x86 also requests those
owner instances to stop. The request is generation-checked and only prompts
exit; it is not an acknowledgment. Owner exit acknowledges those entries
after its local TLB reconciliation; a worker then uncharges and frees the
folio. If the owner exits before the request is delivered, its registered
completion is still drained by that exit path. If the CPU has since admitted a
new owner, the generation check leaves that owner alone.

Dirty folios, writable PTE batches, failed unmaps, DMA-pinned folios, folios
with buffer-release work, migration, huge-page collapse, and other callers
without a reclaim disposition keep the synchronous path. Exhausting the pool
or either fixed-size record array also falls back to synchronous flushing.
Completion storage is reserved before PTE removal. A stalled owner keeps its
folio and completion slot; after all 16 slots are occupied, further reclaim
can wait on the existing synchronous path. This is bounded backpressure, not a
timeout. If the acknowledgement array fills after some owners were registered,
the completion-specific stop request is not issued, and the synchronous
fallback flushes the partial set through the ordinary path. That path may issue
its own stop requests; the partial completion references drain after local
reconciliation before the cancelled slot returns to the pool.

The generic ``arch_tlbbatch_flush()`` remains synchronous. Only the dedicated
vmscan completion path can defer a matching x86 ring-3 owner, and it retains
the folio until that owner acknowledges the generation. The per-CPU completion
list is drained by the orderly ``x86_cpu_accel_user_exit()`` path after its
local flush. A CPU-offline or recovery path that bypasses that exit must keep
the completion outstanding until it has independently established equivalent
quiescence; inactive state alone is not an acknowledgement.

Terminal x86 kexec has a separate quiescence contract. The native kexec
shutdown path drains accelerator owners before the confidential-memory
callbacks and keeps owner admission closed through the handoff. This is needed
because ``reboot_force`` can skip ``stop_other_cpus()``; the owner gate does
not quiesce unrelated Linux activity on that forced path. With normal CPU-stop
policy, ``enc_kexec_finish()`` runs only after ``stop_other_cpus()`` has
stopped the other CPUs. Crash shutdown sends a one-shot NMI whose responding
CPUs enter the non-returning stop path before the encryption callback. This is
quiescence for transfer to another kernel, not an owner-exit acknowledgement
that a returning MM caller can reuse. The crash NMI shootdown waits only one
second, so a CPU that does not respond is not proven quiescent by that
timeout.

``mmu_gather`` is a distinct range-flush path. On x86 it calls
``flush_tlb_mm_range()``; after the flush/generation rules permit reclamation,
``tlb_flush_mmu_free()`` releases its queued data pages and page-table batches.
An asynchronous redesign of that path would need to transfer those
``mmu_gather`` lists to its own completion-managed object. Those lists are not
part of ``arch_tlbbatch_flush()`` or its architecture batch. The transfer must
also cover intermediate drains: exhausting a data-page batch can make the
caller run ``tlb_flush_mmu()``, and a full page-table batch can be flushed and
freed before ``tlb_finish_mmu()``. Page-table unsharing has an immediate TLB
flush and a GUP-fast synchronization before a table can be reused; delayed rmap
removals are also performed after a TLB-only flush. Deferring only the final
lists in ``tlb_finish_mmu()`` would miss these earlier release points. A
completion design must preserve those immediate barriers and transfer each
deferred data-page and table batch, including its software-walker RCU lifetime,
to the completion owner.

This is address-space deferral, not a general asynchronous reclaim interface.
Any future path that lets an active owner continue using the affected ``mm``
must retain every data or page-table page until each CPU that could still use
or speculatively walk the old translation has invalidated it or been quiesced.
The vmscan path above does this for one clean folio at a time. Extending it to
other paths must also:

* carry enough address-space, generation, and target information to associate
  each acknowledgement with the pages it protects; and
* preserve outstanding acknowledgements across owner exit, CPU offlining, and
  recovery, with bounded storage or explicit backpressure.

A per-CPU pending bit alone cannot meet this contract. The driver's current
NMI escape is also unsuitable as a generic MM mechanism: it is a controller
request that terminates this prototype's run, not an acknowledgement that
arbitrary MM callers can request and await. Keep generic callers on the
synchronous ``arch_tlbbatch_flush()`` path unless they transfer their own page
disposition into a completion-managed object.

There is no safe timeout-only variant of these flush hooks. The MM hooks return
no error, and their callers proceed on the assumption that the translation
lifetime is complete. A timeout followed by return would leave the caller free
to start writeback or release a page that the owner can still access. A
completion-managed design must take responsibility for that page before the
unmap becomes visible, associate it with the affected ``mm`` generation and
owner CPUs, and release it only after invalidation or quiescence is
acknowledged. If completion storage cannot be reserved, the operation must
retain the synchronous path. Reclaim and ``mmu_gather`` need separate
completion owners because they carry different page lists.

The current reclaim slice keeps migration, huge-page collapse, and every
caller without a deferred folio disposition synchronous. For vmscan, it
retains each eligible folio and prevents reuse or release until its owners
acknowledge the generation. The preallocated object and its bounded record
capacity are secured before clearing the first PTE; when capacity is exceeded,
the operation completes synchronously before reclaim proceeds. ``mmu_gather``
remains separate and needs its own completion object for both data-page and
page-table batches.

The generic owner/MM contract must keep four events distinct: closing
admission, requesting a stop, ending execution, and acknowledging the required
TLB invalidation. A stop request is nonblocking and runs outside MM, page-table,
and owner locks. It may ask an owner to leave, but only the matching owner
instance may acknowledge, and only after execution has ended and its local TLB
has reconciled the requested ``mm`` generation plus any unscoped invalidation.
The registry must identify the address space actually loaded by an owner,
independently of the task that submitted the work; ``mm_cpumask`` alone is not
that registry. Admission and invalidation publication must share an
interlock, so an owner either joins the affected completion or flushes the
current generation before it can execute.

The first MM-core step now provides a per-``mm`` execution-owner registry and
an update-depth interlock. The x86 backend registers the ``mm`` it actually
loads, publishes TLB generations to those records, and snapshots matching
owners for reclaim. The snapshot is only a candidate list: MM now attaches a
subscription to the exact owner generation under the registry lock.
``mm_cpumask`` remains the ordinary CPU-flush target set; it is not used as
the ownership record. The x86 clean-folio prototype uses this generic
subscription and owner-stop callback contract.

``mmu_owner_snapshot()`` now invokes each registered owner's ``get`` operation
while holding the registry lock and returns a reference to its owner-data
context; callers release it with ``mmu_owner_snapshot_put()``. The x86 backend
uses permanent per-CPU request storage, so its get/put operations are no-ops.
``mmu_owner_subscribe()`` validates the snapshot generation, pins both owner
and completion data, attaches the preallocated record while unregister is
excluded, and issues a nonblocking stop request after releasing MM and owner
locks. Unregister detaches subscriptions; x86 acknowledges them only after
local TLB reconciliation. The owner-data pin is released before the completion
callback, and the completion reference is released after that callback.
Owners without a stop contract and any bounded-storage or generation failure
retain the synchronous path.

A generic completion is reserved before the first PTE is removed and owns the
affected page disposition as well as references to the matching owner
instances. After the mapping change publishes its generation, MM may request
stops outside locks and let ordinary CPU targets complete synchronously. The
completion releases its folios or page-table pages only after those CPU flushes
and all matching owner acknowledgements finish. A later mapping change still
uses its ordinary shootdown. Owners without a stop contract remain synchronous.
If storage cannot be reserved, callers keep the existing synchronous path.
There is no timeout that can make an unacknowledged invalidation complete.
``mmu_gather`` needs a separate completion owner for its data-page and
page-table batches; changing the x86 TLB hook cannot transfer those lists by
itself.

The current NMI escape is ring-3-driver-specific, requires a controller
request, and does not cover kernel-mode owners; it cannot provide this
generic contract. The existing x86 clean-folio completion is a prototype of
page-lifetime transfer, not the generic owner/MM API described above.

The current NMI escape is not an MM-callable stop-and-ack operation. The
driver's ``cpu_accel_user_nmi()`` accepts an escape only when the saved frame
is from user mode on the configured CPU and the admitted image is active; it
redirects that frame to the image's pinned escape entry and stack. The
controller sends the NMI and waits up to one second, but a timeout does not
prove that the owner exited or that its TLB was reconciled. A direct kernel-mode
owner does not match the handler's user-mode check and cannot be unwound by
this path. The architecture owner record now accepts an optional per-owner
stop callback, but it still has no error-returning quiesce interface. NMI
delivery, a timeout, or an inactive state by itself is therefore not a TLB
acknowledgement.

The in-tree kernel lifecycle workload has a cooperative ``stop_requested``
flag, but checks it only at its workload loop boundary, after the optional
buffer copy. ``x86_cpu_accel_direct_enter()`` now registers an optional stop
callback with the owner. The in-tree kernel lifecycle callback publishes its
cooperative stop flag; other callbacks that do not promise to poll and return
are left on the synchronous fallback. There is no architecture-enforced stop
deadline. A future quiesce interface would still need an explicit completion
after the owner returns and a post-exit TLB acknowledgement. If an owner cannot
confirm exit, a timeout cannot let a void MM flush hook return as if
invalidation completed; its caller must keep waiting or own the affected page
lifetime through a completion object.

The stop request, owner exit, and TLB acknowledgement remain separate events.
Synchronous IPI flush paths reserve target CPUs against new owner entry before
requesting stops. They request a stop only from an owner that registered a
callback; callbacks without that capability stay on the synchronous fallback.
The request is marked under the per-CPU owner lock, which pins its callback
data; the callback itself runs after dropping both that lock and the global
ownership lock. It must not sleep, wait for owner exit, or re-enter the owner
API. The ring-3 callback raises the existing owner-specific escape NMI; the
image, stack, and exit helper remain pinned for the duration of ownership.

A stop request does not acknowledge a flush. The synchronous IPI completion
remains the flush acknowledgement for these paths, and filtered ring-3 owners
continue to rely on generation reconciliation before re-entry. A callback that
does not return, or an owner that cannot take the IPI, can still block the
synchronous caller indefinitely.

The ``cpu_accel:tlb_flush_wait`` event brackets a synchronous flush whose
target set overlaps active owners. Its unique ID, target counts,
and caller identify the flush; a ``begin`` without a matching ``complete``
shows that the synchronous operation has not reported completion. On an
mm-scoped path, a ring-3 owner in another ``mm`` may be filtered from the IPI
set and reconcile its generation before re-entry; ``complete`` marks the
flush operation's completion contract, not necessarily a local invalidation
on every recorded owner target. The
``cpu_accel:owner_stop_request`` and
``cpu_accel:owner_exit_complete`` events expose per-owner progress. They
include the CPU and owner generation so events can be paired across reuse of a
worker CPU. The stop event records whether a callback exists and whether this
call dispatched it, along with the requesting call site. The exit event is
emitted only after the owner's required local TLB reconciliation and reclaim
acknowledgements complete; its generation and unscoped-flush fields describe
the state reconciled at exit. With tracefs mounted at
``/sys/kernel/tracing``, enable the events and read ``trace_pipe``::

  echo 1 > /sys/kernel/tracing/events/cpu_accel/tlb_flush_wait/enable
  echo 1 > /sys/kernel/tracing/events/cpu_accel/owner_stop_request/enable
  echo 1 > /sys/kernel/tracing/events/cpu_accel/owner_exit_complete/enable
  cat /sys/kernel/tracing/trace_pipe

Correlate flush records by ID and owner records by CPU and generation;
``callback_sent`` distinguishes the one-shot callback dispatch from later stop
requests for the same owner. If tracing was enabled before the flush and the
buffer reports no lost records, a flush ``begin`` without ``complete`` means
the synchronous flush has not reported completion. Likewise, a stop request
without a matching exit event means that owner's completion has not been
reported. Silence alone is not evidence of completion. These events do not
replace synchronous IPI completion or impose a timeout.

Owner exit clears the runnable-owner state, then remains counted as active and
blocks new entry on that CPU until any required local TLB flush and reclaim
acknowledgements complete. The local flush reconciles every pending ``mm``
generation and unscoped invalidation recorded for that owner; CPU offlining,
maintenance, and global-flush selection continue to see the owner until this
completion. If a mapping change is made after the owner's exit flush, the
caller must still wait for the ordinary shootdown for that change. An MM hook
with no error return cannot treat a stop timeout as permission to continue; it
must keep the synchronous wait or transfer the affected page lifetime to a
completion object.

Kernel mapping invalidation and kernel code maintenance have separate
requirements:

* Kernel virtual mapping changes may use the synchronous kernel-address-only
  flush path while an owner is active. It preserves the owner's user TLB
  entries and keeps the old kernel translation valid until the target CPU
  handles the flush. It may wait for owner exit.
* Kernel text updates, including jump-label/static-key, ftrace, kprobe,
  livepatch, BPF JIT, and module text changes, must retain their existing
  text-patching rendezvous requirements. TLB invalidation alone does not make
  an instruction patch safe. Any rendezvous must include the owned CPUs and
  complete before the patcher reports success or releases old code; if it
  waits through deferred IPIs, it can wait for owner exit.
  The x86 SMP text-poke batch uses an INT3 transition and synchronous
  ``smp_text_poke_sync_each_cpu()`` rendezvous. That path may wait for an active
  owner, but it is not a global guard for code-update paths that do not use the
  same rendezvous.
* Changes to mappings or code used by exception entry, NMI, fault handling, or
  recovery must either quiesce the owner before retiring the old path or keep
  both the transition path and its backing pages valid for every active owner.
  A successful TLB flush by itself does not establish this semantic safety.

Runtime ``set_memory*()`` caller audit
--------------------------------------

The x86 CPA layer keeps these operations synchronous. Depending on the change
and its aliases, ``cpa_flush()`` uses synchronous per-CPU callbacks or
``flush_tlb_all()``; cache-attribute changes may also flush caches. While an
accelerator owns a target CPU, its deferred callback is serviced after owner
exit and the synchronous caller waits for that completion. A private
allocation or device-only buffer therefore does not make it safe to return
before the flush completes.

Representative runtime callers fall into these lifecycle groups:

* Executable-memory helpers such as ``execmem``, BPF program packs and
  trampolines, module strict-RWX transitions, and the SRAM execute helper
  change permissions as code is built, published, and retired. Their
  code-publication and text-lifetime rules are independent of TLB completion.
  The x86 ITS ``set_memory_x()`` caller for core thunks is reached from the
  ``__init`` alternatives pass. Module thunks use ``execmem`` while the
  module's retpoline patching transaction holds the owner-draining text
  mutex; their ROX transition completes before module initialization can
  publish the code.
* DMA and confidential-computing paths change encryption or cache attributes
  for allocations such as direct DMA buffers, SWIOTLB bounce buffers and
  pools, ALSA WC DMA pages, IOMMU command buffers, SFS command buffers,
  virtual PTP pages, guest report buffers, Hyper-V shared pages, and KVM PAE
  roots. Their reverse transition follows device teardown or the end of the
  hypervisor-sharing lifetime. SEV-SNP host RMP transitions call
  ``adjust_direct_map()``, which can split a large entry with
  ``set_memory_4k()``. Its synchronous CPA flush completes before the RMP
  state changes.
* Device page tables and trace buffers, including AGP, AMD/Radeon GART tables,
  Intel trace buffers, and staging media page tables, have device or userspace
  lifetime rules that must be drained before their attributes are restored.
  The system DMA-BUF heap similarly changes encryption state around buffer
  exposure and release. Dell firmware-update buffers change cache mode over
  their staging lifetime. Hibernation restore protection operates on its own
  restore pages. The terminal x86 kexec shutdown path has separate owner-drain
  rules described above; kexec-time page transitions still use CPA's
  synchronous completion.

Most remaining x86 callers establish platform mappings during startup: IDT
protection, the real-mode trampoline, EFI and legacy PCI BIOS attributes,
kernel section permissions, and initial direct-map 4K splits, including KFENCE
and AMD IOMMU setup. They run before a userspace accelerator owner can be
admitted. The runtime SEV-SNP path above is hardware-specific and is not
covered by the test VM.

No ``set_memory*()`` call exists in ``drivers/cpu_accel``. The current
accelerator driver has no import or registration path for these external
buffers; its ring-3 image admits only its validated mappings, and the current
kernel workload receives its own registered work region. This audit does not
prove arbitrary direct callbacks cannot reference such memory. Any future
accelerator or NIC integration that shares one of these regions must add an
explicit ownership handoff that drains CPU and device users before changing
attributes. It must not weaken CPA's synchronous completion contract.

The x86 prototype has an owner-drain maintenance gate. The x86 text-mutex
wrappers first bar new accelerator admissions, request a stop from each owner
with a registered nonblocking callback, and wait for existing owners to exit
and finish TLB reconciliation before taking the existing mutex; unlock releases
the mutex before reopening admission. A stop callback is only a request: owners
without one, or owners that do not honor it, retain the synchronous wait. The
gate therefore spans the whole text-mutex transaction, including its
text-patching rendezvous, and may wait without a bound. It covers x86
text-poke clients and generic kprobes that use this mutex.
MTRR add and delete operations also take the gate before the CPU-hotplug read
lock and hold it through their stop-machine rendezvous and MTRR map rebuild.
Late microcode reload takes the gate before its CPU-hotplug read lock and holds
it through the update; its static-key text-patch transactions nest the same
gate in the updating task.
TDX module installation takes the gate before its CPU-hotplug read lock and
holds it through the stop-machine update.
The public ``stop_machine()`` entry point takes the gate before its CPU-hotplug
read lock and holds it through the rendezvous, covering other generic
stop-machine callers on x86. The inactive-CPU variant cannot sleep, so it
reserves the gate without waiting and returns ``-EBUSY`` if an owner or other
maintenance transaction is active; the cache CPU-online callback propagates
that failure. Direct ``stop_machine_cpuslocked()`` callers still need an
explicit gate or a proof that CPU-hotplug locking excludes owners.

CPU teardown takes the gate in ``_cpu_down()`` before its CPU-hotplug write
lock and holds it through the teardown callbacks, including the
``stop_machine_cpuslocked()`` rendezvous. This prevents an owned CPU from being
offlined and bars new owners until the teardown transaction completes.

The current x86 call-site audit found direct ``stop_machine_cpuslocked()``
users in CPU teardown, MTRR add/delete, late microcode reload, and TDX module
installation; each takes the maintenance gate before the CPU-hotplug lock.
Cache CPU-online initialization uses ``stop_machine_from_inactive_cpu()``,
which makes a nonblocking reservation and propagates ``-EBUSY``, while ordinary
``stop_machine()`` callers use the gated wrapper. This inventory is specific
to the current tree; new direct callers need their own lock-order and
rendezvous review.

The exported ``stop_core_cpuslocked()`` rendezvous is used by Intel IFS. Its
``do_core_test()`` entry reserves the nonblocking maintenance gate before the
CPU-hotplug read lock and returns ``-EBUSY`` if any owner is active. The
reservation spans the per-core rendezvous loop. This check is global and
conservative; new callers must establish the same owner exclusion before
taking CPU-hotplug locks.

KGDB's x86 breakpoint path uses ``text_poke_kgdb()`` for its read-only text
fallback after an NMI roundup. The generic KGDB loop proceeds after its
one-second wait even if not every online CPU entered the debugger, so the
roundup alone cannot establish accelerator-owner quiescence. The x86 KGDB
entry now takes the nonblocking maintenance reservation before the roundup.
If an owner or another maintenance transaction is active, it declines the
debugger entry and leaves the exception to the normal handler. Otherwise it
holds the reservation until the debugger CPUs resume, including a debugger CPU
handoff. This prevents accelerator execution throughout KGDB's patching
session; the existing timeout behavior for ordinary Linux CPUs is unchanged.
After the session, every successfully armed KGDB software or hardware
breakpoint continues to block accelerator admission until it is removed. This
prevents an owner from later executing a patched kernel instruction or
triggering a KGDB hardware breakpoint after the session reservation is gone.

The mapping and exception-path audit classifies the current mechanisms as
follows:

* The x86 CPA ``set_memory*()`` path and ``flush_tlb_kernel_range()`` complete
  their TLB work synchronously. If an owner is in the target set, its flush
  callback runs after owner exit, and the caller cannot release or reuse the
  affected backing memory before completion. This establishes translation
  lifetime; it does not establish that changing permissions or code semantics
  while an owner executes is safe. The central x86 ``set_memory*()`` path has
  no global owner gate, and its callers span boot setup, page allocation,
  executable memory, and device mappings. In particular, ``DEBUG_PAGEALLOC``
  reaches CPA from allocator contexts and deliberately bypasses the normal CPA
  lock. Any runtime caller changing code or mappings used by exception, NMI,
  fault, or recovery execution still needs a call-site proof that the target
  is unpublished, protected by the maintenance gate, or valid throughout the
  transition.
* Local-only kernel invalidation is an explicit exception. KFENCE and KMMIO
  use ``flush_tlb_one_kernel()``; KFENCE documents that it cannot send IPIs in
  allocator or fault context and tolerates stale translations on other CPUs.
  ``CONFIG_DEBUG_PAGEALLOC`` is another exception: ``__kernel_map_pages()``
  updates the direct-map PTE through CPA, then deliberately uses a local
  ``__flush_tlb_all()`` because a remote flush can deadlock in allocator
  context. This is outside the normal synchronous CPA flush path. None of
  these local-only paths records an accelerator owner's pending TLB
  generation, so their best-effort fault/protection behavior is not a
  remote-invalidation guarantee for an active owner. Any operation that
  requires remote invalidation for correctness must use a synchronous path or
  prevent owner overlap.
* Vmalloc unmap and kernel page-table reclamation clear the mapping and
  synchronously flush kernel translations before the virtual address, data
  page, or page-table page can be reused. These paths may wait for an active
  owner to exit.
* BPF arena teardown has both kernel and user mappings to retire. It clears
  the ``init_mm`` alias, completes ``flush_tlb_kernel_range()``, zaps the range
  from every registered user VMA, and frees the pages only after both steps.
  The kernel-range flush does not replace the per-``mm`` user unmaps. The
  current accelerator worker's sealed ``mm`` rejects extra VMAs, including a
  BPF arena VMA. Arena teardown takes ``mmap_read_lock()`` for each user ``mm``;
  the worker holds its ``mm`` write-locked during an active epoch, so teardown
  must wait for owner exit before zapping that VMA. Any future owner allowed to
  retain one must be quiesced before teardown and remain excluded until both
  invalidations complete and the pages are safe to reuse.
* The direct-map ``*_noflush()`` helpers do not complete their own TLB
  transition. Vmalloc's ``VM_FLUSH_RESET_PERMS`` teardown invalidates the
  direct-map entries, flushes the corresponding direct-map range, then
  restores the default mapping before freeing the pages. Secretmem and
  hibernation pair direct-map invalidation with an explicit kernel-range flush
  before the page can be exposed or reused. A new caller must provide the same
  completion and page-lifetime ordering.
* Hibernation restore-image protection changes private restore-buffer pages
  with ``set_memory_ro()`` and ``set_memory_rw()``. A buffer page becomes
  read-only after its image data has been consumed, and write access is
  restored before ``swsusp_free()`` releases it. These are data buffers, not
  executable or exception mappings, and the current accelerator ABI does not
  map them into the worker ``mm``. Their lifetime still depends on synchronous
  CPA completion before page reuse; this path does not need the kernel
  code-patching gate.
* Cache-type changes through ``ioremap()`` reject ordinary system RAM that is
  not reserved; PAT tracks cache types and rejects incompatible aliases, and
  direct-map updates complete a synchronous TLB flush. Runtime driver buffers
  still need their own exclusion rule. For example, Intel Trace Hub applies
  UC before publishing its buffer to users and restores WB only after its
  user and mmap counts drain. A cache-type transition on any PFN mapped by an
  accelerator must wait until that mapping is no longer active.
* Executable-memory permission changes also depend on publication and object
  lifetime. The execmem cache fills unused blocks with trapping instructions
  before publishing them as free. Code generators keep new images unreachable
  until their contents and ROX permissions are ready. BPF trampoline teardown
  waits for task RCU and, where needed, its in-flight reference count before
  freeing the image; module unload waits for its RCU readers before releasing
  module memory. With ``CONFIG_MITIGATION_ITS``, built-in indirect-thunk pages
  that use ``set_memory_x()`` are allocated from the ``__init`` alternatives
  pass and sealed by ``its_fini_core()``. Module ITS thunk generation and
  relocation run through ``its_init_mod()``/``its_fini_mod()`` during
  ``module_frob_arch_sections()``, under the owner-draining text-mutex gate;
  the module remains unpublished during this setup. These are caller
  lifetime rules, not a global owner gate. The module permission sequence was
  checked separately: ``complete_formation()`` finalizes core section
  permissions before setting ``MODULE_STATE_COMING`` and running module init.
  With strict module RWX enabled, that step makes core data NX and text ROX;
  core rodata is also made RO when rodata protection is enabled. After
  successful init publishes ``MODULE_STATE_LIVE``,
  ``module_enable_rodata_ro_after_init()`` seals ``.data..ro_after_init`` when
  strict module RWX and rodata protection are enabled. This final RW-to-RO
  transition relies on the section's no-more-writes contract, so concurrent
  readers remain valid; any live code update or post-init write still needs
  its own synchronization rule.
* Confidential-memory conversions use the x86 memory-encryption lock to
  coordinate conversion state, but that lock does not drain accelerator
  owners. The Hyper-V conversion path explicitly requires callers to keep the
  range unused and unreferenced throughout the transition. Any range that an
  accelerator can access through a user mapping must be excluded from
  conversion until that access is gone. The generic DMA allocation path
  converts newly allocated pages before returning them and restores the
  encryption state before freeing them; a future DMA handoff of an
  accelerator-shared page needs an explicit owner transfer around conversion.
* Built-in exception tables are fixed after initialization. IDT and FRED
  system-vector installation helpers are ``__init``-only and reject updates
  after setup. Module and BPF exception-table lookup, and NMI handler-list
  traversal, use RCU; module removal and NMI-handler unregister wait for a
  grace period before releasing the old table or handler. These lifetime
  rules protect lookup readers without requiring an owner gate for those
  mutations.
* The emergency NMI handler bypasses the registered list only for the one-shot
  crash CPU shootdown and remains installed while the machine stops; it is not
  a runtime reconfiguration path.
* KGDB retains its timeout-based roundup for ordinary Linux CPUs. On x86, its
  entry first takes a nonblocking maintenance reservation and declines debugger
  entry if an owner or another maintenance transaction is active. Once
  admitted, new owners cannot start during the roundup and debugger session.
  Armed breakpoints continue to block admission after the session until
  removal. The timeout therefore does not leave an unclassified
  accelerator-owner path, although the generic roundup alone does not stop
  every ordinary CPU.

The current x86 direct-call sweep found no additional runtime writer of
published kernel text or exception state outside the established gates and
explicit lifetime rules above. This is a current-tree result, not an
owner-aware guarantee from central CPA: new ``set_memory*()`` or kernel
mapping callers, and any new external-buffer import path, still need a
call-site rule. Local-only KFENCE, KMMIO, and ``DEBUG_PAGEALLOC`` invalidation
remains best-effort and cannot provide remote owner invalidation. A blanket
gate in ``stop_machine_cpuslocked()`` would run after callers acquired
CPU-hotplug locks, while existing text-patch paths acquire the gate before
their patching locks; that ordering needs call-site review to avoid a lock
inversion. New operations must be classified as allowed, routed to
housekeeping, deferred, rejected, or requiring controlled owner termination.
Operations that cannot prove they include the owned CPU in their execution
and mapping rendezvous are unsupported while accelerator ownership is active.

The direct x86 ftrace CPA callers have a narrower, source-verified rule.
``set_ftrace_ops_ro()`` runs from boot-time ``mark_rodata_ro()``. A newly
generated trampoline is made ROX before ``arch_ftrace_update_trampoline()``
publishes its address to the caller. Updates to an existing published
trampoline take the owner-draining text-mutex wrapper around
``smp_text_poke_single()``. This closes those ftrace cases; it does not imply
that other executable pools or subsystem ``set_memory*()`` callers use the
same gate.

The live x86 text-patching call-site audit found the owner-draining
maintenance reservation around ftrace code updates (including function-graph
hooks), jump-label transforms and batch application, static calls, BPF text
pokes, module relocations, kprobe mutation, and callthunk patching.
``text_poke_copy()`` and ``text_poke_set()`` take the same reservation
themselves. KGDB uses its nonblocking reservation described above. Newly
generated code is populated before publication, while early text patching is
limited to initialization or unpublished module text. Uprobes change user
process mappings and are governed by the per-address-space lifetime rules,
not this kernel-text audit. This inventory describes the current tree; new
writers still need to be checked at their call sites.

Each additional interlock must cover the entire maintenance transaction:
reserve owner admission before changing code or mappings, retain that
reservation through every execution rendezvous and TLB completion, then
release it. A reservation started only by the final
``smp_text_poke_sync_each_cpu()`` is too late to protect the preceding writes.
The SMP text-poke sequence uses an INT3 transition and repeated synchronous
core-sync callbacks; its callbacks can wait for owners, but that sequence does
not cover stop-machine, exception-table, IDT/NMI, or other maintenance paths
automatically. Each such path needs an explicit interlock or an explicit
reject/defer/quiesce rule before mutation.

Until these rules are implemented for an architecture, call-function replay
is only a mechanism detail. The direct APIC prototype must not advertise
complete APIC ownership, TLB-shootdown suppression, or protected address-space
isolation.

MM construction boundary
========================

When built as a module, the ``cpu_accel`` driver cannot construct a separate
populated user ``mm`` by combining the interfaces currently exposed to it.
``mm_alloc()`` is exported only for KUnit, ``insert_vm_struct()`` is
MM-internal, and ``switch_mm_irqs_off()`` is an architecture-level address
space switch rather than a task ``mm`` lifecycle API. Helpers such as
``vm_insert_page()`` map pages into an existing VMA; they do not create and
own a complete address space or arrange for user execution to run as a task
whose ``current->mm`` is that address space.

This does not mean the first sealed ``mm`` requires a new MM-core allocator.
The normal ``execve()`` path creates a fresh ``mm`` for a worker task, and the
CLI uses that path so the worker does not share the companion's ``mm``. The
current prototype now removes the worker's ordinary runtime VMAs and verifies
the complete remaining VMA set, including the fixed architecture-owned
``[vsyscall]`` mapping. The driver pins the admitted image, private stack, and
registered private RSEQ user-area pages, then holds the worker ``mm``
write-locked for the active epoch, preserving the existing ``current->mm`` and
loaded-address-space relationship. This path avoids changing the task's
``mm`` during execution.

If the design instead requires the driver to assemble a new ``mm`` from
registered pages, that needs a narrow MM-core interface and a task lifecycle
that make construction, user execution, and teardown one operation. The
interface must provide at least:

* creation of an otherwise empty ``mm`` with normal architecture and MM
  accounting initialized;
* installation of only the admitted image, private stack/data, and registered
  shared mappings, with ordinary VMA, reverse-mapping, page-reference, and
  page-table accounting maintained;
* prefaulting and freezing those mappings for an active epoch;
* execution in a task context whose ``current->mm`` and loaded address space
  agree, with a defined way to restore the task's prior address space; and
* teardown only after accelerator ownership ends, required local and remote
  TLB invalidations complete, and mapped pages can safely be released.

For that driver-assembled path, the x86 ownership record must identify the
address space actually loaded for accelerator execution, independently of the
task that submitted the request. The existing process-``mm`` backend relies
on those identities being the same. Do not work around the missing MM
lifecycle by assigning a CR3 directly or by relabeling the worker's ordinary
process ``mm`` as sealed.

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

The following are implementation checkpoints for the x86 prototype, not the
numbered phases in the project v1.1 roadmap.  Checkpoint 10 is address-space
and TLB ownership work; it is not project Phase 10 (SMP).  This work supports
the sealed-execution requirements in Phase 4 and the adversarial isolation
and maintenance validation in Phase 8.

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
7. [completed for x86 prototype] Add ABI 14 selective reschedule-IPI
   ownership.  Remote scheduler reschedule requests are deferred and counted
   while the direct backend owns the CPU, then one request is replayed after
   exit.
8. [completed for x86 prototype] Add ABI 15 selective call-function-IPI
   ownership.  Remote call-function requests remain queued, are deferred and
   counted while the direct backend owns the CPU, then are replayed after
   exit. On native x86 this also delays TLB flush callbacks carried by
   call-function work, but does not define TLB ownership or cover other
   interrupt sources.
9. [completed for x86 prototype] Add ABI 16 accounting for TLB flush target
   batches that overlap direct CPU ownership across the x86 IPI and INVLPGB
   paths. This is target telemetry; it does not track pending generations or
   prove that invalidations completed.
10. [in progress for x86 prototype] Implement the address-space/TLB ownership
    policy above. The current implementation records per-owner invalidation
    generations, reconciles the local TLB before releasing pinned image pages,
    filters mm-scoped flushes only for ring-3 owners in a different ``mm``;
    same-``mm`` range flushes request owner stop and synchronously wait for
    reconciliation. Filtering same-``mm`` owners requires the pending
    ``mmu_gather`` completion-ownership work. Flush targets remain reserved
    against new ownership. Kernel-address-range flushes use
    INVLPGB plus system-wide completion when available with no active owner;
    when an owner is active, kernel-only IPIs wait for exit and preserve user
    translations. Standalone full and all-nonglobal flushes also wait for
    owners. Batched unmap through ``arch_tlbbatch_flush()`` remains synchronous
    for each affected address space: an owner whose own ``mm`` was changed,
    and every kernel-mode owner, stays in the IPI target set. A ring-3 owner
    in a different ``mm`` may be omitted because the changed ``mm`` generation
    forces a local flush before that CPU can use the address space again.
    Returning with an affected ``mm`` usable while its stale translations
    remain would allow premature I/O or page reclamation. Any path that omits
    an owner must retain affected pages until owner-exit acknowledgment or
    guarantee owner quiescence. The first bounded vmscan clean-folio completion
    path is implemented; dirty/writeback, migration, generic batch, and
    ``mmu_gather`` paths remain synchronous. The prototype's
    user NMI escape requires a driver/controller request and cannot serve as
    the generic MM quiesce path. The current prototype seals an exec-created
    worker ``mm`` with a complete VMA
    allowlist, including the fixed x86 ``[vsyscall]`` exception. The sleepable
    maintenance gate now requests registered owner stops and waits for owner
    exit after local TLB reconciliation; it may still wait without a bound.
    The ``owner_stop_request``/``owner_exit_complete`` and
    ``tlb_flush_wait`` tracepoints, supplemented by optional IPI/CSD events,
    expose the stop and callback sequence for diagnosis. An unmatched flush
    begin shows incomplete synchronous work, but these observational events do
    not bound it. The full-flush path preserves invalidation-before-reuse, and
    there is no safe timeout for these void MM hooks. The current x86 caller
    audit found no additional live kernel-code or exception-state writer
    outside the established gates and lifecycle rules above. The source review
    found no safe flush-hook-only extension for dirty reclaim or migration:
    each would need to transfer and resume its data operation after owner
    acknowledgement. Generic batched unmap also lacks the affected-page
    disposition, while ``mmu_gather`` has intermediate data/table drains and
    immediate GUP-fast and RCU lifetime barriers. Keep these paths synchronous
    until MM core can reserve a completion before unmap and own the caller's
    deferred operation or page disposition across every release point. The stable
    owner-subscription API now provides post-reconcile acknowledgement for the
    existing clean-folio prototype. The next distinct MM-core step is an
    ``mmu_gather`` completion owner that transfers data/table batches at every
    drain point while preserving its immediate walker barriers.
    Dirty/writeback reclaim and migration remain synchronous until their data
    operations can also be transferred as post-ack continuations. A focused VM
    test unmaps a touched
    2 MiB mapping from another ``mm`` while the target CPU is ring-3 owned,
    then uses privileged ``/proc/self/pagemap`` and ``/proc/kpageflags``
    inspection to track a freed data-page PFN and, when observed, the
    PTE-table PFN. While the owner remains active, the data-page PFN is reused
    in a live 16 MiB mapping; the 00595 run also observed the PTE-table PFN
    reused for a new PTE table in the adjacent 2 MiB slot. PTE teardown returns
    before owner exit. On re-entry, the old VA faults while the replacement data mapping still
    holds the reused PFN. PTE-page reuse is optional because
    ``/proc/kpageflags`` may be unavailable or the bounded allocation probes
    may not recycle a table page.
    The harness now includes a second owner run for reclaim completion: it
    writes and fsyncs a one-page regular file, keeps a read-only alias in the
    test controller's ``mm``, and starts a ring-3 worker that touches the
    corresponding unpinned page in its own ``mm`` before remaining active.
    ``MADV_PAGEOUT`` through the test-controller alias must stop the registered
    owner, reclaim the page, and preserve its contents on refault. This
    exercises the current clean-file
    page vmscan completion path; the focused VM run is still pending.
    The focused test also forks a page holder and writes the parent's
    write-protected mapping while the ring-3 owner runs in another ``mm``.
    It checks that the holder still reads the original page while the parent
    sees its private copy, then checks the private value again after the parent
    ``mm`` re-enters the target CPU. This exercises the ``wp_page_copy()``
    ``flush_tlb_mm_range()`` path and generation reconciliation; it does not
    cover kernel code or exception mapping updates.
    The 00595 validation passed the COW/re-entry, pageout, data/PTE-PFN reuse,
    and deferred-reschedule checks without a soft-lockup or RCU-stall log.
    This does not prove safety for every page-reuse or speculative-walk case.
    INVLPGB completion protects TLB translation lifetime but does not make
    active kernel code patching safe.
    A separate trace-assisted run loaded the ``dummy`` module while the owner
    was active. The module loader's ``set_memory_nx()`` reached
    ``kernel_tlb_flush_all()``; the stop callback completed and the owner
    exited before the synchronous IPI handler ran, and ``modprobe`` succeeded.
    This exercises module mapping setup, not patching code that is already
    executing. A follow-up audit found ``__split_large_page()`` held
    ``pgd_lock`` over its synchronous ``flush_tlb_all()``. It now publishes
    the fully populated split table under ``pgd_lock``, releases that lock,
    then flushes while ``cpa_lock`` still serializes attribute changes. On
    00596, ``modprobe dummy`` completed with a CPU2 owner active during 256
    short-lived process creates/exits; telemetry counted one deferred TLB
    target and call-function request, with no soft-lockup or RCU stall. The
    owner CLI returned status 1 because its delayed user-escape ioctl raced
    with the kernel stop and got ``EINVAL``. A temporary VM trigger exercised
    both kernel-range IPI branches with a direct owner on CPU2. A one-page
    vmalloc purge traced the range
    handler and ``do_kernel_range_flush`` on the owner CPU; a 40-page purge
    selected the full-flush sentinel and traced ``kernel_tlb_flush_all()``
    followed by ``do_flush_tlb_all`` on that CPU. In both runs the owner
    stopped within milliseconds, after the stop callback and before the
    synchronous IPI handler, and the kernel log had no soft-lockup, RCU-stall, BUG, Oops, or
    panic matches. The trigger and harness were temporary VM files, not an
    in-tree regression test. These tests do not exercise INVLPGB.
    On 00597, the COW/pageout/PFN-reuse regression passed again with seven TLB
    targets and one deferred reschedule. The new ``tlb_flush_wait`` trace
    bracketed ``modprobe dummy``'s kernel full flush: flush ID 20 began with
    eight targets and one owner, CPU2's stop callback was sent, and the same
    ID completed before the matching owner-exit event. ``modprobe`` succeeded;
    the CLI later returned status 1 because its delayed user-escape ioctl
    raced with the kernel stop and returned ``EINVAL``. The kernel-log scan
    found no new soft-lockup, RCU-stall, BUG, Oops, or panic records.
    Global-ASID INVLPGB broadcasts reserve all online CPUs through ``TLBSYNC``
    so no new owner can enter during the invalidation.
    A driver-assembled ``mm`` would additionally
    require MM-core construction and task-lifecycle APIs. Add IOMMU-backed
    ``DMA`` regions and userspace/NIC integration only after this non-networked
    memory contract is stable.

    The MM-core owner registry binds x86 owner instances to their loaded
    ``mm`` and serializes admission against batched-unmap publication. The
    generic subscription API now validates owner generation, holds owner and
    completion references, requests stops outside registry locks, and detaches
    subscriptions during unregister. The x86 owner-exit path acknowledges
    after local TLB reconciliation. Ordinary CPU flushes and callers without
    an explicit page disposition remain synchronous, and no timeout authorizes
    page reuse. Do not extend asynchronous reclaim to ``mmu_gather`` or other
    page dispositions in this step.

    A later run on ``7.3.0-rc3-accel-tlbfix-00604-g44f7b92cde35-dirty``
    reported soft lockups during a module load. CPU0's ``modprobe`` was
    waiting in ``smp_call_function_many_cond()`` from the CPA
    ``flush_tlb_all()`` in ``__split_large_page()``; an NMI backtrace still
    found CPU2 in the ring-3 ``cpu-accelctl`` TSC loop. CPUs 4 and 7 were also
    spinning in ``pgd_alloc()`` and ``pgd_free()``. This shows that the
    synchronous kernel-wide flush did not complete while the ring-3 owner was
    still executing, but the supplied log does not show whether the stop
    callback was absent, its NMI was not accepted, or another target blocked
    the rendezvous. The ``owner_stop_request``, ``owner_exit_complete``, and
    ``tlb_flush_wait`` trace records were not captured, and the VM does not
    retain the previous boot journal. Treat the earlier clean runs as limited
    coverage, not as validation of this failure path. Reproduce with those
    trace events enabled and an independent bounded owner-escape fallback
    before changing the stop/ack protocol.

    A follow-up run on the same 00604 kernel used the checkout-matched
    ``cpu-accelctl`` and ``test-tlb-generation`` tools. The COW and reclaim
    regression passed, including ``wp_page_copy()`` and ``MADV_PAGEOUT``
    while the other-mm ring-3 owner was active. Tracing recorded six
    ``tlb_flush_wait`` begin/complete pairs with two targets and one owner;
    the mm-scoped owner filter kept the ring-3 CPU out of those IPI masks, so
    there were no matching stop-request events. Both owner exits completed
    with ``stop_requested=0``. The run reported no soft-lockup, RCU-stall, BUG,
    Oops, or panic records. This validates the exercised mm-scoped COW and
    reclaim paths, but it does not reproduce the global CPA full-flush stall.

    The VM's installed module tree contains only ten modules, all currently
    loaded for the accelerator, network, root filesystem, or ``/boot``;
    ``dummy.ko`` is absent. A guarded ``modprobe dummy`` attempt stopped at
    this precondition before tracing or changing VM state. A no-op probe module
    in ``tools/cpu_accel/tlb-flush-probe/`` now provides the module-load CPA
    trigger without unloading a live system module. The standalone
    ``tools/cpu_accel/test-tlb-flush.sh`` captures the flush, stop, and exit
    events in a separate tracefs instance and gives the ring-3 owner a bounded
    escape fallback. The installed ``/usr/local/bin/cpu-accelctl`` rejects the
    kernel's shared ABI; use the checkout-matched binary staged under
    ``/tmp``.

    On the same 00604 kernel, two direct probe loads during the
    non-cooperative ring-3 workload completed the global CPA flush. The trace
    showed one flush begin with eight targets and one owner, a stop callback
    sent to CPU2, the matching owner exit with ``stop_requested=1`` and an
    unscoped local flush, then completion of the same flush ID. ``insmod``
    succeeded and the probe was unloaded after the owner exited. Neither
    direct run reported a soft-lockup, RCU-stall, BUG, Oops, or panic. The first
    run's CLI returned an error because its timed escape ioctl raced with the
    completed owner stop and got ``EINVAL``; ``cpu-accelctl`` now accepts that
    result only when the shared state confirms successful escape. The second direct
    run passed, as did the standalone harness with a three-second bounded
    escape. Its trace had the same stop/exit/flush ordering and the CLI exited
    successfully.

    The harness can run ``test-tlb-cow-churn`` on a separate pinned control
    CPU by setting ``CPU_ACCEL_COW_CHURN_MS`` and ``CPU_ACCEL_STRESS_CPU``.
    Each fork child and its parent write the same private page while both
    address spaces remain alive, forcing two COW faults per iteration. Three
    two-second runs on CPU3 completed 17,540, 17,940, and 17,715 forks
    (106,390 COW writes total) while the non-cooperative owner ran on CPU2.
    All three global CPA flushes stopped the owner, observed its exit, and
    completed the same flush ID. The second and third traces also captured
    two-target mm-scoped flush pairs immediately before global flush IDs 12
    and 14. All runs reported no soft-lockup, RCU-stall, BUG, Oops, or panic.
    This bounded concurrency test still does not reproduce the earlier
    intermittent soft lockup, so keep the synchronous fallback and continue
    increasing concurrent coverage before changing the stop/ack protocol.

    Three additional five-second runs on CPU3 completed 43,795, 44,183, and
    45,668 forks (133,646 total, with 267,292 forced COW writes) while the
    ring-3 owner ran on CPU2. Each global CPA flush stopped that owner, observed
    its exit, and completed the same flush ID. In the second run, twenty
    two-target mm-scoped flush pairs (IDs 16--35) completed immediately before
    global flush ID 36; the trace then showed the CPU2 stop request, matching
    owner exit, and flush completion. The first and third runs likewise
    completed global flush IDs 15 and 37 through the stop/exit path. All three
    runs reported no soft-lockup, RCU-stall, BUG, Oops, or panic. The reported
    intermittent lockup remains unreproduced; retain the synchronous fallback
    and continue controlled coverage before changing the stop/ack protocol.

    The harness also requires every observed ``tlb_flush_wait`` begin ID to
    have exactly one later completion, allowing interleaved flushes. Optional
    core IPI-send, CSD callback, and x86 call-function IRQ tracepoints capture
    target masks and callback delivery when available. After verifying that the
    VM was running ``7.3.0-rc3-accel-tlbfix-00604-g44f7b92cde35-dirty``, one
    five-second run completed 43,826 forks and 87,652 forced COW writes on
    CPU3. Global CPA flush ID 38 began with eight targets and one owner; its
    stop request targeted owner CPU2, the ``do_flush_tlb_all`` callback entered
    and exited on CPU2, the matching owner exit was observed, and that same
    flush ID then completed. The CSD trace also recorded callback delivery for
    the queued remote CPUs. There were no lost trace events or soft-lockup,
    RCU-stall, BUG, or Oops records, and the VM remained responsive. This adds
    low-level delivery evidence but does not reproduce the reported lockup;
    keep the synchronous fallback and continue the caller-semantics audit
    before changing the stop/ack protocol.

    The test-only fixture now retains one unpinned, read-only shared regular-
    file page in the worker and maps an alias in the companion. On kernel
    ``7.3.0-rc3-accel-tlbfix-00603-g7c09283ad6e9``, the focused reclaim test
    passed: ``MADV_PAGEOUT`` reclaimed the page after owner exit and the
    refaulted contents matched the file. This test does not check physical
    page reuse. Keep the mapping out of the normal workload ABI and continue
    rejecting unregistered VMAs.
