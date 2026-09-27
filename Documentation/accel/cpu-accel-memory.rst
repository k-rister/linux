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
area, when present. The kernel may update that area on the user-return
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
backend, ``flush_tlb_multi()`` now removes active ring-3 owners from an
mm-scoped flush mask before dispatching through either the native or KVM
paravirtual path. The owned worker ``mm`` is write-locked for the active
interval, so a flush for that ``mm`` is recorded and reconciled with a local
flush before unlock. A flush for another ``mm`` advances that ``mm``'s TLB
generation; ``switch_mm()`` performs the needed local flush before the owner
CPU can use it again. This avoids making a synchronous flush wait for an
interrupt-disabled ring-3 owner that is running a different address space.
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
translations from outliving the flush. Standalone full and all-nonglobal
flushes retain the synchronous IPI path during ownership because they would
evict active accelerator user translations. The task-local
``arch_tlbbatch_flush()`` path has a narrower exception: it may defer a ring-3
owner only when every affected ``mm`` is different from the owner's sealed
``mm``. Its per-``mm``
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
list only after reclaim has selected it for release. Other callers continue to
wait for the synchronous flush; returning with only a pending generation would
let them proceed as if invalidation had completed.

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
specific completion. Owner exit acknowledges those entries after its local
TLB reconciliation; a worker then uncharges and frees the folio.

Dirty folios, writable PTE batches, failed unmaps, DMA-pinned folios, folios
with buffer-release work, migration, huge-page collapse, and other callers
without a reclaim disposition keep the synchronous path. Exhausting the pool
or either fixed-size record array also falls back to synchronous flushing.
Completion storage is reserved before PTE removal. A stalled owner keeps its
folio and completion slot; after all 16 slots are occupied, further reclaim
can wait on the existing synchronous path. This is bounded backpressure, not a
timeout.

The generic ``arch_tlbbatch_flush()`` remains synchronous. Only the dedicated
vmscan completion path can defer a matching x86 ring-3 owner, and it retains
the folio until that owner acknowledges the generation. The per-CPU completion
list is drained by the orderly ``x86_cpu_accel_user_exit()`` path after its
local flush. A CPU-offline or recovery path that bypasses that exit must keep
the completion outstanding until it has independently established equivalent
quiescence; inactive state alone is not an acknowledgement.

``mmu_gather`` is a distinct range-flush path. On x86 it calls
``flush_tlb_mm_range()``; after the flush/generation rules permit reclamation,
``tlb_flush_mmu_free()`` releases its queued data pages and page-table batches.
An asynchronous redesign of that path would need to transfer those
``mmu_gather`` lists to its own completion-managed object. Those lists are not
part of ``arch_tlbbatch_flush()`` or its architecture batch.

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

The alternative is a generic owner-quiesce operation that can safely terminate
every supported owner type and report completion to MM callers. The current
NMI escape is ring-3-driver-specific, requires a controller request, and does
not cover kernel-mode owners; it cannot provide this contract. Until a generic
quiesce API exists, synchronous flush waits remain unbounded by design.

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

The x86 prototype has an owner-drain maintenance gate. The x86 text-mutex
wrappers first bar new accelerator admissions and wait for existing owners to
exit, then take the existing mutex; unlock releases the mutex before reopening
admission. The gate therefore spans the whole text-mutex transaction, including
its text-patching rendezvous. It covers x86 text-poke clients and generic
kprobes that use this mutex, and may wait without a bound for an owner to exit.
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
* The direct-map ``*_noflush()`` helpers do not complete their own TLB
  transition. Vmalloc's ``VM_FLUSH_RESET_PERMS`` teardown invalidates the
  direct-map entries, flushes the corresponding direct-map range, then
  restores the default mapping before freeing the pages. Secretmem and
  hibernation pair direct-map invalidation with an explicit kernel-range flush
  before the page can be exposed or reused. A new caller must provide the same
  completion and page-lifetime ordering.
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
  module memory. These are caller lifetime rules, not a global owner gate.
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

The audit does not establish semantic safety for every runtime
``set_memory*()`` caller. Kernel mapping changes that do not use the gate still
need a call-site rule, and the init-only and RCU lifetime rules above do not
cover future exception or NMI mutation paths. A blanket gate in
``stop_machine_cpuslocked()`` would run after callers acquired CPU-hotplug
locks, while existing text-patch paths acquire the gate before their patching
locks; that ordering needs call-site review to avoid a lock inversion.
Classify each remaining operation as allowed, routed to housekeeping,
deferred, rejected, or requiring controlled owner termination. Operations
that cannot prove they include the owned CPU in their execution and mapping
rendezvous are unsupported while accelerator ownership is active.

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
    filters mm-scoped flushes for the active ring-3 owner, and keeps flush
    targets reserved against new ownership. Kernel-address-range flushes use
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
    allowlist, including the fixed x86 ``[vsyscall]`` exception. Remaining
    work includes safe handling of full-flush waits and semantic safety for
    kernel code/exception mapping updates. A focused VM test unmaps a touched
    2 MiB mapping from another ``mm`` while the target CPU is ring-3 owned,
    then uses privileged ``/proc/self/pagemap`` and ``/proc/kpageflags``
    inspection to track a freed data-page PFN and, when observed, the
    PTE-table PFN. While the owner remains active, the data-page PFN is reused
    in a live 16 MiB mapping; an earlier VM run also observed the PTE-table
    PFN reused for a new PTE table in the adjacent 2 MiB slot. In the latest
    run, the PTE-page subprobe skipped because no candidate page was released
    during its bounded wait. PTE teardown returns before owner exit; after
    re-entry, the old VA faults while the replacement data mapping still
    holds the reused PFN. PTE-page reuse is optional because
    ``/proc/kpageflags`` may be unavailable or the bounded allocation probes
    may not recycle a table page.
    This does not prove safety for every page-reuse or speculative-walk case.
    INVLPGB completion protects TLB translation lifetime but does not make
    active kernel code patching safe.
    Global-ASID INVLPGB broadcasts reserve all online CPUs through ``TLBSYNC``
    so no new owner can enter during the invalidation.
    A driver-assembled ``mm`` would additionally
    require MM-core construction and task-lifecycle APIs. Add IOMMU-backed
    ``DMA`` regions and userspace/NIC integration only after this non-networked
    memory contract is stable.
