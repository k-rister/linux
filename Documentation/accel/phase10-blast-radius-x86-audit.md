# Phase 10 blast-radius x86 semantic audit

## Scope

This note records the baseline-to-current review of the 25 `arch/x86` files in
`Documentation/accel/phase10-blast-radius-semantic-inventory.tsv`.  The review
baseline is `40288c9206c1` on `codex/accelerator-blast-radius-v2`.

The audit covers ownership and direct entry, vector and IDT setup, IPI
filtering, text patching, maintenance rendezvous, TLB and MMU coordination,
debugger interaction, and the static MM initializer required by execution-owner
bookkeeping.

## Method

The review used:

- The complete `git diff 40288c9206c1..HEAD` for the 25 files.
- The owner-state, maintenance-window, IPI, TLB, and stop helpers in
  `arch/x86/include/asm/cpu_accel.h` and `arch/x86/kernel/accel.c`.
- The configuration dependencies in `drivers/cpu_accel/Kconfig`.
- The semantic inventory generator and a byte-for-byte regeneration check.
- `git diff --check`.

The x86 class is closed only for semantic review.  Full both-configuration
build evidence remains tracked separately by the Phase 10 build checklist.

## Per-file verdicts

| File | Verdict | Review result |
|---|---|---|
| `arch/x86/entry/entry_fred.c` | pass | The FRED system-vector table now installs `CPU_ACCEL_VECTOR` only when `CONFIG_CPU_ACCEL_CORE` is enabled. The missing guard was a real disabled-config defect and is remediated in this tree. |
| `arch/x86/include/asm/idtentry.h` | pass | The `sysvec_cpu_accel` declaration is config-gated and matches the FRED/IDT table installation. |
| `arch/x86/include/asm/irq_vectors.h` | pass | `CPU_ACCEL_VECTOR` and the `FIRST_SYSTEM_VECTOR` adjustment are config-gated; vector `0xea` does not overlap the posted-MSI vector. |
| `arch/x86/include/asm/tlb.h` | pass | MMU gather uses the completion path only when both completion fields are present; all other gathers retain the ordinary flush boundary. |
| `arch/x86/include/asm/tlbflush.h` | pass | New completion and reclaim fields are initialized by `init_flush_tlb_info()`; unmap generation notification occurs after `inc_mm_tlb_gen()` and before the batch is marked unmapped. |
| `arch/x86/kernel/Makefile` | pass | `accel.o` is linked only under `CONFIG_CPU_ACCEL_CORE`. |
| `arch/x86/kernel/alternative.c` | pass | Text-poke paths use the owner-aware text-mutex wrapper; user-image owners are correctly exempt from `sync_core()` while admission remains closed by the wrapper. |
| `arch/x86/kernel/apic/ipi.c` | pass | Reschedule and call-function IPIs are deferred only for an active owner; owner exit replays deferred IPIs, preventing loss. |
| `arch/x86/kernel/callthunks.c` | pass | Builtin and module call-thunk patching preserve the original `text_mutex` critical sections through the owner-aware wrapper. |
| `arch/x86/kernel/cpu/cacheinfo.c` | pass | CPU-online failure now propagates `stop_machine_from_inactive_cpu()` and removes the CPU from the cache-info mask, preserving rollback. |
| `arch/x86/kernel/cpu/microcode/core.c` | pass | Late microcode reload drains owners before stop-machine and text patching, and releases the maintenance window after `cpus_read_unlock()`. |
| `arch/x86/kernel/cpu/mtrr/mtrr.c` | pass | MTRR add/delete hold a terminate-mode maintenance window across the existing CPU-hotplug and MTRR mutex regions. |
| `arch/x86/kernel/ftrace.c` | pass | Ftrace prepare/finish and trampoline patching retain the original text-mutex boundaries through the owner-aware wrapper. |
| `arch/x86/kernel/idt.c` | pass | The IDT entry is installed only under `CONFIG_CPU_ACCEL_CORE` and matches the gated declaration and vector definition. |
| `arch/x86/kernel/jump_label.c` | pass | Direct, queued, and apply-time jump-label transforms retain the original text-mutex critical sections through the owner-aware wrapper. |
| `arch/x86/kernel/kgdb.c` | pass | Hardware-breakpoint reservation and release now maintain the accelerator KGDB breakpoint count on the same success/failure boundaries. |
| `arch/x86/kernel/module.c` | pass | Module relocation preserves the existing early/non-early split and text-mutex boundary; the owner-aware wrapper is used only on the non-early path. |
| `arch/x86/kernel/reboot.c` | pass | Kexec shutdown intentionally opens a terminal maintenance window and does not release it before the one-way transfer to the next kernel. |
| `arch/x86/kernel/smp.c` | pass | Call-function handlers defer only while an owner is active; the sender and exit paths pair correctly, so a late IPI is either handled normally or deferred and replayed. |
| `arch/x86/kernel/static_call.c` | pass | Static-call transform and fixup retain the original `text_mutex` critical sections through the owner-aware wrapper. |
| `arch/x86/kernel/tboot.c` | pass | The static `tboot_mm` initializer supplies the execution-owner lock and list required by the generic MM owner representation. |
| `arch/x86/mm/pat/set_memory.c` | pass | CPA flushes reserve owner targets before synchronous dispatch. Dropping `pgd_lock` before the post-split flush avoids an owner-exit lock inversion while `cpa_lock` continues to serialize CPA operations. |
| `arch/x86/mm/tlb.c` | pass | Global-ASID, broadcast, kernel, mm-range, and batched-unmap flushes either use a safe IPI fallback or reserve all online CPUs; filtering is generation- and completion-aware. |
| `arch/x86/net/bpf_jit_comp.c` | pass | BPF text poking retains its compare-and-patch semantics and text-mutex boundary through the owner-aware wrapper. |
| `arch/x86/virt/vmx/tdx/seamldr.c` | pass | P-SEAMLDR installation holds a terminate-mode maintenance window across `cpus_read_lock()` and stop-machine, matching the all-CPU rendezvous requirement. |

## Invariants verified

- **State transitions and lifetime:** owner entry, direct entry, stop request,
  exit, TLB reconciliation, and active-count publication are ordered by the
  request lock, release/acquire atomics, and the owner-exit finish sequence.
- **Lock ordering:** text mutexes open a maintenance window before locking
  `text_mutex`; nested maintenance callers join an existing window rather than
  blocking behind its owner.  TLB flush reservations are released after remote
  dispatch completes.
- **Error and rollback:** owner admission failure leaves the CPU unused;
  cache-online failure clears the cache-info mask; KGDB counts are changed only
  on successful reserve or release; reclaim overflow falls back to the ordinary
  synchronous flush.
- **Hotplug and owner exit:** maintenance windows block new admission and
  request existing owners to exit.  Deferred reschedule and call-function IPIs
  are replayed by owner exit, so no wakeup is lost.
- **No unsafe owned-CPU execution:** all-CPU rendezvous paths use terminate
  mode, except text patching, which may skip only ring-3 user-image owners that
  are not executing patched kernel text.
- **No silent semantic change:** disabled-config stubs are no-ops, ordinary
  non-owner paths retain their original completion boundaries, and owner-aware
  filtering changes behavior only while an accelerator owner is active.

## Finding and remediation

The FRED system-vector table originally referenced `CPU_ACCEL_VECTOR` even
when `CONFIG_CPU_ACCEL_CORE=n`.  The working tree now wraps that entry in
`#ifdef CONFIG_CPU_ACCEL_CORE`, matching the IDT and vector-header guards.
No other x86 semantic defect was identified.

## Evidence

- `tools/cpu_accel/generate-blast-radius-semantic-inventory.sh` regenerated
  `Documentation/accel/phase10-blast-radius-semantic-inventory.tsv` exactly.
- The inventory contains 25 `arch/x86` rows; all 25 now have `pass` verdicts.
- `git diff --check` passed on the audited working tree.

## Status

The x86 semantic class is **pass**.  The FRED disabled-config defect is
remediated, and the 25 x86 inventory rows are closed with per-file evidence.
Build-matrix and runtime-stress evidence remain governed by their separate
Phase 10 checklist items.
