# Phase 10 blast-radius MMU and reclaim audit

## Scope

This note records the semantic review of the memory-management files in
`Documentation/accel/phase10-blast-radius-semantic-inventory.tsv`:

- `mm/Makefile`
- `mm/folio.c`
- `mm/init-mm.c`
- `mm/internal.h`
- `mm/mmu_gather.c`
- `mm/rmap.c`
- `mm/slub.c`
- `mm/vmalloc.c`
- `mm/vmscan.c`

The review baseline is `40288c9206c1` on
`codex/accelerator-blast-radius-v2`.

## Method

The review compared the baseline-to-current diff with:

- The execution-owner registry and subscription lifetime in `mm/owner.c`.
- The x86 reclaim records, owner snapshots, registration, release, and TLB
  filters in `arch/x86/kernel/accel.c` and `arch/x86/mm/tlb.c`.
- The `mmu_gather` data-page batch, intermediate-drain, final-drain, and
  page-table exclusion paths in `mm/mmu_gather.c`.
- The rmap batch publication and vmscan allocation/cancellation paths in
  `mm/rmap.c` and `mm/vmscan.c`.
- The per-CPU workqueue bracketing in LRU drain, SLUB flush, and vmalloc
  purge paths.
- The disabled-config declarations and inline fallbacks in
  `arch/x86/include/asm/cpu_accel.h` and `include/linux/smp.h`.

## Per-file verdicts

| File | Verdict | Review result |
|---|---|---|
| `mm/Makefile` | pass | `owner.o` is built only when `CONFIG_CPU_ACCEL_CORE` is enabled. No production MM object is added when the accelerator core is disabled. |
| `mm/folio.c` | pass | LRU drain reserves each target CPU before queueing its per-CPU work, requests an active owner to stop after queueing, and releases the reservation only after `flush_work()` completes. Only CPUs with pending work are reserved. |
| `mm/init-mm.c` | pass | The initial address space supplies the execution-owner lock and list required by the generic `mm_struct` representation. It registers no owner and changes no reclaim behavior by itself. |
| `mm/internal.h` | pass | The reclaim-completion representation and rmap hooks are confined to x86 declarations. The completion owns folio references and work lifetime separately from the architecture acknowledgement record. |
| `mm/mmu_gather.c` | pass | Completion storage is reserved before page removal only while an owner is active. Eligible intermediate and final leaf data-page batches transfer to completion work; full flushes, page-table batches, delayed rmap, VM_IO/VM_PFN, and unshared-table paths stay synchronous. Pool exhaustion and bounded-record overflow fall back synchronously. Partial registration now cancels the completion and returns it only after attached owners reconcile. |
| `mm/rmap.c` | pass | A reclaim run first flushes any earlier task-local batch, then records each affected `mm` generation in the reserved completion. Writable batches, migration, and callers without a reserved completion use the original synchronous flush. A failed bounded registration flushes synchronously and lets the caller cancel the reserved object. |
| `mm/slub.c` | pass | Both per-CPU sheave flush paths track exactly the CPUs on which work was queued. Each reservation is released after the corresponding `flush_work()` returns, and disabled-config hooks are no-ops. |
| `mm/vmalloc.c` | pass | Vmap purge tracks queued node CPUs separately from offline nodes that use the local system workqueue. Each online-node reservation is released after its work completes. The NUMA user-allocation helper adds no owner interaction. |
| `mm/vmscan.c` | pass | Completion allocation is limited to active owners and mapped, clean, non-hugetlb folios without release work. Every failed-unmap, pin, dirty, release, lazyfree, and mapping-removal path flushes and cancels the completion before leaving the folio path. The broad pre-unmap anonymous-folio pin shortcut is removed; the original post-unmap pin check remains. |

## Invariants verified

- **State transitions and lifetime:** completion storage is reserved before
  PTE removal. A successfully deferred folio or `mmu_gather` page batch is
  published to its completion only after reclaim has selected the disposition,
  and owner references keep that storage alive until local TLB reconciliation.
- **Lock ordering and deadlock avoidance:** subscription callbacks do not take
  MM locks. `mmu_gather` brackets address-space updates with the owner-update
  interlock, and per-CPU sync-wait hooks only publish a nonblocking stop
  request before waiting for ordinary work completion.
- **Error and rollback:** pool exhaustion, record overflow, failed unmap,
  writable or dirty PTEs, DMA pins, release work, and mapping-removal failure
  all use the synchronous path. Every vmscan exit path either transfers the
  folio to a ready completion or flushes and cancels the reserved completion.
- **Hotplug and owner exit:** active owners are excluded from per-CPU work
  execution by a generation-scoped stop request. Owner exit drains attached
  subscriptions after local TLB reconciliation; inactive state alone is not an
  acknowledgement.
- **No unsafe owned-CPU execution:** LRU, SLUB, and vmalloc work is queued only
  after reserving the target CPU and is completed before the reservation is
  released. Reclaim filtering changes only registered, generation-matching
  ring-3 owners.
- **No silent semantic change:** with `CONFIG_CPU_ACCEL_CORE=n`, the sync-wait
  hooks and reclaim predicates are no-ops and completion pools are not
  initialized. With the core enabled, changed behavior is limited to active
  accelerator owners and their reserved completion lifetimes.

## Findings

Two defects were found and fixed during this audit:

- Bounded reclaim registration can attach some owners before returning
  `-EOVERFLOW`. The old `mmu_gather` reset path immediately called
  `x86_cpu_accel_reclaim_release()`, which could race owners still holding
  subscription references. The reset path now detects attached subscriptions,
  asserts that the failed flush was not marked as deferred, cancels the
  completion, and lets its worker return the slot only after owners reconcile.
- The memory design document incorrectly said no completion-specific stop
  request had been issued on partial overflow and still described
  `mmu_gather` deferral as future work. It now records the already-delivered
  generation-scoped requests, synchronous partial-set fallback, and current
  leaf-batch transfer and cancellation rules.

The semantic inventory also omitted `kernel/irq/internals.h` after the IRQ
rollback-filter change. That row and the IRQ audit have been corrected in this
tree; the inventory again contains exactly 89 semantic-remainder files.

No unresolved MM semantic defect remains.

## Evidence

- The complete baseline-to-current diff for all nine MM files and their x86
  support paths was reviewed.
- `scripts/checkpatch.pl --strict --no-signoff --no-tree` reports no errors,
  warnings, or checks for the touched source diff.
- An x86-64 build with `CONFIG_CPU_ACCEL_CORE=y` compiles
  `arch/x86/kernel/accel.o`, `mm/mmu_gather.o`, and `mm/vmscan.o`.
- An x86-64 build with `CONFIG_CPU_ACCEL_CORE=n` compiles `mm/mmu_gather.o`,
  `mm/vmscan.o`, and `arch/x86/mm/tlb.o`.
- `make SPHINXDIRS=accel htmldocs` and `git diff --check` pass.
- Simon is currently running the review-baseline kernel
  `7.3.0-rc3-accel-tlbfix-00508-g40288c9206c1-dirty`, not this tree. No
  current-tree `--reclaim-only`, `--mmu-gather-only`, or
  `--mmu-gather-final-only` runtime result is claimed.

## Status

The MMU, TLB, rmap, reclaim, vmalloc, and SLUB semantic class is **pass** for
source review and both focused configuration builds. Simon runtime validation
of the current tree remains a separate Phase 10 regression and stress gate;
it requires a new deployment because the running kernel predates this branch.
