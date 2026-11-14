# Phase 10 blast-radius debug/tracing audit

## Scope

This note records the semantic review of the debug, tracing, kprobe, and
ring-buffer files in
`Documentation/accel/phase10-blast-radius-semantic-inventory.tsv`:

- `include/linux/vmalloc.h`
- `kernel/debug/debug_core.c`
- `kernel/kprobes.c`
- `kernel/trace/ring_buffer.c`

The review baseline is `40288c9206c1` on
`codex/accelerator-blast-radius-v2`.

The x86 members of the wider debug/tracing class—`arch/x86/kernel/ftrace.c`,
`arch/x86/kernel/jump_label.c`, `arch/x86/kernel/alternative.c`,
`arch/x86/kernel/cpu/cacheinfo.c`, and
`arch/x86/kernel/cpu/mtrr/mtrr.c`—were already reviewed in
`Documentation/accel/phase10-blast-radius-x86-audit.md` and have `pass`
verdicts. This note closes the four remaining generic files in that class.

## Method

The review compared the baseline-to-current diff with:

- The public maintenance, KGDB breakpoint, and text-mutex contracts in
  `arch/x86/include/asm/cpu_accel.h`.
- The ownership admission, maintenance-window, and sync-wait implementations
  in `arch/x86/kernel/accel.c`.
- KGDB breakpoint activation, deactivation, removal, debugger entry, CPU
  switching, and resume paths in `kernel/debug/debug_core.c`.
- Every `guard(text_mutex)()` call site in `kernel/kprobes.c`, including the
  optimizer, arm/disarm, aggregate registration, and initial registration
  paths.
- The all-CPU and single-CPU ring-buffer resize paths in
  `kernel/trace/ring_buffer.c`.
- The NUMA-specific user vmalloc implementation in `mm/vmalloc.c` and its
  accelerator-driver call site.

## Per-file verdicts

| File | Verdict | Review result |
|---|---|---|
| `include/linux/vmalloc.h` | pass | The node-specific user vmalloc declaration and profiling macro are additive. No existing vmalloc API changes, and the accelerator driver is the sole current caller. |
| `kernel/debug/debug_core.c` | pass | Debugger entry attempts a nonblocking accelerator maintenance window only after KGDB master selection and before CPU roundup. Failure refuses the session without leaking the window. Successful acquisition blocks new owners until debugger resume, including CPU-switch paths. Software-breakpoint accounting is incremented only after successful installation and decremented only after successful removal, with failed removals preserving state and accounting. |
| `kernel/kprobes.c` | pass | The x86 text-mutex guard opens a maintenance window with `STOP_TERMINATE | STOP_SKIP_OK`, then locks `text_mutex`; unlock reverses that order. Every changed call site is in a sleepable registration, optimization, arm, or disarm path already protected by `cpus_read_lock()` and, where applicable, `kprobe_mutex` or `jump_label_lock`. Non-x86 builds retain the original mutex behavior. |
| `kernel/trace/ring_buffer.c` | pass | Remote ring-buffer resize work brackets `schedule_work_on()` with `arch_smp_sync_wait_begin()` and `arch_smp_sync_wait_stop()`, then calls `arch_smp_sync_wait_end()` exactly once after the uninterruptible completion wait. The all-CPU path tracks exactly the CPUs for which remote work was queued, and no error path lies between queueing and the corresponding completion wait. |

## Invariants verified

- **State transitions and lifetime:** the KGDB maintenance window is acquired
  once per debugger entry and released on both normal return and the
  slave-return path. Breakpoint ownership accounting follows the
  `BP_SET`/`BP_ACTIVE` state machine. Ring-buffer sync-wait references are
  paired by the CPU mask or by the single-CPU control flow.
- **Lock ordering and deadlock avoidance:** KGDB admission takes only the
  accelerator ownership lock while interrupts are disabled. Kprobe text
  modification opens the accelerator maintenance window before taking
  `text_mutex` and releases it after unlocking, preserving the existing
  `cpus_read_lock()`/`jump_label_lock()` ordering. Ring-buffer sync-wait stop
  callbacks only publish a nonblocking owner-exit request.
- **Error and rollback:** failed breakpoint installation or removal preserves
  the prior breakpoint state and accelerator accounting. A failed KGDB
  maintenance-window attempt resumes the system without holding the window.
  Ring-buffer resize failures occur before remote work is queued or after the
  paired completion wait.
- **Hotplug and owner exit:** KGDB refuses entry while any accelerator owner
  is active and blocks new owners while the debugger or persistent
  software breakpoints are installed. Kprobe maintenance asks non-ring-3
  owners to leave and permits ring-3 owners that cannot execute the patched
  text. Ring-buffer resize uses the generic per-CPU sync-wait contract.
- **No unsafe owned-CPU execution:** debugger roundup, kprobe text mutation,
  and remote ring-buffer work do not execute on an accelerator-owned CPU.
  Owner-exit requests are generation-scoped and nonblocking.
- **No silent semantic change:** with `CONFIG_CPU_ACCEL_CORE=n`, KGDB falls
  back to unchanged admission, kprobes use the original `text_mutex`
  behavior, and ring-buffer sync-wait hooks are architecture no-ops. The
  vmalloc helper is additive and does not alter existing allocation paths.

## Findings

No unresolved debug/tracing semantic defect remains.

The first core-disabled validation attempt used the existing Phase 10
core-disabled configuration, which had `CONFIG_KGDB=n`; that configuration
cannot meaningfully compile `kernel/debug/debug_core.o`. A separate
configuration with `CONFIG_CPU_ACCEL=n`, `CONFIG_KGDB=y`,
`CONFIG_KPROBES=y`, and `CONFIG_TRACING=y` was therefore generated and used
for the focused fallback build.

## Evidence

- The complete baseline-to-current diff for all four files and their x86
  support paths was reviewed.
- `scripts/checkpatch.pl --strict --no-signoff --no-tree` reports no errors,
  warnings, or checks for the four-file diff.
- An x86-64 build with `CONFIG_CPU_ACCEL_CORE=y` compiles
  `kernel/debug/debug_core.o`, `kernel/kprobes.o`,
  `kernel/trace/ring_buffer.o`, `mm/vmalloc.o`, and
  `drivers/cpu_accel/cpu_accel.o`.
- An x86-64 build with `CONFIG_CPU_ACCEL=n`, `CONFIG_KGDB=y`,
  `CONFIG_KPROBES=y`, and `CONFIG_TRACING=y` compiles
  `kernel/debug/debug_core.o`, `kernel/kprobes.o`,
  `kernel/trace/ring_buffer.o`, and `mm/vmalloc.o`.

## Status

The debug, tracing, kprobe, and ring-buffer semantic class is **pass** for
source review and both focused configuration builds.
